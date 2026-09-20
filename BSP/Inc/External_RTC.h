#ifndef __EXTERNAL_RTC_H__
#define __EXTERNAL_RTC_H__

#include "main.h"
#include "stm32f4xx.h"
#include "Timer.h"

/* DS1302 寄存器地址 */
#define DS1302_REG_SEC                  0x80 // 秒寄存器
#define DS1302_REG_MIN                  0x82 // 分寄存器
#define DS1302_REG_HOUR                 0x84 // 时寄存器
#define DS1302_REG_DAY                  0x86 // 日寄存器
#define DS1302_REG_MONTH                0x88 // 月寄存器
#define DS1302_REG_DAY_OF_WEEK          0x8A // 星期寄存器
#define DS1302_REG_YEAR                 0x8C // 年寄存器
#define DS1302_REG_WP                   0x8E // 写保护寄存器
#define DS1302_REG_TRICKLK              0x90 // 涓流充电寄存器

/* DS1302 硬件引脚 */
#define DS1302_PORT                     GPIOE
#define DS1302_RST_PIN                  GPIO_Pin_7
#define DS1302_IO_PIN                   GPIO_Pin_8
#define DS1302_CLK_PIN                  GPIO_Pin_9

typedef struct
{
    uint8_t sec;
    uint8_t min;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
    uint8_t week;
} DS1302_Time_t;

void DS1302_Init(void);
bool DS1302_ReadTime(DS1302_Time_t *time);
bool DS1302_SetTime(const DS1302_Time_t *time);

#endif /* __EXTERNAL_RTC_H__ */
