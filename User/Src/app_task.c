#include "app_task.h"

/* ================ 任务角色 ================
 * uiTask    (prio 3): LCD唯一写者。做板级初始化→开机等待画面→
 *                     创建net/sensor任务→开机结果→主页面→事件驱动刷新
 * netTask   (prio 2): 独占USART1/AT。开机连网+SNTP+天气; 之后按周期
 *                     WiFi保活/SNTP/天气更新, 数据就绪后通知uiTask
 * DHT22Task (prio 4): DHT22温湿度周期采集, 更新后通知uiTask。
 *                     优先级高于ui, 避免UI抢占破坏DHT22的us级时序
 * LightSensorTask (prio 2): 光敏传感器中断采集, 更新后通知uiTask。
 * KeyTask   (prio 2): 按键(PA0)手势识别, 产出单击/双击/三击/长按;
 *                     目前单击 → 请求切换昼夜(交LightSensorTask裁决)
 * ========================================== */
#define UI_TASK_PRIORITY      3
#define UI_TASK_STACK_SIZE    1024
TaskHandle_t ui_task_handle;
static void UI_Task(void *pvParameters);

#define DHT22_TASK_PRIORITY      4
#define DHT22_TASK_STACK_SIZE    512
TaskHandle_t dht22_task_handle;
static void DHT22_Task(void *pvParameters);

#define NET_TASK_PRIORITY      2
#define NET_TASK_STACK_SIZE    1024
TaskHandle_t net_task_handle;
static void Net_Task(void *pvParameters);

#define LIGHT_SENSOR_TASK_PRIORITY      2
#define LIGHT_SENSOR_TASK_STACK_SIZE    512
TaskHandle_t lightsensor_task_handle;
static void LightSensor_Task(void *pvParameters);

#define KEY_TASK_PRIORITY      2
#define KEY_TASK_STACK_SIZE    1024
TaskHandle_t key_task_handle;
static void Key_Task(void *pvParameters);

static volatile bool s_lowpower = false; // 低功耗模式(夜间进入,网络活动和温湿度传感器停止)

/* 开机网络阶段结果(仅uiTask在EV_NET_READY后读取)  */
static NetBoot_t s_boot;                // 开机状态
static EventGroupHandle_t g_evt = NULL; // 事件组, 用于通知uiTask

/* LCD/OLED 状态 */
static bool s_lcd_on = true;
static bool s_oled_on = false;

/* ==================== DHT22 Task ==================== */

static void DHT22_Task(void *pvParameters)
{
    (void)pvParameters;

    printf("[DHT22] DHT22_Task start\r\n");

    bool first_run = true;

    /* 第一次运行: 立即采集 */
    if (first_run)
    {
        first_run = false;
        Service_Room_Update();
    }

    for (;;)
    {
        /* 等待超时(10s)即为采集周期; 同时等待"退出低功耗立即补采"请求 */
        EventBits_t bits = xEventGroupWaitBits(g_evt, EV_DHT22_UPDATE_NOW, pdTRUE, pdFALSE, pdMS_TO_TICKS(DHT22_PERIOD_S * 1000));

        bool force = (bits & EV_DHT22_UPDATE_NOW) != 0; // 是否立即补采

        /* 低功耗期间: 只有收到"立即补采"请求才采集, 否则跳过 */
        if (s_lowpower && !force)
            continue;

        if (Service_Room_Update()) /* DHT22 读取, 成功失败都会刷新room_info */
        {
            xEventGroupSetBits(g_evt, EV_DHT22);
        }
    }
}

/* ==================== 任务间共享状态 ==================== */
static TaskHandle_t s_light_task = NULL;         // 光敏任务句柄, 供中断回调与按键任务通知
static TaskHandle_t s_key_task = NULL;           // 按键任务句柄, 供中断回调通知
static volatile bool s_key_toggle_night = false; // 按键请求: 切换昼夜(交光敏任务统一裁决)
static bool s_night = false;                     // 当前昼夜模式(初值与UI默认的白天一致)
static bool s_baseline_dark = false;             // 自动判定基线 = 当前已生效的光照状态

/* ==================== Light Sensor Task ==================== */

/** @brief 光敏中断回调(ISR上下文): 仅通知任务, 去抖在任务内完成
 *  @note 仅通知任务, 不处理中断逻辑, 任务内会去抖并根据状态切换uiTask的显示
 */
static void Light_IRQ_Notify(void)
{
    BaseType_t woken = pdFALSE; // 是否唤醒了更高优先级的任务

    if (s_light_task != NULL)
    {
        vTaskNotifyGiveFromISR(s_light_task, &woken); /* 通知正在阻塞的任务,解除阻塞状态 */
        portYIELD_FROM_ISR(woken);                    /* 请求一次上下文切换 */
    }
}

/** @brief 提交一次昼夜切换: 置事件位通知 UI, 并等待 UI 确认
 */
static void App_Apply_Night(bool night)
{
    xEventGroupSetBits(g_evt, night ? EV_LOWERPOWER : EV_WAKEUP);                      /* 通知UI切换昼夜 */
    xEventGroupWaitBits(g_evt, EV_LOWPOWER_ACK, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000)); /* 等待UI确认 */
}

/** @brief 处理按键的"手动切换昼夜"请求
 *  @details 翻转状态, 并把当前光照记为自动判定基线, 使光敏不会立刻把它改回去;
 *           只有环境光真正变化后, 自动跟随才重新生效
 *  @return true=本次处理了按键请求
 */
static bool Light_HandleKey(void)
{
    if (!s_key_toggle_night) /* 无按键请求 */
        return false;

    s_key_toggle_night = false;              // 清除请求标志
    s_night = !s_night;                      // 切换昼夜状态
    s_baseline_dark = Light_Sensor_IsDark(); // 记录当前光照状态, 作为自动判定基线
    printf("[LIGHT] key switch -> %s\r\n", s_night ? "night" : "day");
    App_Apply_Night(s_night);
    return true;
}

static void LightSensor_Task(void *pvParameters)
{
    (void)pvParameters;

    printf("[LIGHT] LightSensor_Task start\r\n");

    s_light_task = xTaskGetCurrentTaskHandle();      // 保存当前任务句柄, 用于中断回调
    Light_Sensor_RegisterCallback(Light_IRQ_Notify); /* 注册中断回调函数 */
    Light_Sensor_Init();                             /* 初始化光敏传感器 */

    /* 基线取反: 使第4步的"与基线相同则跳过"不成立, 强制上电先判定一次 */
    s_baseline_dark = !Light_Sensor_IsDark(); // 第一次上电值为 true, 使第4步的"与基线相同则跳过"不成立

    printf("[LIGHT] task ready: DO_state=%d dark=%d\r\n",
           (int)Light_Sensor_DO_State, (int)Light_Sensor_IsDark());

    for (;;)
    {
        /* 1. 按键优先: 处理上一轮遗留的切换请求, 处理完重新等待 */
        if (Light_HandleKey())
            continue;

        /* 2. 等事件: 光照中断 / 按键唤醒 / 1000ms 轮询兜底(EXTI 失效时仍能切换) */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        /* 3. 等待期间可能被按键唤醒 */
        if (Light_HandleKey())
            continue;

        /* 4. 光照与基线一致(含边沿抖动), 无需处理 */
        if (Light_Sensor_IsDark() == s_baseline_dark)
            continue;

        /* 5. 2s 去抖: 期间再来光照边沿则重新计时, 来按键则立即结束 */
        TickType_t t0 = xTaskGetTickCount();
        for (;;)
        {
            TickType_t remain = pdMS_TO_TICKS(DEBOUNCE_MS) - (xTaskGetTickCount() - t0); // 去抖时间剩余
            if ((int32_t)remain <= 0)
                break;
            if (ulTaskNotifyTake(pdTRUE, remain) > 0) /* 在剩余去抖时间内有通知 */
            {
                if (s_key_toggle_night)
                    break;                // 按键请求优先: 立即结束去抖(退出当前循环)
                t0 = xTaskGetTickCount(); // 新边沿, 重新去抖
            }
        }

        /* 6. 若因按键结束去抖, 回顶部交给第1步统一处理 */
        if (s_key_toggle_night)
            continue;

        /* 7. 去抖后再确认一次, 电平回到基线则本次判定作废 */
        bool dark = Light_Sensor_IsDark(); // 确认当前光照状态
        if (dark == s_baseline_dark)
            continue;

        s_baseline_dark = dark; // 更新自动判定基线
        printf("[LIGHT] auto: level=%s, mode=%s\r\n",
               dark ? "dark" : "bright", s_night ? "night" : "day");

        if (dark != s_night) /* 当前光照状态和当前昼夜模式不同 */
        {
            s_night = dark; // 更新昼夜模式
            printf("[LIGHT] auto request %s\r\n", s_night ? "NIGHT" : "DAY");
            App_Apply_Night(s_night); /* 提交一次昼夜切换 */
        }
    }
}

/* ==================== Key Task ==================== */

/** @brief 按键中断回调(ISR上下文): 仅通知任务
 *  @note 按下与松开都会触发, 手势识别在任务上下文完成
 */
static void Key_IRQ_Notify(void)
{
    BaseType_t woken = pdFALSE; // 是否唤醒任务

    if (s_key_task != NULL)
    {
        vTaskNotifyGiveFromISR(s_key_task, &woken); /* 通知正在阻塞的任务,解除阻塞状态 */
        portYIELD_FROM_ISR(woken);                  /* 请求一次上下文切换 */
    }
}

/** @brief 把累计击数映射为手势枚举 */
static Key_Gesture_t Key_ClickGesture(uint8_t clicks)
{
    switch (clicks)
    {
    case 1:
        return KEY_GESTURE_CLICK_1;
    case 2:
        return KEY_GESTURE_CLICK_2;
    default:
        return KEY_GESTURE_CLICK_3;
    }
}

static void Key_FullRefresh(void)
{
    xEventGroupSetBits(g_evt, EV_NET_UPDATE_NOW | EV_DHT22_UPDATE_NOW);
}

/** @brief 手势分发
 *  @note 目前只接单击(切换昼夜), 双击/三击/长按留作扩展点
 */
static void Key_OnGesture(Key_Gesture_t gesture)
{
    switch (gesture)
    {
    case KEY_GESTURE_CLICK_1: /* TODO: 待分配 */
        break;
    case KEY_GESTURE_CLICK_2:
    {
        printf("[KEY]Refresh all data\r\n");
        Key_FullRefresh(); /* 刷新所有数据 */
        break;
    }
    case KEY_GESTURE_CLICK_3: /* TODO: 待分配 */
        break;
    case KEY_GESTURE_LONG:
    {
        /* 交光敏任务统一裁决: 昼夜状态机在它手里, 避免两个任务各自改状态 */
        s_key_toggle_night = true;
        if (s_light_task != NULL)
            xTaskNotifyGive(s_light_task); /* 通知光敏任务切换昼夜 */
        break;
    }
    case KEY_GESTURE_NONE:
        break;
    default:
        break;
    }
}

/** @brief 按键手势识别
 *  @details 时序: 等按下 → 长按阈值内松开算短按, 超时即触发长按(按住即响应)
 *                → 连击窗口内再次按下则累加, 窗口超时后结算累计击数
 */
static void Key_Task(void *pvParameters)
{
    (void)pvParameters;

    uint8_t clicks = 0;

    s_key_task = xTaskGetCurrentTaskHandle(); // 保存任务句柄, 用于中断回调
    Key_RegisterCallback(Key_IRQ_Notify);     /* 注册中断回调函数 */
    Key_Init();                               /* 初始化按键(PA0 + EXTI0) */
    
    printf("[KEY] Key_Task start\r\n");
    printf("[KEY] task ready: pressed=%d\r\n", (int)Key_IsPressed());

    for (;;)
    {
        /* 已累计击数时只等连击窗口, 否则无限等待下一次按下 */
        TickType_t wait = (clicks == 0) ? portMAX_DELAY : pdMS_TO_TICKS(KEY_MULTI_GAP_MS);

        /* 连击窗口超时: 期间没有新的按键按下 → 结算当前累计击数。
         * 若有按键按下, ulTaskNotifyTake 会立即返回 > 0, 不进入此分支,
         * 由下方代码继续识别并累加 clicks, 直到某次窗口超时才结算。 */
        if (ulTaskNotifyTake(pdTRUE, wait) == 0)
        {
            if (clicks > 0)
            {
                Key_OnGesture(Key_ClickGesture(clicks)); /* 连击窗口超时: 结算 */
                clicks = 0;
            }
            continue;
        }

        /* 确认当前是否为按下状态 */
        if (!Key_IsPressed())
            continue; // 边沿抖动或误触发: 不是真正的按下

        /* 长按阈值内松开 → 短按; 超时 → 长按(按住即触发) */
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(KEY_LONG_PRESS_MS)) == 0)
        {
            Key_OnGesture(KEY_GESTURE_LONG);

            /* 等松开, 并清掉松开时的残留通知, 避免影响下一次判定 */
            while (Key_IsPressed())
                (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(KEY_RELEASE_POLL_MS));
            (void)ulTaskNotifyTake(pdTRUE, 0);
            clicks = 0;
            continue;
        }

        clicks++;
        if (clicks >= KEY_CLICK_MAX)
        {
            Key_OnGesture(Key_ClickGesture(clicks)); /* 达到上限: 立即结算 */
            clicks = 0;
        }
    }
}

/* ==================== netTask ==================== */

static void Net_Task(void *pvParameters)
{
    (void)pvParameters;

    printf("[NET] Net_Task start\r\n");

    uint32_t sntp_c, wifi_c, weather_c;
    bool wifi_up;
    bool wifi_sleeping = false; /* 模组当前是否处于省电档位(仅本任务访问) */

    printf("[NET] Init Start\r\n");

    /* 开机阶段: AT/WiFi初始化 + 连接 */
    s_boot.wifi_ok = Wireless_Init(); // 此处 s_boot.wifi_ok 表示初始化无线网络是否成功
    if (s_boot.wifi_ok)
        s_boot.wifi_ok = Service_WiFi_Connect(); // 此处 s_boot.wifi_ok 表示连接WiFi是否成功
    wifi_up = s_boot.wifi_ok;                    // 是否连接成功

    /* 开机阶段: SNTP + 天气(需WiFi) */
    if (wifi_up)
        AT_SNTP_Init();
    bool t_ok = wifi_up && Service_Time_Sync();      // SNTP同步是否成功
    bool w_ok = wifi_up && Service_Weather_Update(); // 天气更新是否成功

    s_boot.service_ok = wifi_up && t_ok && w_ok; // 是否所有服务都启动成功

    /* 通知UI: 开机阶段完成(无论成败), 之后后台继续重试 */
    xEventGroupSetBits(g_evt, EV_NET_READY);

    /*  周期调度初值  */
    wifi_c = wifi_up ? WIFI_PERIOD_S : RETRY_WIFI_S;       // WiFi检查周期,连接上30min,未连10s
    sntp_c = t_ok ? SNTP_PERIOD_S : RETRY_SNTP_S;          // SNTP周期,成功后4h,未成功5s
    weather_c = w_ok ? WEATHER_PERIOD_S : RETRY_WEATHER_S; // 天气周期,成功后1h,未成功60s

    for (;;)
    {
        /* 1s 心跳; 同时等待"退出低功耗立即补更"请求(netTask 专属位) */
        EventBits_t bits = xEventGroupWaitBits(g_evt, EV_NET_UPDATE_NOW, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));

        /* 低功耗状态变化 → 同步模组的Wi-Fi省电档位。
         * 必须放在补更之前: 退出夜间时先让模组恢复全速, 再执行 WiFi/SNTP/天气 */
        if (s_lowpower != wifi_sleeping)
        {
            if (Service_WiFi_Sleep(s_lowpower))
                wifi_sleeping = s_lowpower; /* 失败则保持原状态, 下个周期自动重试 */
        }

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
            if (r < 0) /* 连接失败 */
            {
                wifi_up = false;
                wifi_c = RETRY_WIFI_S;
            }
            else /* 连接成功 */
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
            if (r != 0 || !was_up)
                xEventGroupSetBits(g_evt, EV_WIFI); /* 状态变化/首次连上: 刷顶部条 */
        }

        if (--sntp_c == 0U)
        {
            if (!wifi_up)
            {
                sntp_c = 1U; // WiFi未连, 每秒顺延, 等wifi恢复后触发
            }
            else if (Service_Time_Sync()) /* SNTP同步成功 */
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
                weather_c = 1U; // WiFi未连, 每秒顺延, 等wifi恢复后触发
            }
            else if (Service_Weather_Update()) /* 天气更新 */
            {
                weather_c = WEATHER_PERIOD_S;
                xEventGroupSetBits(g_evt, EV_WEATHER); /* 天气更新成功: 刷新天气部分UI */
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
    OLED_ShowClock(t.hour, t.minute, t.year, t.month, t.day); /* 显示时间+日期 */

    /* 关闭LCD，并置全局状态位 */
    ST7789_Display_Power(false); /* 关闭背光 + 0x28 显示睡眠 */
    s_lcd_on = false;            // 关闭LCD
    s_oled_on = true;            // 显示OLED

    printf("[LP] Enter night: LCD off, OLED on, time_source=%s\r\n", time_source ? "RTC" : "SoftClock");
}

/* 回到白天: OLED关闭, LCD恢复并全量重绘 */
static void UI_Enter_Day(void)
{
    OLED_Display_Off();

    ST7789_Display_Power(true); /* 0x29 显示开 + 开背光 */

    uint32_t prof_t0 = Prof_Cycles(); /* 整页重绘耗时(DWT CYCCNT) */
    Main_Page_Display();              /* 夜间未刷新, 全量重绘一次 */
    printf("[PROF] main page render: %u us\r\n", (unsigned)Prof_Us(prof_t0));

    s_lcd_on = true;   // 开启LCD
    s_oled_on = false; // 关闭OLED

    /* 通知网络/传感器更新数据 */
    s_lowpower = false; // 退出低功耗模式
    xEventGroupSetBits(g_evt, EV_NET_UPDATE_NOW | EV_DHT22_UPDATE_NOW);

    printf("[UI] Enter day: OLED off, LCD on\r\n");
}

/* ==================== uiTask(LCD唯一写者) ==================== */

static void UI_Task(void *pvParameters)
{
    (void)pvParameters;

    EventBits_t bits = 0; // 事件位, 用于等待uiTask的事件

    g_evt = xEventGroupCreate(); // 事件组: 仅被等待的bit置位才会唤醒

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
    xTaskCreate(DHT22_Task, "dht22", DHT22_TASK_STACK_SIZE, NULL, DHT22_TASK_PRIORITY, &dht22_task_handle);
    xTaskCreate(Net_Task, "net", NET_TASK_STACK_SIZE, NULL, NET_TASK_PRIORITY, &net_task_handle);

    /* 等待开机网络阶段结束(成功或失败都置EV_NET_READY), 超时30s兜底 */
    xEventGroupWaitBits(g_evt, EV_NET_READY, pdTRUE, pdFALSE, pdMS_TO_TICKS(30 * 1000));
    printf("[UI] Boot net stage done: wifi=%d service=%d\r\n", (int)s_boot.wifi_ok, (int)s_boot.service_ok);

    Boot_Page_Show(s_boot.wifi_ok, s_boot.service_ok); /* 开机结果(连接详情) */
    vTaskDelay(pdMS_TO_TICKS(2500));                   /* 结果页停留, 便于查看详情 */
    printf("[UI] Enter main page\r\n");

    uint32_t prof_t0 = Prof_Cycles(); /* 整页重绘耗时(DWT CYCCNT) */
    Main_Page_Display();              /* 进入主页面(数据已就绪, 首绘即正确) */
    printf("[PROF] main page render: %u us\r\n", (unsigned)Prof_Us(prof_t0));

    /* 进入主页面后再启动光敏任务与按键任务: 保证昼夜切换只发生在开机完成之后 */
    xTaskCreate(LightSensor_Task, "light_sensor", LIGHT_SENSOR_TASK_STACK_SIZE, NULL, LIGHT_SENSOR_TASK_PRIORITY, &lightsensor_task_handle);
    xTaskCreate(Key_Task, "key", KEY_TASK_STACK_SIZE, NULL, KEY_TASK_PRIORITY, &key_task_handle);

    for (;;)
    {
        /* 低功耗模式: 事件等待放宽(夜间只显示分钟); 非低功耗: 500ms心跳 */
        TickType_t wait = s_lowpower ? pdMS_TO_TICKS(EV_LP_UI_TICK_MS) : pdMS_TO_TICKS(500);

        bits = xEventGroupWaitBits(g_evt,
                                   EV_WEATHER | EV_DHT22 | EV_WIFI | EV_LOWERPOWER | EV_WAKEUP,
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
                Main_Page_Net_Update(); /* WiFi连接状态变化: 刷顶部WiFi/定位条 */
            if (bits & EV_WEATHER)
            {
                Main_Page_Weather_Update();
                Main_Page_Net_Update(); /* 天气到位后同步城市/定位图标 */
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

void App_Task_Init(void)
{
    xTaskCreate(UI_Task, "ui", UI_TASK_STACK_SIZE, NULL, UI_TASK_PRIORITY, &ui_task_handle);
}
