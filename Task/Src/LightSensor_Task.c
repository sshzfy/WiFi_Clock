#include "LightSensor_Task.h"

/* ==================== 任务间共享状态(仅本文件内) ==================== */
static TaskHandle_t s_light_task = NULL;         // 光敏任务句柄, 供中断回调与按键请求通知
static volatile bool s_key_toggle_night = false; // 按键请求: 切换昼夜(由本任务统一裁决)
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
    xEventGroupSetBits(Task_Events(), night ? EV_LOWERPOWER : EV_WAKEUP);                      /* 通知UI切换昼夜 */
    xEventGroupWaitBits(Task_Events(), EV_LOWPOWER_ACK, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000)); /* 等待UI确认 */
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

    /* 丢弃就绪前遗留的按键请求, 避免它把下面的"基线取反"误当成环境光变化的依据 */
    s_key_toggle_night = false;

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

void LightSensor_Task_Start(void)
{
    xTaskCreate(LightSensor_Task, "light_sensor", LIGHT_SENSOR_TASK_STACK_SIZE, NULL, LIGHT_SENSOR_TASK_PRIORITY, NULL);
}

void LightSensor_Task_RequestToggleNight(void)
{
    s_key_toggle_night = true;
    if (s_light_task != NULL)
        xTaskNotifyGive(s_light_task); /* 通知光敏任务切换昼夜 */
}
