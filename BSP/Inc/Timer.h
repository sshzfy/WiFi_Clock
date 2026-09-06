#ifndef __TIMER_H__
#define __TIMER_H__

#include "stm32f4xx.h"
#include <stddef.h>

void TIM5_Init(void);
uint64_t TIM5_Get_ms(void);
uint64_t TIM5_Get_us(void);
void delay_us(uint32_t us);
void delay_ms(uint32_t ms);

#endif /* __TIMER_H__ */
