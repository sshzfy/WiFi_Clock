#ifndef __USART_H__
#define __USART_H__

#include "main.h"
#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

void USART1_Init(void);
uint16_t Usart1_RX_Count(void);
int Usart1_RX_Read(void);
void Usart1_RX_Flush(void);
void Usart2_Debug_Init(void);

#endif /* __USART_H__ */
