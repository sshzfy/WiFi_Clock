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
    DS1302_Time_t chk; // 用于存储回读时间

    delay_ms(10); // 等 DS1302 完成一次秒寄存器更新

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
    printf("[RTC] DS1302 init done (pins: PE7=RST PE8=IO PE9=CLK)\r\n");

    /* 首次读取, 必要时写入基准时间 */
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

    /* 每秒读取 + 刷新显示 */
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

        /*  OLED 刷新(定长字符串, 无残影)  */
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

/* ================ W25Q64 / littlefs 测试公共工具 ================
 * 说明: printf 内容一律使用 ASCII。源文件是 UTF-8, 而工程里的中文字模是
 * GB2312, 串口日志混用两种编码会在终端里显示为乱码, 不利于定位问题。
 * ============================================================ */

/* 生成可复现的测试数据: 同一 seed 必然产生同一段字节 */
static void Test_FillPattern(uint8_t *p, uint32_t n, uint32_t seed)
{
    for (uint32_t i = 0; i < n; i++)
        p[i] = (uint8_t)((i * 31U) + (i >> 8) + seed);
}

/* FNV-1a 校验和, 用于大块数据比对时避免占用大缓冲 */
static uint32_t Test_Checksum(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261U;
    for (uint32_t i = 0; i < n; i++)
    {
        h ^= p[i];
        h *= 16777619U;
    }
    return h;
}

/* ================ W25Q64 SPI Flash 裸驱动测试 ================
 * 观察点:
 *   - T1/T2  读 ID: 若为 0x000000/0xFFFFFF, 说明 SPI 本身不通(接线/片选/时钟/供电)
 *   - T5/T6  覆盖两个曾经出错的地址边界:
 *              T5 起点 0x0F0 需要跨 256 字节页边界(原代码会一次写满 256 字节并回卷到页首)
 *              T6 起点 0x200 的 bit8=1, 原页内剩余量算法在此得到 0 而**死循环**
 *   - T8     512 字节跨 4KB 扇区边界
 *   - T9     整扇区 4KB 连续编程 + 读回校验(使用独立扇区)
 * 测试区固定在**最后 4 个 4KB 扇区**(0x7FC000 起), 不会破坏 littlefs 已写入的数据。
 * 各用例地址互不重叠 —— NOR Flash 只能把 1 写成 0, 同一区域重复编程会造成假失败。
 * ============================================================ */

/* 测试用共享缓冲: W25Q64 与 LFS 两个测试项在编译期二选一(switch 会被常量折叠),
 * 共用同一组缓冲可省下约 11KB RAM —— F407VE 只有 128KB SRAM, 而裸机分支因为
 * Usart.c 直接调用了 FreeRTOS API, 仍然会链接 heap_4 的 95KB 堆。 */
#define TEST_BUF_SIZE 4096U
static uint8_t s_test_buf[TEST_BUF_SIZE];
static uint8_t s_test_rd[TEST_BUF_SIZE];

#define W25Q64_TEST_BASE (W25Q64_FLASH_SIZE - 4U * W25Q64_SECTOR_SIZE)

/* 从 addr 读 n 字节与 exp 比较, 返回不一致的字节个数 */
static uint32_t W25Q64_CountMismatch(uint32_t addr, const uint8_t *exp, uint32_t n)
{
    uint32_t bad = 0;

    W25Q64_Read(addr, s_test_rd, n);
    for (uint32_t i = 0; i < n; i++)
    {
        if (s_test_rd[i] != exp[i])
            bad++;
    }

    return bad;
}

/* 返回区域中非 0xFF 的字节个数(擦除态应为 0) */
static uint32_t W25Q64_CountNotFF(uint32_t addr, uint32_t n)
{
    uint32_t bad = 0;

    W25Q64_Read(addr, s_test_rd, n);
    for (uint32_t i = 0; i < n; i++)
    {
        if (s_test_rd[i] != 0xFFU)
            bad++;
    }

    return bad;
}

static void W25Q64_Test_RunRound(uint32_t round, uint32_t *out_pass, uint32_t *out_fail)
{
    uint32_t pass = 0;
    uint32_t fail = 0;
    uint32_t bad;
    char detail[72];

#define W25Q_CHECK(name, cond, ...)                           \
    do                                                        \
    {                                                         \
        snprintf(detail, sizeof(detail), __VA_ARGS__);        \
        if (cond)                                             \
        {                                                     \
            pass++;                                           \
            printf("[W25Q] %-26s OK   %s\r\n", name, detail); \
        }                                                     \
        else                                                  \
        {                                                     \
            fail++;                                           \
            printf("[W25Q] %-26s FAIL %s\r\n", name, detail); \
        }                                                     \
    } while (0)

    printf("\r\n[W25Q] ===== round %lu, test base = 0x%06lX =====\r\n",
           (unsigned long)round, (unsigned long)W25Q64_TEST_BASE);

    /*  T1: JEDEC ID (9Fh), W25Q64 = EF 40 17  */
    uint32_t jedec = W25Q64_ReadJedecId();
    W25Q_CHECK("T1 JEDEC ID (9Fh)", jedec == 0xEF4017U,
               "id = 0x%06lX, expect 0xEF4017", (unsigned long)jedec);

    /*  T2: READ_ID (90h), W25Q64 = EF 16  */
    uint16_t rid = W25Q64_ReadId();
    W25Q_CHECK("T2 READ_ID (90h)", rid == 0xEF16U,
               "id = 0x%04X, expect 0xEF16", (unsigned)rid);

    /*  T3: 擦除基站扇区后应全为 0xFF  */
    bool er = W25Q64_SectorErase(W25Q64_TEST_BASE);
    bad = W25Q64_CountNotFF(W25Q64_TEST_BASE, W25Q64_SECTOR_SIZE);
    W25Q_CHECK("T3 sector erase", er && (bad == 0U),
               "erase=%d, notFF = %lu", (int)er, (unsigned long)bad);

    /*  T4: 扇区起始处写 16 字节  */
    Test_FillPattern(s_test_buf, 16, 0x11);
    bool wr = W25Q64_PageProgram(W25Q64_TEST_BASE + 0x000U, s_test_buf, 16);
    bad = W25Q64_CountMismatch(W25Q64_TEST_BASE + 0x000U, s_test_buf, 16);
    W25Q_CHECK("T4 program @0x000 (16B)", wr && (bad == 0U),
               "prog=%d, mismatch = %lu", (int)wr, (unsigned long)bad);

    /*  T5: 从 0xF0 写 32 字节, 跨越 256 字节页边界  */
    Test_FillPattern(s_test_buf, 32, 0x22);
    wr = W25Q64_PageProgram(W25Q64_TEST_BASE + 0x0F0U, s_test_buf, 32);
    bad = W25Q64_CountMismatch(W25Q64_TEST_BASE + 0x0F0U, s_test_buf, 32);
    W25Q_CHECK("T5 page-cross @0x0F0 (32B)", wr && (bad == 0U),
               "prog=%d, mismatch = %lu", (int)wr, (unsigned long)bad);

    /*  T6: 从 0x200 写 16 字节(bit8=1, 原死循环触发点)  */
    Test_FillPattern(s_test_buf, 16, 0x33);
    wr = W25Q64_PageProgram(W25Q64_TEST_BASE + 0x200U, s_test_buf, 16);
    bad = W25Q64_CountMismatch(W25Q64_TEST_BASE + 0x200U, s_test_buf, 16);
    W25Q_CHECK("T6 page @0x200 (16B)", wr && (bad == 0U),
               "prog=%d, mismatch = %lu", (int)wr, (unsigned long)bad);

    /*  T7: 从 0x2F0 连续写 256 字节, 跨越 2 个页边界  */
    Test_FillPattern(s_test_buf, 256, 0x44);
    wr = W25Q64_PageProgram(W25Q64_TEST_BASE + 0x2F0U, s_test_buf, 256);
    bad = W25Q64_CountMismatch(W25Q64_TEST_BASE + 0x2F0U, s_test_buf, 256);
    W25Q_CHECK("T7 multi-page @0x2F0 (256B)", wr && (bad == 0U),
               "prog=%d, mismatch = %lu", (int)wr, (unsigned long)bad);

    /*  T8: 写 512 字节, 跨越 4KB 扇区边界(0xF00 -> 0x1100)  */
    er = W25Q64_SectorErase(W25Q64_TEST_BASE + W25Q64_SECTOR_SIZE);
    Test_FillPattern(s_test_buf, 512, 0x55);
    wr = W25Q64_PageProgram(W25Q64_TEST_BASE + 0x0F00U, s_test_buf, 512) && er;
    bad = W25Q64_CountMismatch(W25Q64_TEST_BASE + 0x0F00U, s_test_buf, 512);
    W25Q_CHECK("T8 sector-cross @0x0F00 (512B)", wr && (bad == 0U),
               "prog=%d, mismatch = %lu", (int)wr, (unsigned long)bad);

    /*  T9: 整扇区 4096 字节写入 + 读回校验和  */
    Test_FillPattern(s_test_buf, W25Q64_SECTOR_SIZE, 0x66);
    uint32_t sum_w = Test_Checksum(s_test_buf, W25Q64_SECTOR_SIZE);
    er = W25Q64_SectorErase(W25Q64_TEST_BASE + 2U * W25Q64_SECTOR_SIZE);
    wr = W25Q64_PageProgram(W25Q64_TEST_BASE + 2U * W25Q64_SECTOR_SIZE,
                            s_test_buf, W25Q64_SECTOR_SIZE) &&
         er;
    W25Q64_Read(W25Q64_TEST_BASE + 2U * W25Q64_SECTOR_SIZE, s_test_rd, W25Q64_SECTOR_SIZE);
    uint32_t sum_r = Test_Checksum(s_test_rd, W25Q64_SECTOR_SIZE);
    W25Q_CHECK("T9 full sector (4096B)", wr && (sum_w == sum_r),
               "sum_w = 0x%08lX, sum_r = 0x%08lX", (unsigned long)sum_w, (unsigned long)sum_r);

    printf("[W25Q] - round %lu: PASS %lu, FAIL %lu, busy-timeout %lu -\r\n",
           (unsigned long)round, (unsigned long)pass, (unsigned long)fail,
           (unsigned long)W25Q64_GetBusyTimeoutCount());

#undef W25Q_CHECK

    *out_pass = pass;
    *out_fail = fail;
}

static void W25Q64_BareMetal_Test(void)
{
    char line[24];
    uint32_t round = 0;
    uint32_t pass = 0;
    uint32_t fail = 0;

    OLED_Init();
    OLED_Clear();

    printf("[W25Q] W25Q64_Init() (CS=PA4, CLK=PA5, MISO=PA6, MOSI=PA7)\r\n");
    W25Q64_Init();
    printf("[W25Q] test region: last 4 sectors, base = 0x%06lX, chip = %lu KB\r\n",
           (unsigned long)W25Q64_TEST_BASE, (unsigned long)(W25Q64_FLASH_SIZE / 1024U));

    for (;;)
    {
        round++;
        W25Q64_Test_RunRound(round, &pass, &fail);

        /* OLED 汇总: 全部通过时只显示 PASS, 有失败时显示失败项数 */
        snprintf(line, sizeof(line), "W25Q64  %lu/%lu", (unsigned long)pass,
                 (unsigned long)(pass + fail));
        OLED_Write_String(0, 0, line, &Font_16);

        if (fail == 0U)
            snprintf(line, sizeof(line), "ALL PASS R%lu", (unsigned long)round);
        else
            snprintf(line, sizeof(line), "FAIL=%lu", (unsigned long)fail);
        OLED_Write_String(0, 16, line, &Font_16);

        snprintf(line, sizeof(line), "TMO=%lu", (unsigned long)W25Q64_GetBusyTimeoutCount());
        OLED_Write_String(0, 32, line, &Font_16);

        delay_ms(3000);
    }
}

/* ================ littlefs 文件系统测试 ================
 * 观察点:
 *   - 首次运行必然打印 "mount failed -> formatting"(芯片还不是 littlefs 格式)
 *   - **复位后再次运行**应直接 "init success", 且 T8 重新挂载后读回仍然正确
 *     → 这才证明数据真的落到 Flash, 而不是只在内存缓存里
 *   - 若每次上电都在格式化, 说明写入没有落盘(擦除粒度/片选/忙等待问题)
 * ============================================================ */

#define LFS_TEST_CHUNK 4096U
#define LFS_TEST_BIG_SIZE (64U * 1024U)
/* 用于图片用例的真实图片尺寸(Image_err: 40x40 RGB565) */
#define LFS_TEST_IMG_W 40U
#define LFS_TEST_IMG_H 40U
#define LFS_TEST_IMG_SIZE (LFS_TEST_IMG_W * LFS_TEST_IMG_H * 2U)

static struct lfs_info s_lfs_info;

/* 写文件(覆盖), 返回 0 成功 */
static int LFS_WriteFile(const char *path, const uint8_t *data, uint32_t size)
{
    lfs_file_t file;
    int err = lfs_file_open(&g_lfs, &file, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err != LFS_ERR_OK)
        return err;

    lfs_ssize_t n = lfs_file_write(&g_lfs, &file, data, size);
    if (n != (lfs_ssize_t)size)
    {
        lfs_file_close(&g_lfs, &file);
        return (n < 0) ? (int)n : LFS_ERR_IO;
    }

    return lfs_file_close(&g_lfs, &file);
}

/* 读文件, 返回实际读到的字节数, 负数为错误码 */
static int LFS_ReadFile(const char *path, uint8_t *data, uint32_t size)
{
    lfs_file_t file;
    int err = lfs_file_open(&g_lfs, &file, path, LFS_O_RDONLY);
    if (err != LFS_ERR_OK)
        return err;

    lfs_ssize_t n = lfs_file_read(&g_lfs, &file, data, size);
    lfs_file_close(&g_lfs, &file);

    return (int)n;
}

/* 大文件分块写 + 分块读, 用校验和比对(不申请 64KB 缓冲) */
static bool LFS_RoundTripBig(void)
{
    lfs_file_t file;
    uint32_t sum_w = 0;
    uint32_t sum_r = 0;
    int err;

    err = lfs_file_open(&g_lfs, &file, "/big.bin", LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err != LFS_ERR_OK)
    {
        printf("[LFS ] big: open(w) err = %d\r\n", err);
        return false;
    }
    for (uint32_t off = 0; off < LFS_TEST_BIG_SIZE; off += LFS_TEST_CHUNK)
    {
        Test_FillPattern(s_test_buf, LFS_TEST_CHUNK, off);
        sum_w ^= Test_Checksum(s_test_buf, LFS_TEST_CHUNK);
        if (lfs_file_write(&g_lfs, &file, s_test_buf, LFS_TEST_CHUNK) != (lfs_ssize_t)LFS_TEST_CHUNK)
        {
            lfs_file_close(&g_lfs, &file);
            printf("[LFS ] big: write failed at %lu\r\n", (unsigned long)off);
            return false;
        }
    }
    if (lfs_file_close(&g_lfs, &file) != LFS_ERR_OK)
        return false;

    err = lfs_file_open(&g_lfs, &file, "/big.bin", LFS_O_RDONLY);
    if (err != LFS_ERR_OK)
    {
        printf("[LFS ] big: open(r) err = %d\r\n", err);
        return false;
    }
    for (uint32_t off = 0; off < LFS_TEST_BIG_SIZE; off += LFS_TEST_CHUNK)
    {
        if (lfs_file_read(&g_lfs, &file, s_test_rd, LFS_TEST_CHUNK) != (lfs_ssize_t)LFS_TEST_CHUNK)
        {
            lfs_file_close(&g_lfs, &file);
            printf("[LFS ] big: read failed at %lu\r\n", (unsigned long)off);
            return false;
        }
        sum_r ^= Test_Checksum(s_test_rd, LFS_TEST_CHUNK);
    }
    lfs_file_close(&g_lfs, &file);

    if (sum_w != sum_r)
        printf("[LFS ] big: sum mismatch 0x%08lX vs 0x%08lX\r\n",
               (unsigned long)sum_w, (unsigned long)sum_r);

    return (sum_w == sum_r);
}

/* 重新挂载后校验 /t1.bin 内容是否仍在 -> 验证数据真的持久化 */
static bool LFS_RemountAndVerify(void)
{
    int err;

    err = lfs_unmount(&g_lfs);
    if (err != LFS_ERR_OK)
    {
        printf("[LFS ] remount: unmount err = %d\r\n", err);
        return false;
    }

    err = lfs_mount(&g_lfs, &g_lfs_config);
    if (err != LFS_ERR_OK)
    {
        printf("[LFS ] remount: mount err = %d\r\n", err);
        return false;
    }

    Test_FillPattern(s_test_buf, 512, 0xAA);
    memset(s_test_rd, 0, 512);
    if (LFS_ReadFile("/t1.bin", s_test_rd, 512) != 512)
        return false;

    return (memcmp(s_test_buf, s_test_rd, 512) == 0);
}

static void LFS_BareMetal_Test(void)
{
    char line[24];
    char detail[80];
    uint32_t pass = 0;
    uint32_t fail = 0;
    int err;
    uint16_t w = 0;
    uint16_t h = 0;
    lfs_ssize_t used;

    OLED_Init();
    OLED_Clear();
    OLED_Write_String(0, 0, "LFS TEST", &Font_16);

#define LFS_CHECK(name, cond, ...)                            \
    do                                                        \
    {                                                         \
        snprintf(detail, sizeof(detail), __VA_ARGS__);        \
        if (cond)                                             \
        {                                                     \
            pass++;                                           \
            printf("[LFS ] %-28s OK   %s\r\n", name, detail); \
        }                                                     \
        else                                                  \
        {                                                     \
            fail++;                                           \
            printf("[LFS ] %-28s FAIL %s\r\n", name, detail); \
        }                                                     \
    } while (0)

    printf("\r\n[LFS ] W25Q64_Init()...\r\n");
    W25Q64_Init();

    err = LittleFS_Init();
    if (err != 0)
    {
        printf("[LFS ] init failed, err = %d, stop here\r\n", err);
        OLED_Write_String(0, 16, "INIT FAIL", &Font_16);
        for (;;)
            ;
    }

    printf("[LFS ] geometry: block = %lu B x %lu, total = %lu KB\r\n",
           (unsigned long)g_lfs_config.block_size, (unsigned long)g_lfs_config.block_count,
           (unsigned long)(g_lfs_config.block_size / 1024U * g_lfs_config.block_count));

    /*  T1: 基本写 + 读回  */
    Test_FillPattern(s_test_buf, 512, 0xAA);
    err = LFS_WriteFile("/t1.bin", s_test_buf, 512);
    memset(s_test_rd, 0, 512);
    int n = LFS_ReadFile("/t1.bin", s_test_rd, 512);
    LFS_CHECK("T1 write/read 512B",
              (err == 0) && (n == 512) && (memcmp(s_test_buf, s_test_rd, 512) == 0),
              "err = %d, read = %d", err, n);

    /*  T2: 目录创建 + 子目录内文件  */
    err = lfs_mkdir(&g_lfs, "/font");
    if (err == LFS_ERR_EXIST)
        err = 0;
    Test_FillPattern(s_test_buf, 1024, 0xBB);
    int err2 = LFS_WriteFile("/font/f.bin", s_test_buf, 1024);
    memset(s_test_rd, 0, 1024);
    n = LFS_ReadFile("/font/f.bin", s_test_rd, 1024);
    LFS_CHECK("T2 mkdir + subdir file",
              (err == 0) && (err2 == 0) && (n == 1024) && (memcmp(s_test_buf, s_test_rd, 1024) == 0),
              "mkdir = %d, write = %d, read = %d", err, err2, n);

    /*  T3: lfs_stat 大小校验  */
    err = lfs_stat(&g_lfs, "/font/f.bin", &s_lfs_info);
    LFS_CHECK("T3 stat size", (err == 0) && (s_lfs_info.size == 1024U),
              "err = %d, size = %lu, type = %d", err,
              (unsigned long)s_lfs_info.size, (int)s_lfs_info.type);

    /*  T4: 追加写(T1 已写入 512B, 追加 128B 后应为 640B)  */
    Test_FillPattern(s_test_buf, 128, 0xCC);
    lfs_file_t af;
    err = lfs_file_open(&g_lfs, &af, "/t1.bin", LFS_O_WRONLY | LFS_O_APPEND);
    if (err == LFS_ERR_OK)
    {
        err2 = (lfs_file_write(&g_lfs, &af, s_test_buf, 128) == 128) ? 0 : -1;
        err = lfs_file_close(&g_lfs, &af);
        if (err == 0)
            err = err2;
    }
    err2 = lfs_stat(&g_lfs, "/t1.bin", &s_lfs_info);
    LFS_CHECK("T4 append 128B", (err == 0) && (err2 == 0) && (s_lfs_info.size == 640U),
              "err = %d, size = %lu", err, (unsigned long)s_lfs_info.size);

    /*  T5: 大文件 64KB 往返  */
    LFS_CHECK("T5 big file 64KB", LFS_RoundTripBig(), "chunk = %lu B",
              (unsigned long)LFS_TEST_CHUNK);

    /*  T6: 目录遍历  */
    lfs_dir_t dir;
    uint32_t entries = 0;
    err = lfs_dir_open(&g_lfs, &dir, "/font");
    if (err == LFS_ERR_OK)
    {
        while (lfs_dir_read(&g_lfs, &dir, &s_lfs_info) > 0)
        {
            if (s_lfs_info.name[0] != '\0')
                entries++;
        }
        lfs_dir_close(&g_lfs, &dir);
    }
    LFS_CHECK("T6 dir traverse /font", (err == 0) && (entries >= 1U),
              "err = %d, entries = %lu", err, (unsigned long)entries);

    /*  T7: 删除文件后 stat 应报 NOENT  */
    err = lfs_remove(&g_lfs, "/font/f.bin");
    err2 = lfs_stat(&g_lfs, "/font/f.bin", &s_lfs_info);
    LFS_CHECK("T7 remove + NOENT", (err == 0) && (err2 == LFS_ERR_NOENT),
              "remove = %d, stat = %d", err, err2);

    /*  T8: 卸载后重新挂载, 数据仍在(真实持久化)  */
    LFS_CHECK("T8 remount persist", LFS_RemountAndVerify(), "unmount + mount + read back");

    /*  T9: 字体存取(MDM 上真实点阵表)  */
    err = SaveFont("/font/f12.bin", Font_12_Table, 1024U);
    memset(s_test_rd, 0, 1024);
    err2 = LoadFont("/font/f12.bin", s_test_rd, 1024U);
    LFS_CHECK("T9 SaveFont/LoadFont",
              (err == 0) && (err2 == 1024) && (memcmp(Font_12_Table, s_test_rd, 1024) == 0),
              "save = %d, load = %d", err, err2);

    /*  T10: 图片存取(40x40 RGB565 = 3200 字节, 用测试图案)  */
    Test_FillPattern(s_test_rd, LFS_TEST_IMG_SIZE, 0x5A);
    err = SaveImage("/img_err.bin", s_test_rd, LFS_TEST_IMG_W, LFS_TEST_IMG_H);
    memset(s_test_buf, 0, LFS_TEST_IMG_SIZE);
    err2 = LoadImage("/img_err.bin", &w, &h, s_test_buf, TEST_BUF_SIZE);
    /* 注意只比较 3200 字节: Image_err.data 指向的 gImage_err 就是 3200 字节,
     * 只比较前 3200 字节 */
    LFS_CHECK("T10 SaveImage/LoadImage",
              (err == 0) && (err2 == 0) && (w == LFS_TEST_IMG_W) && (h == LFS_TEST_IMG_H) &&
                  (memcmp(s_test_rd, s_test_buf, LFS_TEST_IMG_SIZE) == 0),
              "save = %d, load = %d, w = %u, h = %u", err, err2, (unsigned)w, (unsigned)h);

    /*  T11: 打开不存在的文件应返回 NOENT  */
    n = LFS_ReadFile("/no_such_file.bin", s_test_rd, 16);
    LFS_CHECK("T11 open missing file", n == LFS_ERR_NOENT, "ret = %d", n);

    /*  空间使用情况  */
    used = lfs_fs_size(&g_lfs);
    printf("[LFS ] - used blocks = %ld / %lu (%.1f%%), busy-timeout = %lu -\r\n",
           (long)used, (unsigned long)g_lfs_config.block_count,
           ((double)used * 100.0) / (double)g_lfs_config.block_count,
           (unsigned long)W25Q64_GetBusyTimeoutCount());
    printf("[LFS ] - result: PASS %lu, FAIL %lu -\r\n",
           (unsigned long)pass, (unsigned long)fail);

#undef LFS_CHECK

    /* 汇总显示后常驻循环, 便于长时间观察; 每 5 秒把 /t1.bin 重新读一遍 */
    for (;;)
    {
        snprintf(line, sizeof(line), "LFS %lu/%lu", (unsigned long)pass,
                 (unsigned long)(pass + fail));
        OLED_Write_String(0, 0, line, &Font_16);

        snprintf(line, sizeof(line), fail == 0U ? "ALL PASS" : "HAS FAIL");
        OLED_Write_String(0, 16, line, &Font_16);

        memset(s_test_rd, 0, 512);
        n = LFS_ReadFile("/t1.bin", s_test_rd, 512);
        snprintf(line, sizeof(line), "RD=%d", n);
        OLED_Write_String(0, 32, line, &Font_16);

        if (n != 512)
            printf("[LFS ] periodic re-read /t1.bin returned %d\r\n", n);

        delay_ms(5000);
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
    case BM_TEST_MODULE_W25Q64:
        W25Q64_BareMetal_Test();
        break;
    case BM_TEST_MODULE_LFS:
        LFS_BareMetal_Test();
        break;
    default:
        while (1)
            ;
    }
}

#endif /* USE_FREERTOS == 0 */
