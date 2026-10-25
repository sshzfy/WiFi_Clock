#ifndef __BOOT_UART_H__
#define __BOOT_UART_H__

#include <stdint.h>

/* Bootloader 专用轮询串口：USART2，PA2=TX / PA3=RX，115200-8N1。
 * 不复用 BSP/Usart.c，避免拖入 FreeRTOS/DMA/printf 依赖。 */

void Boot_Uart_Init(void);
void Boot_Uart_PutChar(char c);
void Boot_Uart_Puts(const char *s);
void Boot_Uart_PutU32(uint32_t v);
void Boot_Uart_PutHex(uint32_t v);

#endif
