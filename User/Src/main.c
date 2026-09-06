#include "main.h"
#include "Board.h"
#include "app_task.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

int main(void)
{
    Board_Peripheral_Init();                        // 外设时钟初始化
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4); // 4位抢占,无子优先级(FreeRTOS要求)

    App_Task_Init(); // 创建UI任务(内部再创建net/sensor任务)

    vTaskStartScheduler(); // 启动FreeRTOS调度器

    while (1)
        ; // code should never reach here
}


void vAssertCalled(const char *file, int line)
{
    /* Assertion failed callback (configASSERT). */
    printf("Assertion failed in file %s at line %d\n", file, line);
    while (1)
        ;
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    /* Task stack overflow callback. */
    (void)xTask;
    printf("Stack overflow in task %s\n", pcTaskName);
    while (1)
        ;
}
