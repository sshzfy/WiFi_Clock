#include "main.h"
#include "Board.h"
#include "Timer.h"
#include "BuildConfig.h"
#include "FreeRTOS.h"
#include "task.h"

#if (USE_FREERTOS == 1)

#include "app_task.h"

int main(void)
{
    Board_Peripheral_Init(); /* clocks */

    App_Task_Init();         /* create net/sensor/UI tasks */
    vTaskStartScheduler();   /* start FreeRTOS scheduler */

    while (1)
        ; /* never reached */
}

#else /* USE_FREERTOS == 0 : bare-metal module test */

#include "bare_test.h"

int main(void)
{
    Board_Peripheral_Init(); /* clocks */
    TIM5_Init();             /* 1ms time base for delay_us/delay_ms */

    BareMetal_Module_Test(); /* selected module test, owns its own loop */

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
