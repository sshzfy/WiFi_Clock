#include "OTA_Task.h"

#include "OTA.h"
#include "OTA_Serial.h"

#include "FreeRTOS.h"
#include "task.h"

/* ============================================================
 * OTA 任务
 * ------------------------------------------------------------
 * 优先级 2：低于 ui(3)、dht22(4)，与 net 同级。
 *   - 不能高于 ui：下载期间 ui 每 50 ms 还要刷屏/轮询触摸；
 *   - 不担心被抢占：字节由 USART2 中断收进 4 KB 环形缓冲，
 *     任务被抢占毫秒不会丢数据
 * ============================================================ */

/* 失败应答：OTA_Run 出错返回后统一回复 PC */
static const uint8_t s_fail[6] = {0xAA, 0x55, 'F', 'A', 'I', 'L'};

/**
 * @brief  OTA 任务主体
 * @note   循环等待握手 -> 执行会话 -> 失败回 FAIL -> 清缓冲重来。
 *         升级成功会在 OTA_Run 里复位，OTA_OK 实际上不会返回。
 */
static void OTA_Task(void *pvParameters)
{
    (void)pvParameters;

    for (;;)
    {
        /* 阻塞在握手等待上；升级成功会在 OTA_Run 里复位，
         * 因此 OTA_OK 实际上永远不会返回。 */
        ota_result_t ret = OTA_Run();

        if (ret != OTA_OK)
            OTA_Serial_Send(s_fail, sizeof(s_fail));

        /* 一次会话结束：清掉缓冲里残留的半帧数据，
         * 稍等片刻再回到 OTA_Run 等待下一次握手。 */
        OTA_Serial_Flush();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief  初始化 OTA 串口并创建任务
 * @note   由 app_task.c 在 OTA_SERIAL_TEST=1 时调用。
 *         初始化在调度器启动前做（App_Task_Init 上下文），
 *         中断使能后、任务运行前收到的字节会先进环形缓冲，不会丢。
 */
void OTA_Task_Start(void)
{
    OTA_Serial_Init();
    xTaskCreate(OTA_Task, "ota", 512, NULL, 2, NULL);
}
