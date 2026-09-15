#ifndef __BARE_TEST_H__
#define __BARE_TEST_H__

#include "BuildConfig.h"
#include "main.h"
#include "stm32f4xx.h"
#include "Timer.h"
#include "I2C.h"
#include "OLED.h"
#include "Usart.h"
#include "Light_Sensor.h"
#include "External_RTC.h"
#include "LFS_Operation.h"

#if (USE_FREERTOS == 0)

typedef enum
{
    BM_TEST_MODULE_OLED = 1,   // OLED 测试
    BM_TEST_MODULE_LIGHT = 2,  // 光敏电阻测试
    BM_TEST_MODULE_DS1302 = 3, // DS1302 外部RTC测试
    BM_TEST_MODULE_W25Q64 = 4, // W25Q64 SPI Flash 裸驱动测试
    BM_TEST_MODULE_LFS = 5,    // littlefs 文件系统测试
} BM_TEST_MODULE_t;

/* Run the bare-metal module test selected by BM_TEST_MODULE (BuildConfig.h).
 * Each test owns an endless loop and must be called after clock + TIM5 init. */
void BareMetal_Module_Test(void);

#endif /* USE_FREERTOS == 0 */

#endif /* __BARE_TEST_H__ */
