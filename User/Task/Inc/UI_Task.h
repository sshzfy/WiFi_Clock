#ifndef __UI_TASK_H__
#define __UI_TASK_H__

#include "main.h"
#include "BuildConfig.h"
#include "Task_Common.h"
#include "Task_Config.h"
#include "DHT22_Task.h"
#include "Net_Task.h"
#include "LightSensor_Task.h"
#include "Key_Task.h"
#include "Board.h"
#include "Page.h"
#include "App.h"
#include "LCD.h"
#include "OLED.h"
#include "External_RTC.h"
#include "Asset.h"
#include "Profiling.h"
#include "Touch.h"
#include "ui.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"

/* 触摸范围 */
#define TOUCH_X_WIFI           5U // WiFi 图标 20x20 在 (5,5)
#define TOUCH_Y_WIFI           5U
#define TOUCH_SIZE_WIFI        20U
#define TOUCH_X_LOC            26U // 定位图标 20x20 紧挨 WiFi 图标右侧 (26,5)
#define TOUCH_Y_LOC            5U
#define TOUCH_SIZE_LOC         20U

#define TOPBAR_Y                 5U   // 图标行与信息行的 y(与 WiFi 图标一致)
#define TOPBAR_WIFI_X            5U   // WiFi 图标 x
#define TOPBAR_LOC_X             26U  // 定位图标 x(紧挨 WiFi 图标右侧)
#define TOPBAR_ICON_SIZE         20U  // 图标边长
#define TOPBAR_MARGIN            5U   // 信息距屏幕右边缘的留白
#define TOPBAR_NAME_CHARS        10U  // WiFi 名、城市名各最多 10 字符
#define TOPBAR_INFO_SIZE         (TOPBAR_NAME_CHARS * 2U + 4U) // "[名,城市]" + '\0' = 24

#define KB_ACTIVE()        (s_kb_screen != UI_SCR_MAIN) // 键盘界面激活时用小心跳(10ms)

/* 当前停在哪个输入界面 */
typedef enum
{
    UI_SCR_MAIN = 0, // 不在 LVGL 界面(主页面)
    UI_SCR_WIFI,     // ui_WiFi: 配网
    UI_SCR_LOCATION  // ui_location: 定位信息
} UI_Screen_t;

void UI_Task_Start(void);
void UI_Task_EnterNight(void);
void UI_Task_EnterDay(void);
bool UI_Task_IsLowPower(void);

#endif /* __UI_TASK_H__ */
