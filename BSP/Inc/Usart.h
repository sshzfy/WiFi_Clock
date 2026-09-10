#ifndef __USART_H__
#define __USART_H__

#include "main.h"
#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

void Usart2_Debug_Init(void);

/* USART1 RX 环形缓冲接口(AT层使用) */
uint16_t Usart1_RX_Count(void);
char Usart1_RX_Read(void);

#endif /* __USART_H__ */
