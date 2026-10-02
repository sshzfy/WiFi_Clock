#include "UI_Task.h"

/* LCD/OLED 状态(仅uiTask访问) */
static bool s_lcd_on = true;
static bool s_oled_on = false;

/* ==================== 低功耗设计 ==================== */

/** @brief 确保RTC初始化
 *  @note 首次调用时, 初始化RTC, 后续调用仅检查是否已初始化
 */
static void RTC_Ensure_Init(void)
{
    static bool rtc_inited = false; // 是否初始化RTC
    if (!rtc_inited)
    {
        DS1302_Init();
        rtc_inited = true;
    }
}

/** @brief 从系统时钟同步RTC时间
 *  @return true 成功, false 失败
 *  @note 首次调用时, 初始化RTC, 后续调用仅检查是否已初始化
 *  @note 首次调用时, 从系统时钟获取时间, 后续调用仅对比时间是否一致, 允许分钟误差2分钟
 */
static bool RTC_Sync_From_SystemClock(void)
{
    AT_Date_Info_t t;
    DS1302_Time_t rtc, check; // rtc-写入外部RTC的时间,check-回读对比时间

    if (!Clock_IsSynced())
        return false;

    Clock_GetDateTime(&t);
    if ((t.year < 2000))
        return false;

    rtc.year = t.year;
    rtc.month = t.month;
    rtc.day = t.day;
    rtc.hour = t.hour;
    rtc.min = t.minute;
    rtc.sec = t.second;
    rtc.week = (t.weekday >= 1 && t.weekday <= 7) ? t.weekday : 1; // DS1302 星期寄存器为 1~7

    RTC_Ensure_Init();
    if (!DS1302_SetTime(&rtc))
        return false;

    if (!DS1302_ReadTime(&check))
        return false;

    /* 对比时间是否一致: 用"当日分钟总数"比较, 允许写入耗时造成的 2 分钟偏差(含跨小时) */
    int base_min = (int)rtc.hour * 60 + (int)rtc.min;
    int chk_min = (int)check.hour * 60 + (int)check.min;
    int dm = chk_min - base_min;

    bool ret = (check.year == rtc.year) &&
               (check.month == rtc.month) &&
               (check.day == rtc.day) &&
               (check.week == rtc.week) &&
               (dm >= 0) && (dm <= 2);

    return ret;
}

/** @brief 从RTC读取数据时间
 *  @param out 输出时间结构体指针
 *  @return true 成功, 从RTC读取时间, false 失败, 从系统时钟获取时间
 */
static bool RTC_ReadDataTime(AT_Date_Info_t *out)
{
    DS1302_Time_t rtc;

#if RTC_LOWPOWER_ENABLE

    RTC_Ensure_Init();
    if (DS1302_ReadTime(&rtc))
    {
        out->year = rtc.year;
        out->month = rtc.month;
        out->day = rtc.day;
        out->hour = rtc.hour;
        out->minute = rtc.min;
        out->second = rtc.sec;
        out->weekday = (rtc.week >= 1 && rtc.week <= 7) ? rtc.week : 1;

        return true;
    }
#endif

    Clock_GetDateTime(out); /* 从系统时钟获取时间 */

    return false;
}

/* ==================== 双屏切换 ==================== */

/** @brief 进入夜晚: LCD完全关闭, OLED显示时间+日期(首次懒初始化)
 *  @note 首次调用时, OLED会初始化, 后续调用仅更新时间+日期
 */
void UI_Task_EnterNight(void)
{
    static bool oled_inited = false; // 默认未初始化OLED
    AT_Date_Info_t t;

    /* 进入 lowpower 模式前, 同步RTC时间 */
    if (Clock_IsSynced())
    {
        bool ret = RTC_Sync_From_SystemClock();
        printf("[LP] Sync RTC time %s\r\n", ret ? "done" : "fail");
    }

    APP_SET_LOWPOWER(true); // 进入低功耗模式

    /* 初始化OLED, 时间优先取RTC */
    if (!oled_inited)
    {
        OLED_Init();
        OLED_Clear();
        oled_inited = true;
    }
    OLED_Display_On(); // 0xAF 开显示 + 清屏

    bool time_source = RTC_ReadDataTime(&t);                  // 从RTC读取时间
    OLED_ShowClock(t.hour, t.minute, t.year, t.month, t.day); /* 显示时间+日期 */

    /* 关闭LCD，并置全局状态位 */
    ST7789_Display_Power(false); /* 关闭背光 + 0x28 显示睡眠 */
    s_lcd_on = false;            // 关闭LCD
    s_oled_on = true;            // 显示OLED

    printf("[LP] Enter night: LCD off, OLED on, time_source=%s\r\n", time_source ? "RTC" : "SoftClock");
}

/** @brief 回到白天: OLED关闭, LCD恢复并全量重绘(并清除低功耗)
 *  @note  由光敏任务裁决后调用
 */
void UI_Task_EnterDay(void)
{
    OLED_Display_Off();

    ST7789_Display_Power(true); /* 0x29 显示开 + 开背光 */

    uint32_t prof_t0 = Prof_Cycles(); /* 整页重绘耗时(DWT CYCCNT) */
    Main_Page_Display();              /* 夜间未刷新, 全量重绘一次 */
    printf("[PROF] main page render: %u us\r\n", (unsigned)Prof_Us(prof_t0));

    s_lcd_on = true;   // 开启LCD
    s_oled_on = false; // 关闭OLED

    /* 通知网络/传感器更新数据 */
    APP_SET_LOWPOWER(false); // 退出低功耗模式
    xEventGroupSetBits(Task_Events(), EV_NET_UPDATE_NOW | EV_DHT22_UPDATE_NOW);

    printf("[UI] Enter day: OLED off, LCD on\r\n");
}

/** @brief 当前是否处于低功耗(夜间)模式 */
bool UI_Task_IsLowPower(void)
{
    return APP_IS_LOWPOWER();
}

/* ==================== uiTask(LCD唯一写者) ==================== */

static void UI_Task(void *pvParameters)
{
    (void)pvParameters;

    EventBits_t bits = 0; // 事件位, 用于等待uiTask的事件

    Task_Common_Init(); /* 事件组: 仅被等待的bit置位才会唤醒 */

    Board_Init(); /* TIM5/LCD/USART2/ST7789 初始化 */
    printf("[UI] UI_Task start\r\n");
    printf("[UI] Board init done, boot page\r\n");

    /* 资源层未就绪时字模与图片都取不到, 屏幕只会出现纯色块。
     * 这里只提示, 不阻塞流程 —— 网络与传感器功能仍可正常使用。 */
    if (!Asset_Ready())
    {
        printf("[UI] WARN: asset layer not ready, screen shows blank blocks\r\n");
        printf("[UI] WARN: flash the provision firmware to write fonts/images\r\n");
    }

    Boot_Page_Wait(); /* 开机等待页面 */

    /* 创建DHT22/服务任务(sensor=4保护DHT22时序, net=2低于ui避免饿死显示) */
    DHT22_Task_Start();
    Net_Task_Start();

    /* 等待开机网络阶段结束(成功或失败都置EV_NET_READY), 超时30s兜底 */
    xEventGroupWaitBits(Task_Events(), EV_NET_READY, pdTRUE, pdFALSE, pdMS_TO_TICKS(30 * 1000));

    /* 网络结果由 netTask 提供, UI 不再直接读它的内部状态 */
    bool boot_wifi = false, boot_service = false;
    Net_Task_GetBoot(&boot_wifi, &boot_service);
    printf("[UI] Boot net stage done: wifi=%d service=%d\r\n", (int)boot_wifi, (int)boot_service);

    Boot_Page_Show(boot_wifi, boot_service); /* 开机结果(连接详情) */
    vTaskDelay(pdMS_TO_TICKS(2500));         /* 结果页停留, 便于查看详情 */
    printf("[UI] Enter main page\r\n");

    uint32_t prof_t0 = Prof_Cycles(); /* 整页重绘耗时(DWT CYCCNT) */
    Main_Page_Display();              /* 进入主页面(数据已就绪, 首绘即正确) */
    printf("[PROF] main page render: %u us\r\n", (unsigned)Prof_Us(prof_t0));

    /* 进入主页面后再启动光敏任务与按键任务: 保证昼夜切换只发生在开机完成之后 */
    LightSensor_Task_Start();
    Key_Task_Start();

    for (;;)
    {
        /* 低功耗模式: 事件等待放宽(夜间只显示分钟); 非低功耗: 500ms心跳 */
        TickType_t wait = APP_IS_LOWPOWER() ? pdMS_TO_TICKS(EV_LP_UI_TICK_MS) : pdMS_TO_TICKS(500);

        bits = xEventGroupWaitBits(Task_Events(),
                                   EV_WEATHER | EV_DHT22 | EV_WIFI | EV_LOWERPOWER | EV_WAKEUP,
                                   pdTRUE, pdFALSE, wait);

        /* 昼夜切换(阻塞等待ACK的光敏任务需要该确认) */
        if (bits & EV_LOWERPOWER)
        {
            UI_Task_EnterNight();
            xEventGroupSetBits(Task_Events(), EV_LOWPOWER_ACK);
        }
        if (bits & EV_WAKEUP)
        {
            UI_Task_EnterDay();
            xEventGroupSetBits(Task_Events(), EV_LOWPOWER_ACK);
        }

        /* 仅白天绘制LCD */
        if (s_lcd_on)
        {
            if (bits & EV_WIFI)
                Main_Page_Net_Update(); /* WiFi连接状态变化: 刷顶部状态条 */
            if (bits & EV_WEATHER)
            {
                Main_Page_Weather_Update();
                Main_Page_Net_Update(); /* 天气到位后同步城市名(按公网IP定位) */
            }
            if (bits & EV_DHT22)
                Main_Page_Room_Update(); /* 房间温度更新 */
        }

        /* 夜间OLED仅显示时间+日期(分钟变化才刷新); 白天刷新LCD时钟 */
        if (s_oled_on)
        {
            static uint8_t last_h = 0xFF, last_m = 0xFF, last_d = 0xFF;
            AT_Date_Info_t t;

            (void)RTC_ReadDataTime(&t); /* 夜间时间优先取外部RTC, 失败时内部已回退软件时钟 */
            if (t.hour != last_h || t.minute != last_m || t.day != last_d)
            {
                last_h = t.hour;
                last_m = t.minute;
                last_d = t.day;
                OLED_ShowClock(t.hour, t.minute, t.year, t.month, t.day);
            }
        }
        else
        {
            Main_Page_Clock_Update(); /* 时钟/日期刷新 */
        }
    }
}

/* uiTask: LCD唯一写者 + 开机流程编排者(创建 net/sensor 任务) */
void UI_Task_Start(void)
{
    xTaskCreate(UI_Task, "ui", UI_TASK_STACK_SIZE, NULL, UI_TASK_PRIORITY, NULL);
}
