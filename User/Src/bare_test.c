#include "bare_test.h"

#if (USE_FREERTOS == 0)

/* ================ 模块测试 ================ */

/* 测试项由 BuildConfig.h 的 BM_TEST_MODULE 宏在编译期选择, 此处不再定义同名变量 */

/* ================ OLED 测试 ================ */
static void OLED_BareMetal_Test(void)
{
    OLED_Init();
    OLED_Clear();

    OLED_Write_String(0, 0, "OLED TEST OK", &Font_16);

    uint32_t cnt = 0;
    char buf[16];

    while (1)
    {
        cnt++;
        snprintf(buf, sizeof(buf), "CNT=%lu  ", (unsigned long)cnt);
        OLED_Write_String(0, 24, buf, &Font_16);
        delay_ms(500);
    }
}

/* ================ 光敏传感器测试 ================ */

#define LIGHT_THRESHOLD_ON 800   // 开灯阈值 (光线变暗)
#define LIGHT_THRESHOLD_OFF 1000 // 关灯阈值 (光线变亮)

static void Light_Sensor_BareMetal_Test(void)
{
    OLED_Init();
    OLED_Clear();
    Light_Sensor_Init();

    OLED_Write_String(0, 0, "SENSOR TEST OK", &Font_16);

    while (1)
    {
        char buf[16];
#if AO_DO_SWITCH == 1
        uint16_t adc_val = Light_Sensor_Read();
        snprintf(buf, sizeof(buf), "ADC_VAL=%d", adc_val);
#else
        snprintf(buf, sizeof(buf), "DO_STATE=%d", Light_Sensor_DO_State);
        // snprintf(buf, sizeof(buf), "12:20");
#endif
        OLED_Write_String(0, Font_16.size, buf, &Font_16);
        delay_ms(500);
    }
}

/* ================ DS1302 外部 RTC 测试 ================
 * 观察点:
 *   - 复位后时间是否从上次继续(而不是回到基准值) → 验证 Init 不再清零秒寄存器
 *   - 断电(VCC)后重新上电时间是否连续           → 验证模块电池保持
 *   - 秒值连续 DS1302_TEST_HALT_LIMIT 次不变     → 提示晶振停振/电池欠压
 *   - 拔掉模块后是否只报错不卡死                 → 验证失败路径
 * ==================================================== */

/* 1 = 每次上电都写入基准时间(会覆盖 RTC 现有时间, 且会把 CH 清零以启动振荡器)
 * 0 = 仅在读取失败或年份早于 DS1302_TEST_MIN_YEAR 时写入
 * 说明: 若读取到的秒寄存器 CH 位=1(时钟停走), 置 1 强制写入一次以启动时钟;
 *       验证断电保持时必须置 0, 否则每次上电都会把时间重写为基准值。 */
#define DS1302_TEST_FORCE_SET 0
/* 判定 "RTC 未初始化" 的年份下限 */
#define DS1302_TEST_MIN_YEAR 2020
/* 基准时间(星期: 1=周一 ... 7=周日), 仅在需要写入时使用。
 * 建议不要设为 23:59, 否则跨天时回读校验会失败。 */
#define DS1302_TEST_YEAR 2026
#define DS1302_TEST_MONTH 9
#define DS1302_TEST_DAY 13
#define DS1302_TEST_WEEK 7 /* 2026-09-13 为周日 */
#define DS1302_TEST_HOUR 12
#define DS1302_TEST_MINUTE 0
#define DS1302_TEST_SECOND 0
/* 串口日志周期(秒) */
#define DS1302_TEST_LOG_PERIOD 10
/* 连续读到相同秒多少次判定为 "走时停滞" */
#define DS1302_TEST_HALT_LIMIT 3

/**
 * @brief 写入基准时间并做回读校验(写入接口无返回值校验, 只能靠回读确认)
 * @return true 回读结果与基准时间一致(允许跨 1 分钟)
 */
static bool DS1302_BareMetal_SetDefault(const DS1302_Time_t *base)
{
    DS1302_Time_t chk;

    delay_ms(10); /* 等 DS1302 完成一次秒寄存器更新 */

    if (!DS1302_ReadTime(&chk))
    {
        printf("[RTC] SET VERIFY FAIL: read back failed\r\n");
        return false;
    }

    /* 用"当日分钟数"比较, 允许写入耗时造成的 1 分钟进位 */
    int base_min = (int)base->hour * 60 + (int)base->min;
    int chk_min = (int)chk.hour * 60 + (int)chk.min;
    int dm = chk_min - base_min;

    bool ok = (chk.year == base->year) && (chk.month == base->month) &&
              (chk.day == base->day) && (dm >= 0) && (dm <= 1);

    printf("[RTC] SET VERIFY %s: %04u-%02u-%02u %02u:%02u:%02u W%u\r\n",
           ok ? "OK" : "FAIL",
           (unsigned)chk.year, (unsigned)chk.month, (unsigned)chk.day,
           (unsigned)chk.hour, (unsigned)chk.min, (unsigned)chk.sec, (unsigned)chk.week);

    return ok;
}

static void DS1302_BareMetal_Test(void)
{
    const DS1302_Time_t base = {
        .sec = DS1302_TEST_SECOND,
        .min = DS1302_TEST_MINUTE,
        .hour = DS1302_TEST_HOUR,
        .day = DS1302_TEST_DAY,
        .month = DS1302_TEST_MONTH,
        .year = DS1302_TEST_YEAR,
        .week = DS1302_TEST_WEEK,
    };

    DS1302_Time_t t;
    char line[24];
    uint32_t rd_ok = 0;
    uint32_t rd_err = 0;
    uint32_t halt_cnt = 0;
    uint32_t log_cnt = 0;
    int last_sec = -1;
    bool have_valid = false;
    bool halt_reported = false;

    OLED_Init();
    OLED_Clear();

    DS1302_Init();
    printf("[RTC] DS1302 init done (pins: PB0=RST PB1=IO PB2=CLK)\r\n");

    /* ---- 首次读取, 必要时写入基准时间 ---- */
    bool first_ok = DS1302_ReadTime(&t);
    bool uninit = (!first_ok) || (t.year < DS1302_TEST_MIN_YEAR);

    if ((DS1302_TEST_FORCE_SET != 0) || uninit)
    {
        printf("[RTC] %s, write default time %04u-%02u-%02u %02u:%02u:%02u W%u\r\n",
               (DS1302_TEST_FORCE_SET != 0) ? "force set" : (first_ok ? "year too old" : "read failed"),
               (unsigned)base.year, (unsigned)base.month, (unsigned)base.day,
               (unsigned)base.hour, (unsigned)base.min, (unsigned)base.sec, (unsigned)base.week);

        if (DS1302_SetTime(&base))
            have_valid = DS1302_BareMetal_SetDefault(&base);
        else
            printf("[RTC] set time FAILED\r\n");
    }
    else
    {
        have_valid = true;
        printf("[RTC] RTC already running, keep existing time: %04u-%02u-%02u %02u:%02u:%02u W%u\r\n",
               (unsigned)t.year, (unsigned)t.month, (unsigned)t.day,
               (unsigned)t.hour, (unsigned)t.min, (unsigned)t.sec, (unsigned)t.week);
    }

    /* ---- 每秒读取 + 刷新显示 ---- */
    for (;;)
    {
        bool ok = DS1302_ReadTime(&t);

        if (ok)
        {
            rd_ok++;
            have_valid = true;

            /* 秒值是否停滞(ReadTime 屏蔽了 CH 位, 只能这样间接判断走时) */
            if (last_sec == (int)t.sec)
            {
                if (halt_cnt < 0xFFFFU)
                    halt_cnt++;
            }
            else
            {
                halt_cnt = 0;
            }
            last_sec = (int)t.sec;
        }
        else
        {
            rd_err++;
        }

        /* 停振告警: 出现时立即打印一次, 恢复正常后允许再次告警 */
        if (halt_cnt >= DS1302_TEST_HALT_LIMIT)
        {
            if (!halt_reported)
            {
                halt_reported = true;
                printf("[RTC] WARN: second not advancing (%lu reads), clock may be halted\r\n",
                       (unsigned long)halt_cnt);
            }
        }
        else if (halt_cnt == 0U)
        {
            halt_reported = false;
        }

        /* 首次 + 每 DS1302_TEST_LOG_PERIOD 秒打印一行, 便于长时间观察 */
        log_cnt++;
        if ((log_cnt == 1U) || ((log_cnt % DS1302_TEST_LOG_PERIOD) == 0U))
        {
            if (ok)
            {
                printf("[RTC] read OK: %04u-%02u-%02u %02u:%02u:%02u W%u (RD=%lu ER=%lu)\r\n",
                       (unsigned)t.year, (unsigned)t.month, (unsigned)t.day,
                       (unsigned)t.hour, (unsigned)t.min, (unsigned)t.sec, (unsigned)t.week,
                       (unsigned long)rd_ok, (unsigned long)rd_err);
            }
        }

        /* ---- OLED 刷新(定长字符串, 无残影) ---- */
        if (have_valid)
        {
            snprintf(line, sizeof(line), "%04u-%02u-%02u W%u",
                     (unsigned)t.year, (unsigned)t.month, (unsigned)t.day, (unsigned)t.week);
            OLED_Write_String(0, 0, line, &Font_16);

            snprintf(line, sizeof(line), "%02u:%02u:%02u",
                     (unsigned)t.hour, (unsigned)t.min, (unsigned)t.sec);
        }
        else
        {
            OLED_Write_String(0, 0, "-- no valid data --", &Font_16);
            snprintf(line, sizeof(line), "--:--:--");
        }
        OLED_Write_String(0, 16, line, &Font_32);

        if (halt_cnt >= DS1302_TEST_HALT_LIMIT)
            snprintf(line, sizeof(line), "HALT ER=%lu", (unsigned long)rd_err);
        else if (rd_err != 0U)
            snprintf(line, sizeof(line), "ER=%lu", (unsigned long)rd_err);
        else
            snprintf(line, sizeof(line), "RD=%lu OK", (unsigned long)rd_ok);
        OLED_Write_String(0, 48, line, &Font_16);

        delay_ms(1000);
    }
}

void BareMetal_Module_Test(void)
{
    /* 裸机分支的 main() 不会调用 Board_Init(), USART2 需要在这里初始化,
     * 否则 printf 没有任何输出(调度器未运行时 fputc 自动退化为轮询发送) */
    Usart2_Debug_Init();
    printf("\r\n[BARE] bare-metal module test start, build %s %s\r\n", __DATE__, __TIME__);

    switch (BM_TEST_MODULE)
    {
    case BM_TEST_MODULE_OLED:
        OLED_BareMetal_Test();
        break;
    case BM_TEST_MODULE_LIGHT:
        Light_Sensor_BareMetal_Test();
        break;
    case BM_TEST_MODULE_DS1302:
        DS1302_BareMetal_Test();
        break;
    default:
        while (1)
            ;
    }
}

#endif /* USE_FREERTOS == 0 */
