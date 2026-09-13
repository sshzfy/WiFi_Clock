#ifndef __BUILD_CONFIG_H__
#define __BUILD_CONFIG_H__

/* ============================================================
 * Build switches
 * ------------------------------------------------------------
 * FreeRTOS 开关:
 *   1 = 使用FreeRTOS调度
 *   0 = 裸机,方便测试模块完成后加入FreeRTOS调度
 *
 * BM_TEST_MODULE (only used when USE_FREERTOS == 0):
 *   BM_TEST_MODULE_OLED   = OLED (SSD1306 0.96, soft I2C PB6=SCL/PB7=SDA)
 *   BM_TEST_MODULE_LIGHT  = 光敏电阻 (DO: PA1/EXTI1; AO: PA0/ADC1_IN0)
 *   BM_TEST_MODULE_DS1302 = DS1302 外部RTC (PB0=RST/PB1=IO/PB2=CLK)
 * ============================================================ */

#define USE_FREERTOS   1

#if (USE_FREERTOS == 0)
/* 裸机模块测试项: 在此切换。
 * 注意: 该宏是唯一入口, 不要在 bare_test.c 里另建同名变量。 */
#ifndef BM_TEST_MODULE
#define BM_TEST_MODULE BM_TEST_MODULE_DS1302
#endif
#endif /* USE_FREERTOS == 0 */

#endif /* __BUILD_CONFIG_H__ */
