#ifndef __DELAY_H__
#define __DELAY_H__

#include "main.h"

/*============================== SysTick_Delay_2 ==============================*/
void SysTick_Init(void);
uint64_t Get_SysTick_Cnt(void);
uint64_t Get_us(void);
uint64_t Get_ms(void);
void delay_us(uint32_t us);
void delay_ms(uint32_t ms);

#endif /* __DELAY_H__ */
