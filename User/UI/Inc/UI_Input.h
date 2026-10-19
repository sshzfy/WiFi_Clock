#ifndef __UI_INPUT_H__
#define __UI_INPUT_H__

#include "main.h"
#include "BuildConfig.h"
#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"
#include "ui.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "LCD.h"
#include "Touch.h"
#include "Timer.h"
#include "Task_Common.h"
#include "App.h"


/* 触摸热区: 左上角并排两个 20x20 图标(WiFi 在 x=5, 定位在 x=26, y 都是 5) */
#define TOUCH_X_WIFI           5U
#define TOUCH_Y_WIFI           5U
#define TOUCH_SIZE_WIFI        20U
#define TOUCH_X_LOC            26U
#define TOUCH_Y_LOC            5U
#define TOUCH_SIZE_LOC         20U

/* 当前停在哪个输入界面 */
typedef enum
{
    UI_SCR_MAIN = 0, // 不在 LVGL 界面(主页面)
    UI_SCR_WIFI,     // ui_WiFi: 配网
    UI_SCR_LOCATION  // ui_location: 定位信息
} UI_Screen_t;

/* 一个可点击区域: 命中后置位对应事件 */
typedef struct
{
    uint16_t x;        // 左上角 x
    uint16_t y;        // 左上角 y
    uint16_t size;     // 边长
    EventBits_t event; // 命中后置位的事件
} UI_Hotspot_t;

void UI_Input_Calibrate(void);
void UI_Input_PollTouch(void);
bool UI_Input_HandleEvent(EventBits_t bits, bool allow_open);
void UI_Input_Open(UI_Screen_t screen);
void UI_Input_Close(void);
bool UI_Input_IsActive(void);
void UI_Input_Tick(void);

#endif /* __UI_INPUT_H__ */
