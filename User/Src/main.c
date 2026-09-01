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

int fputc(int ch, FILE *stream)
{
    (void)stream;

    USART_ClearFlag(USART2, USART_FLAG_TXE);
    USART_SendData(USART2, (uint16_t)ch);
    while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET)
        ;

    return ch;
}
