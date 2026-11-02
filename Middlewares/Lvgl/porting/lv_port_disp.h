/**
 * @file lv_port_disp.h
 *
 * LVGL ÏÔÊ¾¶Ë¿Ú(2.4" ST7789V, 240x320, SPI3 + DMA)
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

void lv_port_disp_init(void);

void disp_enable_update(void);
void disp_disable_update(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_PORT_DISP_H*/
