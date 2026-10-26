#ifndef __BOOT_UART_H__
#define __BOOT_UART_H__

#include <stdint.h>

void Boot_Uart_Init(void);
void Boot_Uart_PutChar(char c);
void Boot_Uart_Puts(const char *s);
void Boot_Uart_PutU32(uint32_t v);
void Boot_Uart_PutHex(uint32_t v);

#endif
