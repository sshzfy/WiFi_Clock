#ifndef __BOARD_H__
#define __BOARD_H__

#include "main.h"
#include "stm32f4xx.h"
#include "LCD.h"
#include "Timer.h"
#include "Usart.h"

void Board_Peripheral_Init(void);
void Board_Init(void);
void Test(void);

#endif /* __BOARD_H__ */
