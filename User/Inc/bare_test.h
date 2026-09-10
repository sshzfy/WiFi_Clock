#ifndef __BARE_TEST_H__
#define __BARE_TEST_H__

#include "BuildConfig.h"
#include "main.h"
#include "stm32f4xx.h"
#include "Timer.h"
#include "I2C.h"
#include "Font.h"
#include "OLED.h"
#include "Light_Sensor.h"

#if (USE_FREERTOS == 0)

/* Run the bare-metal module test selected by BM_TEST_MODULE.
 * Each test owns an endless loop and must be called after clock + TIM5 init. */
void BareMetal_Module_Test(void);

#endif /* USE_FREERTOS == 0 */

#endif /* __BARE_TEST_H__ */
