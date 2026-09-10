#ifndef __LCD_H__
#define __LCD_H__

#include "main.h"
#include "Timer.h"
#include "Font.h"
#include "Image.h"

/* ST7789 显示区域 */
#define WIDTH 240
#define HEIGHT 320

/* ST7789 端口定义 */
#define ST7789_SCLK_Port GPIOC
#define ST7789_SCLK_Pin GPIO_Pin_10

#define ST7789_MOSI_Port GPIOC
#define ST7789_MOSI_Pin GPIO_Pin_12

#define ST7789_MISO_Port GPIOC
#define ST7789_MISO_Pin GPIO_Pin_11

#define ST7789_CS_Port GPIOE
#define ST7789_CS_Pin GPIO_Pin_2

#define ST7789_RESET_Port GPIOE
#define ST7789_RESET_Pin GPIO_Pin_3

#define ST7789_DC_Port GPIOE
#define ST7789_DC_Pin GPIO_Pin_4

#define ST7789_BACKLIGHT_Port GPIOE
#define ST7789_BACKLIGHT_Pin GPIO_Pin_5

/* 颜色 */
#define COLOR(r, g, b) ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
#define COLOR_BLACK       COLOR(0, 0, 0)          // 纯黑
#define COLOR_RED         COLOR(255, 0, 0)        // 纯红
#define COLOR_DARKRED     COLOR(128, 0, 0)        // 深红
#define COLOR_MAROON      COLOR(196, 30, 30)      // 暗红
#define COLOR_TOMATO      COLOR(255, 80, 0)       // 番茄红
#define COLOR_ORANGERED   COLOR(255, 69, 0)       // 橙红
#define COLOR_MAGENTA     COLOR(255, 0, 255)      // 品红
#define COLOR_HOTPINK     COLOR(255, 105, 180)    // 亮粉红
#define COLOR_PINK        COLOR(255, 192, 203)    // 粉红
#define COLOR_ROSE        COLOR(255, 0, 127)      // 玫瑰红
#define COLOR_PURPLERED   COLOR(128, 0, 32)       // 紫红
//橙色系
#define COLOR_ORANGE      COLOR(255, 165, 0)      // 橙色
#define COLOR_DARKORANGE  COLOR(255, 140, 0)      // 暗橙
#define COLOR_GOLD        COLOR(255, 215, 0)      // 金色
#define COLOR_YELLOW      COLOR(255, 255, 0)      // 纯黄
#define COLOR_LIMEYELLOW  COLOR(255, 255, 100)    // 淡黄
#define COLOR_KHAKI       COLOR(240, 230, 140)    // 卡其黄
#define COLOR_SANDY       COLOR(210, 180, 140)    // 沙色
#define COLOR_BROWN       COLOR(165, 42, 42)      // 棕色
#define COLOR_SADDLEBROWN COLOR(139, 69, 19)      // 深棕色/赭石
//绿色系
#define COLOR_GREEN       COLOR(0, 255, 0)        // 纯绿
#define COLOR_DARKGREEN   COLOR(0, 128, 0)        // 深绿
#define COLOR_FORESTGREEN COLOR(34, 139, 34)      // 森林绿
#define COLOR_LIMEGREEN   COLOR(50, 205, 50)      // 亮绿/石灰绿
#define COLOR_YELLOWGREEN COLOR(173, 255, 47)     // 黄绿
#define COLOR_CHARTREUSE  COLOR(127, 255, 0)      // 草绿
#define COLOR_OLIVE       COLOR(128, 128, 0)      // 橄榄绿
#define COLOR_SEAGREEN    COLOR(46, 139, 87)      // 海绿
#define COLOR_SPRINGGREEN COLOR(0, 255, 127)      // 春绿/翠绿
//蓝色系
#define COLOR_BLUE        COLOR(0, 0, 255)        // 纯蓝
#define COLOR_NAVY        COLOR(0, 0, 128)        // 海军蓝
#define COLOR_DARKBLUE    COLOR(0, 0, 139)        // 深蓝
#define COLOR_ROYALBLUE   COLOR(65, 105, 225)     // 皇家蓝
#define COLOR_DODGERBLUE  COLOR(30, 144, 255)     // 道奇蓝/亮蓝
#define COLOR_SKYBLUE     COLOR(135, 206, 235)    // 天蓝
#define COLOR_DEEPSKYBLUE COLOR(0, 191, 255)      // 深天蓝
#define COLOR_CYAN        COLOR(0, 255, 255)      // 青色
#define COLOR_DARKCYAN    COLOR(0, 139, 139)      // 深青
#define COLOR_LIGHTCYAN   COLOR(224, 255, 255)    // 浅青
#define COLOR_AZURE       COLOR(240, 255, 255)    // 蔚蓝
//紫色系
#define COLOR_PURPLE      COLOR(128, 0, 128)      // 紫色
#define COLOR_VIOLET      COLOR(238, 130, 238)    // 紫罗兰
#define COLOR_ORCHID      COLOR(218, 112, 214)    // 兰花紫
#define COLOR_MEDIUMPURPLE COLOR(147, 112, 219)   // 中紫
#define COLOR_INDIGO      COLOR(75, 0, 130)       // 靛蓝
#define COLOR_DARKVIOLET  COLOR(148, 0, 211)      // 深紫罗兰
#define COLOR_LAVENDER    COLOR(230, 230, 250)    // 薰衣草淡紫
//白色系
#define COLOR_WHITE       COLOR(255, 255, 255)    // 纯白
#define COLOR_SNOW        COLOR(255, 250, 250)    // 雪白
#define COLOR_GAINSBORO   COLOR(220, 220, 220)    // 亮灰
#define COLOR_LIGHTGRAY   COLOR(192, 192, 192)    // 浅灰
#define COLOR_GRAY        COLOR(128, 128, 128)    // 中灰
#define COLOR_DIMGRAY     COLOR(105, 105, 105)    // 暗灰
#define COLOR_DARKGRAY    COLOR(64, 64, 64)       // 深灰
#define COLOR_SLATEGRAY   COLOR(112, 128, 144)    // 蓝灰
#define COLOR_WARMSKIN    COLOR(255, 225, 200)    // 暖肤色
#define COLOR_BEIGE       COLOR(245, 245, 220)    // 米色/沙色

/* ST7789 函数声明 */
void ST7789_Init(void);
void ST7789_Fill_Color(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void ST7789_Write_String(uint16_t x, uint16_t y, char *ch, uint16_t color_font, uint16_t color_back, const Font_t *font);
void ST7789_Draw_Picture(uint16_t x, uint16_t y, const Image_t *image);
void ST7789_Draw_Picture_AutoTransparent(uint16_t x, uint16_t y, const Image_t *image, uint16_t target_back);

#endif /* __LCD_H__ */
