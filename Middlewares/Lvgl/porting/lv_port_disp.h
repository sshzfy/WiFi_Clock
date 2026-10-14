/**
 * @file lv_port_disp.h
 *
 * LVGL 显示端口(2.4" ST7789V, 240x320, SPI3 + DMA)
 */
#ifndef LV_PORT_DISP_H
#define LV_PORT_DISP_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/* 注册显示驱动: 必须在 lv_init() 之后、创建任何控件之前调用 */
void lv_port_disp_init(void);

/* 暂停/恢复刷屏(刷屏期间会把缓冲交给 DMA, 需要临时闭屏时用) */
void disp_enable_update(void);
void disp_disable_update(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_PORT_DISP_H*/
