#ifndef __UI_DAY_H__
#define __UI_DAY_H__

#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"
#include "BuildConfig.h"
#include "Page.h"
#include "App.h"
#include "LCD.h"
#include "Image.h"
#include "Font.h"
#include "Profiling.h"
#include "Task_Common.h"

#define TOPBAR_Y                 5U   // 图标行与信息行的 y
#define TOPBAR_WIFI_X            5U   // WiFi 图标 x
#define TOPBAR_LOC_X             26U  // 定位图标 x
#define TOPBAR_ICON_SIZE         20U  // 图标边长
#define TOPBAR_MARGIN            5U   // 信息距屏幕右边缘的留白
#define TOPBAR_NAME_CHARS        10U  // WiFi 名、城市名各最多 10 字符
#define TOPBAR_INFO_SIZE         (TOPBAR_NAME_CHARS * 2U + 4U) // "[名,城市]" + '\0' = 24

void UI_Day_Enter(void);
void UI_Day_Leave(void);
void UI_Day_Repaint(void);
void UI_Day_RefreshClock(void);
void UI_Day_HandleEvent(EventBits_t bits);

#endif /* __UI_DAY_H__ */
