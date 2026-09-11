#include "app_task.h"

/* ================ 任务角色 ================
 * uiTask    (prio 3): LCD唯一写者。做板级初始化→开机等待画面→
 *                     创建net/sensor任务→开机结果→主页面→事件驱动刷新
 * netTask   (prio 2): 独占USART1/AT。开机连网+SNTP+天气; 之后按周期
 *                     WiFi保活/SNTP/天气更新, 数据就绪后通知uiTask
 * sensorTask(prio 4): DHT22温湿度周期采集, 更新后通知uiTask。
 *                     优先级高于ui, 避免UI抢占破坏DHT22的us级时序
 *
 * ========================================== */

/*  开机网络阶段结果(仅uiTask在EV_NET_READY后读取)  */
static NetBoot_t s_boot;
static EventGroupHandle_t g_evt = NULL;

/* LCD/OLED 状态 */
static bool s_lcd_on = true;
static bool s_oled_on = false;

/* ==================== DHT22 Task ==================== */

static void DHT22_Task(void *param)
{
    (void)param;

    for (;;)
    {
        if (Service_Room_Update()) /* DHT22 读取, 成功失败都会刷新room_info */
        {
            xEventGroupSetBits(g_evt, EV_ROOM);
        }
        vTaskDelay(pdMS_TO_TICKS(10 * 1000));
    }
}

/* ==================== Light Sensor Task ==================== */

static void LightSensor_Task(void *param)
{
    (void)param;

    Light_Sensor_Init();

    bool requested_night = false;                 // 是否请求进入夜晚标记
    bool last_state = Light_Sensor_IsDark();      // 初始状态
    TickType_t change_tick = xTaskGetTickCount(); // 初始去抖计时

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(200)); /* 采集周期, 必须阻塞让出CPU */

        bool state = Light_Sensor_IsDark(); /* 采集当前状态 */

        if (state != last_state)
        {
            last_state = state;                // 状态变化: 重新开始去抖计时
            change_tick = xTaskGetTickCount(); // 重置去抖计时
            continue;
        }

        if (xTaskGetTickCount() - change_tick < pdMS_TO_TICKS(DEBOUNCE_MS))
            continue; /* 未稳定 */

        if (state && !requested_night)
        {
            requested_night = true;                   // 置为请求进入夜晚标记
            xEventGroupSetBits(g_evt, EV_LOWERPOWER); /* 请求进入夜晚: LCD关, OLED显示 */
            xEventGroupWaitBits(g_evt, EV_LOWPOWER_ACK, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
        }
        else if (!state && requested_night)
        {
            requested_night = false;              // 置为未请求进入夜晚标记
            xEventGroupSetBits(g_evt, EV_WAKEUP); /* 请求回到白天: OLED关, LCD恢复 */
            xEventGroupWaitBits(g_evt, EV_LOWPOWER_ACK, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
        }
    }
}

/* ==================== netTask ==================== */

static void Net_Task(void *param)
{
    (void)param;
    TickType_t last;
    uint32_t sntp_c, wifi_c, weather_c;
    bool wifi_up;

    printf("[NET] Init Start\r\n");

    /* ---- 开机阶段: AT/WiFi初始化 + 连接 ---- */
    s_boot.wifi_ok = Wireless_Init();
    if (s_boot.wifi_ok)
        s_boot.wifi_ok = Service_WiFi_Connect();
    wifi_up = s_boot.wifi_ok;

    /* ---- 开机阶段: SNTP + 天气(需WiFi) ---- */
    if (wifi_up)
        AT_SNTP_Init();
    bool t_ok = Service_Time_Sync();
    bool w_ok = Service_Weather_Update();

    s_boot.service_ok = wifi_up && t_ok && w_ok;

    /* 通知UI: 开机阶段完成(无论成败), 之后后台继续重试 */
    xEventGroupSetBits(g_evt, EV_NET_READY);

    /* ---- 周期调度初值 ---- */
    wifi_c = wifi_up ? WIFI_PERIOD_S : RETRY_WIFI_S;       /* WiFi检查周期,连接上30min,未连10s */
    sntp_c = t_ok ? SNTP_PERIOD_S : RETRY_SNTP_S;          /* SNTP周期,成功后4h,未成功5s */
    weather_c = w_ok ? WEATHER_PERIOD_S : RETRY_WEATHER_S; /* 天气周期,成功后1h,未成功60s */

    last = xTaskGetTickCount();
    for (;;)
    {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000)); /* 1s心跳 */

        if (--wifi_c == 0U)
        {
            int r = Service_WiFi_Update();
            if (r < 0)
            {
                wifi_up = false;
                wifi_c = RETRY_WIFI_S;
            }
            else
            {
                wifi_up = true;
                wifi_c = WIFI_PERIOD_S;
                if (r > 0) /* 刚重连成功: 重新配置SNTP并立即补SNTP+天气 */
                {
                    AT_SNTP_Init();
                    sntp_c = 1U;
                    weather_c = 1U;
                }
            }
            if (r != 0)
                xEventGroupSetBits(g_evt, EV_WIFI); /* 状态变化: 通知UI刷顶部条 */
        }

        if (--sntp_c == 0U)
        {
            if (!wifi_up)
            {
                sntp_c = 1U; /* WiFi未连, 每秒顺延, 等wifi恢复后触发 */
            }
            else if (Service_Time_Sync())
            {
                sntp_c = SNTP_PERIOD_S;
            }
            else
            {
                sntp_c = RETRY_SNTP_S;
            }
        }

        if (--weather_c == 0U)
        {
            if (!wifi_up)
            {
                weather_c = 1U;
            }
            else if (Service_Weather_Update())
            {
                weather_c = WEATHER_PERIOD_S;
                xEventGroupSetBits(g_evt, EV_WEATHER);
            }
            else
            {
                weather_c = RETRY_WEATHER_S;
            }
        }
    }
}

/* ==================== 双屏切换 ==================== */

/** @brief 进入夜晚: LCD完全关闭, OLED显示时间+日期(首次懒初始化)
 *  @note 首次调用时, OLED会初始化, 后续调用仅更新时间+日期
 */
static void UI_Enter_Night(void)
{
    static bool oled_inited = false; // 是否初始化OLED
    AT_Date_Info_t t;

    if (!oled_inited)
    {
        OLED_Init();
        oled_inited = true;
    }
    OLED_Display_On(); /* 0xAF 开显示 + 清屏 */

    Clock_GetDateTime(&t);
    OLED_ShowClock(t.hour, t.minute, t.year, t.month, t.day);

    ST7789_Display_Power(false); /* 关背光 + 0x28 显示睡眠 */
    s_lcd_on = false;            // 关闭LCD
    s_oled_on = true;            // 显示OLED
    printf("[UI] Enter night: LCD off, OLED on\r\n");
}

/* 回到白天: OLED关闭, LCD恢复并全量重绘 */
static void UI_Enter_Day(void)
{
    OLED_Display_Off();

    ST7789_Display_Power(true); /* 0x29 显示开 + 开背光 */
    Main_Page_Display();        /* 夜间未刷新, 全量重绘一次 */
    s_lcd_on = true;            // 开启LCD
    s_oled_on = false;          // 关闭OLED
    printf("[UI] Enter day: OLED off, LCD on\r\n");
}

/* ==================== uiTask(LCD唯一写者) ==================== */

static void UI_Task(void *param)
{
    (void)param;
    EventBits_t bits = 0;

    g_evt = xEventGroupCreate(); /* 事件组: 仅被等待的bit置位才会唤醒 */

    Board_Init(); /* TIM5/LCD/USART2/ST7789 初始化 */
    printf("[SYS]Build Date:%s %s\r\n", __DATE__, __TIME__);
    printf("[UI] Board init done, boot page\r\n");
    Boot_Page_Wait(); /* 开机等待画面 */

    /* 创建采集/服务任务(sensor=4保护DHT22时序, net=2低于ui避免饿死显示) */
    xTaskCreate(DHT22_Task, "dht22", 512, NULL, 4, NULL);
    xTaskCreate(Net_Task, "net", 1024, NULL, 2, NULL);

    /* 等待开机网络阶段结束(成功或失败都置EV_NET_READY), 超时30s兜底 */
    xEventGroupWaitBits(g_evt, EV_NET_READY, pdTRUE, pdFALSE, pdMS_TO_TICKS(30 * 1000));
    printf("[UI] Boot net stage done: wifi=%d service=%d\r\n", (int)s_boot.wifi_ok, (int)s_boot.service_ok);

    Boot_Page_Show(s_boot.wifi_ok, s_boot.service_ok); /* 开机结果(连接详情) */
    vTaskDelay(pdMS_TO_TICKS(2500));                   /* 结果页停留, 便于查看详情 */
    printf("[UI] Enter main page\r\n");
    Main_Page_Display(); /* 进入主页面(数据已就绪, 首绘即正确) */

    /* 进入主页面后再启动光敏任务: 保证昼夜切换只发生在开机完成之后 */
    xTaskCreate(LightSensor_Task, "light", 512, NULL, 2, NULL);

    for (;;)
    {
        /* 500ms心跳 + 事件唤醒(仅等待的bit被置位才返回) */
        bits = xEventGroupWaitBits(g_evt,
                                   EV_WEATHER | EV_ROOM | EV_WIFI | EV_LOWERPOWER | EV_WAKEUP,
                                   pdTRUE, pdFALSE, pdMS_TO_TICKS(500));

        /* 昼夜切换(阻塞等待ACK的光敏任务需要该确认) */
        if (bits & EV_LOWERPOWER)
        {
            UI_Enter_Night();
            xEventGroupSetBits(g_evt, EV_LOWPOWER_ACK);
        }
        if (bits & EV_WAKEUP)
        {
            UI_Enter_Day();
            xEventGroupSetBits(g_evt, EV_LOWPOWER_ACK);
        }

        /* 仅白天绘制LCD */
        if (s_lcd_on)
        {
            if (bits & EV_WIFI)
                Main_Page_Net_Update(); /* WiFi连接状态变化: 刷顶部WiFi/定位条 */
            if (bits & EV_WEATHER)
            {
                Main_Page_Weather_Update();
                Main_Page_Net_Update(); /* 天气到位后同步城市/定位图标 */
            }
            if (bits & EV_ROOM)
                Main_Page_Room_Update();
        }

        /* 夜间OLED仅显示时间+日期(分钟变化才刷新); 白天刷新LCD时钟 */
        if (s_oled_on)
        {
            static uint8_t last_h = 0xFF, last_m = 0xFF, last_d = 0xFF;
            AT_Date_Info_t t;
            Clock_GetDateTime(&t);
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

void App_Task_Init(void)
{
    xTaskCreate(UI_Task, "ui", 1024, NULL, 3, NULL);
}
