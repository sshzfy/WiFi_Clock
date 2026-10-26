#include "boot_uart.h"

#include "stm32f4xx.h"

#include <stddef.h>

/* ============================================================
 * Bootloader 串口日志
 * ------------------------------------------------------------
 * 轮询发送:
 *     Bootloader 只在启动/搬运时打几行日志，字节少、阻塞
 *     几十微秒无所谓，轮询代码最小、行为最确定。
 * ============================================================ */

/**
 * @brief  初始化 USART2（PA2/PA3），115200-8N1，不开中断
 * @note   与 App 的调试串口同路；与 OTA 串口（OTA_Serial）互斥。
 */
void Boot_Uart_Init(void)
{
    GPIO_InitTypeDef gpio;
    USART_InitTypeDef usart;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);

    GPIO_PinAFConfig(GPIOA, GPIO_PinSource2, GPIO_AF_USART2);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource3, GPIO_AF_USART2);

    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin = GPIO_Pin_2 | GPIO_Pin_3;
    gpio.GPIO_Mode = GPIO_Mode_AF;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOA, &gpio);

    USART_StructInit(&usart);
    usart.USART_BaudRate = 115200U;
    usart.USART_WordLength = USART_WordLength_8b;
    usart.USART_StopBits = USART_StopBits_1;
    usart.USART_Parity = USART_Parity_No;
    usart.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_Init(USART2, &usart);

    USART_Cmd(USART2, ENABLE);
}

/**
 * @brief  发送一个字节
 * @param  c 待发送字符
 */
void Boot_Uart_PutChar(char c)
{
    while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET)
    {
    }

    USART_SendData(USART2, (uint16_t)(uint8_t)c);
}

/**
 * @brief  发送字符串
 * @param  s 以 '\0' 结尾的字符串
 * @note   遇到 '\n' 自动补 '\r'；末尾等 TC，保证最后一个字节
 *         真正发完（否则紧接着跳转 App 可能截断输出）。
 */
void Boot_Uart_Puts(const char *s)
{
    if (s == NULL)
        return;

    while (*s != '\0')
    {
        if (*s == '\n')
            Boot_Uart_PutChar('\r');
        Boot_Uart_PutChar(*s++);
    }

    while (USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET)
    {
    }
}

/**
 * @brief  十进制输出 32 位无符号数
 * @param  v 数值
 */
void Boot_Uart_PutU32(uint32_t v)
{
    char buf[11] = {0};
    uint32_t i = 0U;

    if (v == 0U)
    {
        Boot_Uart_PutChar('0');
        return;
    }

    while (v != 0U && i < sizeof(buf))
    {
        buf[i++] = (char)('0' + (v % 10U));
        v /= 10U;
    }

    while (i > 0U)
        Boot_Uart_PutChar(buf[--i]);
}

/**
 * @brief  以 0xXXXXXXXX 形式输出 32 位无符号数
 * @param  v 数值
 * @note   版本号、CRC 用十六进制更直观。
 */
void Boot_Uart_PutHex(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";

    Boot_Uart_Puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4)
        Boot_Uart_PutChar(hex[(v >> shift) & 0x0FU]); // 四位二进制表示一位十六进制
}
