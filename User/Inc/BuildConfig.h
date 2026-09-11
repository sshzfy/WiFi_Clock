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
 *   1 = OLED (SSD1306 0.96, soft I2C PB6=SCL/PB7=SDA)
 *   ... add further modules here while bring-up in bare metal
 * ============================================================ */

#define USE_FREERTOS   1
// #define BM_TEST_MODULE 1

#endif /* __BUILD_CONFIG_H__ */
