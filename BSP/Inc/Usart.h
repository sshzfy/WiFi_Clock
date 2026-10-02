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

/* USART3: 定位模块(ATGM336H)专用。模块出厂默认9600-8N1, 改过配置时只改这里 */
#define USART3_BAUD 9600U

void Usart3_Init(void);
int Usart3_RX_Read(void);

#endif /* __USART_H__ */
