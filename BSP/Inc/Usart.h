#ifndef __USART_H__
#define __USART_H__

#include "main.h"

typedef void (*Usart_Received_Callback_t)(char ch);

void Usart_Init(void);
void Usart2_Debug_Init(void);
void Usart_Received_Register(Usart_Received_Callback_t callback);


#endif /* __USART_H__ */
