#ifndef __PAGE_H__
#define __PAGE_H__

#include "main.h"
#include "Timer.h"
#include "LCD.h"
#include "Image.h"
#include "Font.h"
#include "App.h"

#define WEATHER_MAP_SIZE               (sizeof(weather_map) / sizeof(weather_map[0])) // 天气图标与中文映射表大小
#define MAIN_TOP_BACK_COLOR            COLOR_GRAY                                     // 时间日期模块背景色
#define MAIN_BOTTOM1_BACK_COLOR        COLOR_DEEPSKYBLUE                              // 天气模块背景色1
#define MAIN_BOTTOM2_BACK_COLOR        COLOR_LAVENDER                                 // 天气模块背景色2

/* Main Page: 时钟 */
#define CLOCK_CHAR_W         (Font_48B.size / 2)             // 单字符宽 = 24
#define CLOCK_TOTAL_W        (5 * CLOCK_CHAR_W)              // 总宽 = 5 * 24 = 120
#define CLOCK_X0             ((WIDTH - CLOCK_TOTAL_W) / 2)   // 时钟起始 x = (240 - 120) / 2 = 60
#define CLOCK_Y0             (5 + (Image_wifi.height) * 2)   // 时钟起始 y = 5 + 20 * 2 = 45
#define CLOCK_HH_X           CLOCK_X0                        // 时钟起始 x = 60
#define CLOCK_HH_W           (2 * CLOCK_CHAR_W)              // 时钟小时宽度 = 2 * 24 = 48
#define CLOCK_COLON_X        (CLOCK_X0 + CLOCK_HH_W)         // 冒号位置 = 60 + 48 = 108
#define CLOCK_COLON_W        CLOCK_CHAR_W                    // 冒号宽度 = 24
#define CLOCK_MM_X           (CLOCK_COLON_X + CLOCK_COLON_W) // 时钟分钟起始 x = 108 + 24 = 132
#define CLOCK_MM_W           (2 * CLOCK_CHAR_W)              // 时钟分钟宽度 = 2 * 24 = 48

/* 页面模块大小 */
typedef struct
{
    uint16_t x_start;
    uint16_t y_start;
    uint16_t width;
    uint16_t height;
} Modle_Size_t;

/* 天气图标与中文映射表 */
typedef struct
{
    int code;
    const Image_t *icon;
    const char *chinese;
} Weather_Map_t;

void Boot_Page_Wait(void);
void Boot_Page_Show(bool wifi_ok, bool service_ok);
void Main_Page_Display(void);
void Main_Page_Clock_Update(void);
void Main_Page_Weather_Update(void);
void Main_Page_Room_Update(void);
void Main_Page_Net_Update(void);

#endif /* __PAGE_H__ */
