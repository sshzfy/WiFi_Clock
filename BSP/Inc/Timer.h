#ifndef __TIMER_H__
#define __TIMER_H__

#include "main.h"

typedef void (*period_callback_t)(void);

void TIM5_Init(void);
uint64_t TIM5_Get_ms(void);
void register_period_callback(period_callback_t callback);

#endif /* __TIMER_H__ */
