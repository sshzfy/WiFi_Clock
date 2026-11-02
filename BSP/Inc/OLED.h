#ifndef __OLED_H__
#define __OLED_H__

#include "main.h"
#include "I2C.h"
#include "Font.h"
#include "Timer.h"
#include "App.h"
#include "AT.h"
#include "Asset.h"

/* OLED 引脚定义 */
#define OLED_GPIO_PORT           GPIOB
#define OLED_GPIO_SCL_PIN        GPIO_Pin_6
#define OLED_GPIO_SDA_PIN        GPIO_Pin_7

#define OLED_ADDR               0x3C // OLED 地址
#define OLED_CMD_BYTE           0x00 // 命令字节
#define OLED_DATA_BYTE          0x40 // 数据字节

#define OLED_WIDTH              128 // OLED 宽度
#define OLED_HEIGHT             64  // OLED 高度

extern Soft_I2C_t oled_i2c;

void OLED_Clear(void);
void OLED_WriteChar(uint8_t x, uint8_t y, char ch, const Font_t *font);
void OLED_Write_String(uint8_t x, uint8_t y, const char *str, const Font_t *font);
void OLED_Display_On(void);
void OLED_Display_Off(void);
void OLED_ShowClock(uint8_t hh, uint8_t mm, uint16_t year, uint8_t mon, uint8_t day);
void OLED_Init(void);

#endif /* __OLED_H__ */
