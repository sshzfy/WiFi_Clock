#include "DHT22_Task.h"

/* ==================== DHT22 Task(周期采集) ==================== */

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
        /* 等待超时(=采集周期); 同时等待"退出低功耗立即补采"请求 */
        EventBits_t bits = xEventGroupWaitBits(Task_Events(), EV_DHT22_UPDATE_NOW, pdTRUE, pdFALSE, pdMS_TO_TICKS(DHT22_PERIOD_S * 1000));

        bool force = (bits & EV_DHT22_UPDATE_NOW) != 0; // 是否立即补采

        /* 低功耗期间: 只有收到"立即补采"请求才采集, 否则跳过 */
        if (APP_IS_LOWPOWER() && !force)
            continue;

        if (Service_Room_Update()) /* DHT22 读取, 成功失败都会刷新room_info */
        {
            xEventGroupSetBits(Task_Events(), EV_DHT22);
        }
    }
}

/* DHT22 温湿度采集任务(优先级高于UI, 保护us级位操作时序) */
void DHT22_Task_Start(void)
{
    xTaskCreate(DHT22_Task, "dht22", DHT22_TASK_STACK_SIZE, NULL, DHT22_TASK_PRIORITY, NULL);
}
