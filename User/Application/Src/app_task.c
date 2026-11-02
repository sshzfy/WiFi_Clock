#include "app_task.h"

#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx.h"

#include "boot_layout.h"
#include "boot_param.h"
#include "OTA.h"
#include "Version.h"
#include "BuildConfig.h"

/* App 侧配合 Bootloader 的两件事：
 *   1. 把 VTOR 重新指回 App：SystemInit 会把它改回 0x08000000；
 *   2. 新固件启动 5 秒后写 CONFIRMED 和自身 size/crc/version，
 *      否则 Bootloader 会累加 boot_try，3 次后回滚。 */

/** @brief  把中断向量表重新指向 App 起始地址 */
static void Boot_Apply_VectorTable(void)
{
    /* SystemInit 里 VECT_TAB_OFFSET=0，会把 VTOR 设回 0x08000000，
     * 这里在开中断/启动调度器之前修正。 */
    SCB->VTOR = APP_BASE;
    __DSB();
    __ISB();
}

/** @brief  新固件启动确认任务 */
static void Boot_Confirm_Task(void *pvParameters)
{
    (void)pvParameters;

    vTaskDelay(pdMS_TO_TICKS(5000));

    /* 计算自身固件大小与 CRC
     * 连同 CONFIRMED 一起写入参数区，Bootloader 用它显示版本。 */
    {
        uint32_t size = 0U;
        uint32_t crc = 0U;

        if (OTA_CalcAppInfo(&size, &crc))
            (void)Boot_Param_SetAppInfo(size, crc, APP_VERSION);
        else
            (void)Boot_Param_Confirm();
    }

    vTaskDelete(NULL);
}

/**
 * @brief  初始化应用任务（调度器启动前，由 main 调用）
 * @note   顺序：修正 VTOR -> 创建确认任务（优先级 1 最低档，
 *         只延迟等待，不抢业务任务）-> UI 任务。
 */
void App_Task_Init(void)
{
    Boot_Apply_VectorTable();

    xTaskCreate(Boot_Confirm_Task, "boot_cfm", 128, NULL, 1, NULL);

    UI_Task_Start();
}
