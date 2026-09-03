#include "main.h"
#include "Board.h"
#include "loop.h"

int main(void)
{
    Board_Peripheral_Init(); // 外设时钟初始化
    Board_Init();            // SysTick/TIM5/LCD/USART2 初始化
    loop();                  // 主页循环(永不返回)

    return 0;
}


void vAssertCalled(const char *file, int line)
{
    /* 断言失败处理函数 */
    printf("Assertion failed in file %s at line %d\n", file, line);
    while (1)
        ;
}
