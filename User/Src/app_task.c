#include "app_task.h"
#include "Asset.h"

/* ================ 任务角色 ================
 * uiTask    (prio 3): LCD唯一写者。做板级初始化→开机等待画面→
 *                     创建net/sensor任务→开机结果→主页面→事件驱动刷新
 * netTask   (prio 2): 独占USART1/AT。开机连网+SNTP+天气; 之后按周期
 *                     WiFi保活/SNTP/天气更新, 数据就绪后通知uiTask
 * DHT22Task (prio 4): DHT22温湿度周期采集, 更新后通知uiTask。
 *                     优先级高于ui, 避免UI抢占破坏DHT22的us级时序
 * LightSensorTask (prio 2): 光敏传感器中断采集, 更新后通知uiTask。
 * ========================================== */

static volatile bool s_lowpower = false; // 低功耗模式(夜间进入,网络活动和温湿度传感器停止)

/* 开机网络阶段结果(仅uiTask在EV_NET_READY后读取)  */
static NetBoot_t s_boot;                // 开机状态
static EventGroupHandle_t g_evt = NULL; // 事件组, 用于通知uiTask

/* LCD/OLED 状态 */
static bool s_lcd_on = true;
static bool s_oled_on = false;

/* ==================== DHT22 Task ==================== */
#define DHT22_TASK_PRIORITY 4
#define DHT22_TASK_STACK_SIZE 512
TaskHandle_t dht22_task_handle;

static void DHT22_Task(void *pvParameters)
{
    (void)pvParameters;

    for (;;)
    {
        /* 等待超时(10s)即为采集周期; 同时等待"退出低功耗立即补采"请求 */
        EventBits_t bits = xEventGroupWaitBits(g_evt, EV_SENSOR_UPDATE_NOW, pdTRUE, pdFALSE, pdMS_TO_TICKS(10 * 1000));

        bool force = (bits & EV_SENSOR_UPDATE_NOW) != 0;

        /* 低功耗期间: 只有收到"立即补采"请求才采集, 否则跳过 */
        if (s_lowpower && !force)
            continue;

        if (Service_Room_Update()) // DHT22 读取, 成功失败都会刷新room_info
        {
            xEventGroupSetBits(g_evt, EV_ROOM);
        }
    }
}

/* ==================== Light Sensor Task ==================== */

static TaskHandle_t s_light_task = NULL; // 光敏传感器任务句柄

/** @brief 光敏中断回调(ISR上下文): 仅通知任务, 去抖在任务内完成
 *  @note 仅通知任务, 不处理中断逻辑, 任务内会去抖并根据状态切换uiTask的显示
 */
static void Light_IRQ_Notify(void)
{
    BaseType_t woken = pdFALSE; // 是否唤醒任务

    if (s_light_task != NULL)
    {
        vTaskNotifyGiveFromISR(s_light_task, &woken); // 通知正在阻塞的任务,解除阻塞状态
        portYIELD_FROM_ISR(woken);                    // 请求一次上下文切换
    }
}

#define LIGHT_SENSOR_TASK_PRIORITY 2
#define LIGHT_SENSOR_TASK_STACK_SIZE 512
TaskHandle_t lightsensor_task_handle;

static void LightSensor_Task(void *pvParameters)
{
    (void)pvParameters;

    s_light_task = xTaskGetCurrentTaskHandle();      // 保存当前任务句柄, 用于中断回调
    Light_Sensor_RegisterCallback(Light_IRQ_Notify); // 注册中断回调函数
    Light_Sensor_Init();                             // 初始化光敏传感器

    bool requested_night = false; // 与UI默认(白天)一致
    bool first_run = true;        // 上电先做一次初始判定

    printf("[LIGHT] task ready: DO_state=%d dark=%d\r\n",
           (int)Light_Sensor_DO_State, (int)Light_Sensor_IsDark());

    for (;;)
    {
        bool notified = false;

        if (!first_run)
        {
            /* 等待中断通知; 1000ms 超时用作轮询兜底, 保证 EXTI 未触发时仍能切换 */
            notified = (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) > 0);
        }
        first_run = false;

        if (!notified)
        {
            /* 轮询兜底: 电平与当前模式一致则无需处理(避免重复切换) */
            if (Light_Sensor_IsDark() == requested_night)
                continue;
        }

        /* 2s 去抖: 期间若再来中断则重新计时 */
        TickType_t t0 = xTaskGetTickCount();
        for (;;)
        {
            TickType_t remain = pdMS_TO_TICKS(DEBOUNCE_MS) - (xTaskGetTickCount() - t0); // 去抖时间剩余
            if ((int32_t)remain <= 0)
                break; // 去抖时间到
            if (ulTaskNotifyTake(pdTRUE, remain) > 0)
                t0 = xTaskGetTickCount(); /* 新边沿, 重新去抖 */
        }

        bool dark = Light_Sensor_IsDark(); // 判断是否为“暗”
        printf("[LIGHT] debounce done: level=%s, mode=%s\r\n",
               dark ? "dark" : "bright", requested_night ? "night" : "day");

        if (dark && !requested_night)
        {
            requested_night = true;
            printf("[LIGHT] request NIGHT\r\n");
            xEventGroupSetBits(g_evt, EV_LOWERPOWER);                                          // 进入夜晚: LCD关, OLED显示
            xEventGroupWaitBits(g_evt, EV_LOWPOWER_ACK, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000)); // 等待UI确认
        }
        else if (!dark && requested_night)
        {
            requested_night = false;
            printf("[LIGHT] request DAY\r\n");
            xEventGroupSetBits(g_evt, EV_WAKEUP);                                              // 回到白天: OLED关, LCD恢复
            xEventGroupWaitBits(g_evt, EV_LOWPOWER_ACK, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000)); // 等待UI确认
        }
    }
}

/* ==================== netTask ==================== */
#define NET_TASK_PRIORITY 2
#define NET_TASK_STACK_SIZE 1024
TaskHandle_t net_task_handle;

static void Net_Task(void *pvParameters)
{
    (void)pvParameters;
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
    bool t_ok = wifi_up && Service_Time_Sync();
    bool w_ok = wifi_up && Service_Weather_Update();

    s_boot.service_ok = wifi_up && t_ok && w_ok;

    /* 通知UI: 开机阶段完成(无论成败), 之后后台继续重试 */
    xEventGroupSetBits(g_evt, EV_NET_READY);

    /* ---- 周期调度初值 ---- */
    wifi_c = wifi_up ? WIFI_PERIOD_S : RETRY_WIFI_S;       // WiFi检查周期,连接上30min,未连10s
    sntp_c = t_ok ? SNTP_PERIOD_S : RETRY_SNTP_S;          // SNTP周期,成功后4h,未成功5s
    weather_c = w_ok ? WEATHER_PERIOD_S : RETRY_WEATHER_S; // 天气周期,成功后1h,未成功60s

    for (;;)
    {
        /* 1s 心跳; 同时等待"退出低功耗立即补更"请求(netTask 专属位) */
        EventBits_t bits = xEventGroupWaitBits(g_evt, EV_NET_UPDATE_NOW, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));

        if (bits & EV_NET_UPDATE_NOW)
        {
            /* 三个计数器置 1: 下一拍连续执行 WiFi 检查 + SNTP + 天气 */
            wifi_c = 1U;
            sntp_c = 1U;
            weather_c = 1U;

            printf("[NET] LowPower exit, reset update\r\n");
        }

        /* 低功耗期间: 冻结周期计数器, 不执行任何 update */
        if (s_lowpower)
            continue;

        if (--wifi_c == 0U)
        {
            bool was_up = wifi_up;
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
                if (r > 0) // 刚重连成功: 重新配置SNTP并立即补SNTP+天气
                {
                    AT_SNTP_Init();
                    sntp_c = 1U;
                    weather_c = 1U;
                }
            }
            if (r != 0 || !was_up)
                xEventGroupSetBits(g_evt, EV_WIFI); // 状态变化/首次连上: 刷顶部条
        }

        if (--sntp_c == 0U)
        {
            if (!wifi_up)
            {
                sntp_c = 1U; // WiFi未连, 每秒顺延, 等wifi恢复后触发
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
static void UI_Enter_Night(void)
{
    static bool oled_inited = false; // 默认未初始化OLED
    AT_Date_Info_t t;

    /* 进入 lowpower 模式前, 同步RTC时间 */
    if (Clock_IsSynced())
    {
        bool ret = RTC_Sync_From_SystemClock();
        printf("[LP] Sync RTC time %s\r\n", ret ? "done" : "fail");
    }

    s_lowpower = true; // 进入低功耗模式

    /* 初始化OLED, 时间优先取RTC */
    if (!oled_inited)
    {
        OLED_Init();
        OLED_Clear();
        oled_inited = true;
    }
    OLED_Display_On(); // 0xAF 开显示 + 清屏

    bool time_source = RTC_ReadDataTime(&t);                  // 从RTC读取时间
    OLED_ShowClock(t.hour, t.minute, t.year, t.month, t.day); // 显示时间+日期

    /* 关闭LCD，并置全局状态位 */
    ST7789_Display_Power(false); // 关闭背光 + 0x28 显示睡眠
    s_lcd_on = false;            // 关闭LCD
    s_oled_on = true;            // 显示OLED

    printf("[LP] Enter night: LCD off, OLED on, time_source=%s\r\n", time_source ? "RTC" : "SoftClock");
}

/* 回到白天: OLED关闭, LCD恢复并全量重绘 */
static void UI_Enter_Day(void)
{
    OLED_Display_Off();

    ST7789_Display_Power(true); // 0x29 显示开 + 开背光
    Main_Page_Display();        // 夜间未刷新, 全量重绘一次
    s_lcd_on = true;            // 开启LCD
    s_oled_on = false;          // 关闭OLED

    /* 通知网络/传感器更新数据 */
    s_lowpower = false; // 退出低功耗模式
    xEventGroupSetBits(g_evt, EV_NET_UPDATE_NOW | EV_SENSOR_UPDATE_NOW);

    printf("[UI] Enter day: OLED off, LCD on\r\n");
}

/* ==================== uiTask(LCD唯一写者) ==================== */
#define UI_TASK_PRIORITY 3
#define UI_TASK_STACK_SIZE 1024
TaskHandle_t ui_task_handle;

static void UI_Task(void *pvParameters)
{
    (void)pvParameters;

    EventBits_t bits = 0; // 事件位, 用于等待uiTask的事件

    g_evt = xEventGroupCreate(); // 事件组: 仅被等待的bit置位才会唤醒

    Board_Init(); // TIM5/LCD/USART2/ST7789 初始化
    printf("[SYS]Build Date:%s %s\r\n", __DATE__, __TIME__);
    printf("[UI] Board init done, boot page\r\n");

    /* 资源层未就绪时字模与图片都取不到, 屏幕只会出现纯色块。
     * 这里只提示, 不阻塞流程 —— 网络与传感器功能仍可正常使用。 */
    if (!Asset_Ready())
    {
        printf("[UI] WARN: asset layer not ready, screen shows blank blocks\r\n");
        printf("[UI] WARN: flash the provision firmware to write fonts/images\r\n");
    }

    Boot_Page_Wait(); // 开机等待画面

    /* 创建DHT22/服务任务(sensor=4保护DHT22时序, net=2低于ui避免饿死显示) */
    xTaskCreate(DHT22_Task, "dht22", DHT22_TASK_STACK_SIZE, NULL, DHT22_TASK_PRIORITY, &dht22_task_handle);
    xTaskCreate(Net_Task, "net", NET_TASK_STACK_SIZE, NULL, NET_TASK_PRIORITY, &net_task_handle);

    /* 等待开机网络阶段结束(成功或失败都置EV_NET_READY), 超时30s兜底 */
    xEventGroupWaitBits(g_evt, EV_NET_READY, pdTRUE, pdFALSE, pdMS_TO_TICKS(30 * 1000));
    printf("[UI] Boot net stage done: wifi=%d service=%d\r\n", (int)s_boot.wifi_ok, (int)s_boot.service_ok);

    Boot_Page_Show(s_boot.wifi_ok, s_boot.service_ok); // 开机结果(连接详情)
    vTaskDelay(pdMS_TO_TICKS(2500));                   // 结果页停留, 便于查看详情
    printf("[UI] Enter main page\r\n");
    Main_Page_Display(); // 进入主页面(数据已就绪, 首绘即正确)

    /* 进入主页面后再启动光敏任务: 保证昼夜切换只发生在开机完成之后 */
    xTaskCreate(LightSensor_Task, "light_sensor", LIGHT_SENSOR_TASK_STACK_SIZE, NULL, LIGHT_SENSOR_TASK_PRIORITY, &lightsensor_task_handle);

    for (;;)
    {
        /* 低功耗模式: 事件等待放宽(夜间只显示分钟); 非低功耗: 500ms心跳 */
        TickType_t wait = s_lowpower ? pdMS_TO_TICKS(EV_LP_UI_TICK_MS) : pdMS_TO_TICKS(500);

        bits = xEventGroupWaitBits(g_evt,
                                   EV_WEATHER | EV_ROOM | EV_WIFI | EV_LOWERPOWER | EV_WAKEUP,
                                   pdTRUE, pdFALSE, wait);

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
                Main_Page_Net_Update(); // WiFi连接状态变化: 刷顶部WiFi/定位条
            if (bits & EV_WEATHER)
            {
                Main_Page_Weather_Update();
                Main_Page_Net_Update(); // 天气到位后同步城市/定位图标
            }
            if (bits & EV_ROOM)
                Main_Page_Room_Update();
        }

        /* 夜间OLED仅显示时间+日期(分钟变化才刷新); 白天刷新LCD时钟 */
        if (s_oled_on)
        {
            static uint8_t last_h = 0xFF, last_m = 0xFF, last_d = 0xFF;
            AT_Date_Info_t t;

            (void)RTC_ReadDataTime(&t); // 夜间时间优先取外部RTC, 失败时内部已回退软件时钟
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
            Main_Page_Clock_Update(); // 时钟/日期刷新
        }
    }
}

void App_Task_Init(void)
{
    xTaskCreate(UI_Task, "ui", UI_TASK_STACK_SIZE, NULL, UI_TASK_PRIORITY, &ui_task_handle);
}
