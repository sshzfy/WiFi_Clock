#ifndef __BUILD_CONFIG_H__
#define __BUILD_CONFIG_H__

/* ============================================================
 * Build switches
 * ------------------------------------------------------------
 * USE_FREERTOS:
 *   1 = enable FreeRTOS (boot OS, run normal smart-clock app)
 *   0 = disable OS -> enter bare-metal module test (BM_TEST_MODULE)
 *
 * BM_TEST_MODULE (only used when USE_FREERTOS == 0):
 *   1 = OLED (SSD1306 0.96, soft I2C PB6=SCL/PB7=SDA)
 *   ... add further modules here while bring-up in bare metal
 * ============================================================ */

#define USE_FREERTOS   0
#define BM_TEST_MODULE 1

#endif /* __BUILD_CONFIG_H__ */
