/**
 * @file lv_port_indev.h
 *
 * LVGL 输入端口(XPT2046 四线电阻触摸屏)
 */
#ifndef LV_PORT_INDEV_H
#define LV_PORT_INDEV_H

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

/* 注册指针输入设备: 必须在 lv_init() 之后调用 */
void lv_port_indev_init(void);

/* 取触摸输入设备句柄(未初始化时为 NULL) */
lv_indev_t *lv_port_indev_get(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_PORT_INDEV_H*/
