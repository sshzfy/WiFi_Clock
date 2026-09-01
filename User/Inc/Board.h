#ifndef __BOARD_H__
#define __BOARD_H__

#include "stm32f4xx.h"
#include "delay.h"
#include "ST7789.h"

void Board_Peripheral_Init(void);
void Board_Init(void);
void Test(void);

#endif /* __BOARD_H__ */
