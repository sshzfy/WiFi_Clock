#include "main.h"
#include "misc.h"
#include "Board.h"
#include "Timer.h"
#include "BuildConfig.h"
#include "FreeRTOS.h"
#include "task.h"

#if (USE_FREERTOS == 1)

#include "app_task.h"

int main(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    Board_Peripheral_Init(); /* 外设时钟初始化 */

    App_Task_Init();         /* 创建网络/传感器/UI任务, 并启动FreeRTOS调度器 */
    vTaskStartScheduler();   /* 启动FreeRTOS调度器, 任务开始运行 */

    while (1)
        ; /* never reached */
}

#else /* USE_FREERTOS == 0 : 模块测试 */

#include "bare_test.h"

int main(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    Board_Peripheral_Init(); /* 外设时钟初始化 */
    Test();
    TIM5_Init();             /* 1ms时间定时器初始化 */

    BareMetal_Module_Test(); /* 选中的模块测试, 自定义循环 */

    while (1)
        ; /* never reached */
}

#endif /* USE_FREERTOS */

void vAssertCalled(const char *file, int line)
{
    printf("Assertion failed in file %s at line %d\n", file, line);
    while (1)
        ;
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("Stack overflow in task %s\n", pcTaskName);
    while (1)
        ;
}
