#ifndef __OLED_H__
#define __OLED_H__

#include "main.h"
#include "I2C.h"
#include "Font.h"
#include "Timer.h"

#define OLED_ADDR 0x3C
#define OLED_CMD_BYTE 0x00
#define OLED_DATA_BYTE 0x40

#define OLED_WIDTH 128
#define OLED_HEIGHT 64

void OLED_Clear(void);
void OLED_WriteChar(uint8_t x, uint8_t y, char ch, const Font_t *font);
void OLED_Write_String(uint8_t x, uint8_t y, const char *str, const Font_t *font);
void OLED_Init(Soft_I2C_t *i2c);

#endif /* __OLED_H__ */
