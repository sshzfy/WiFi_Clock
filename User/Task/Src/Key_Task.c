#include "Key_Task.h"

/* ==================== Key Task ==================== */

static TaskHandle_t s_key_task = NULL; // 按键任务句柄, 供中断回调通知

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
    xEventGroupSetBits(Task_Events(), EV_NET_UPDATE_NOW | EV_DHT22_UPDATE_NOW);
}

/** @brief 手势分发
 *  @note 目前只接单击(切换昼夜), 双击/三击/长按留作扩展点
 */
void Key_Task_OnGesture(Key_Gesture_t gesture)
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
        LightSensor_Task_RequestToggleNight();
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
                Key_Task_OnGesture(Key_ClickGesture(clicks)); /* 连击窗口超时: 结算 */
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
            Key_Task_OnGesture(KEY_GESTURE_LONG);

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
            Key_Task_OnGesture(Key_ClickGesture(clicks)); /* 达到上限: 立即结算 */
            clicks = 0;
        }
    }
}

void Key_Task_Start(void)
{
    xTaskCreate(Key_Task, "key", KEY_TASK_STACK_SIZE, NULL, KEY_TASK_PRIORITY, NULL);
}
