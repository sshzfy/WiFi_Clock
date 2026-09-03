#ifndef __IMAGE_H__
#define __IMAGE_H__

#include "main.h"

typedef struct
{
    uint16_t width;
    uint16_t height;
    const uint8_t *data;
} Image_t;

/* 开机引用图片 */
extern const Image_t Image_err;             // 错误图片
extern const Image_t Boot_Page_Waitconnect; // 开机页面, 等待连接
/* 主页面引用图片 */
extern const Image_t Image_wifi;        // WiFi图标
extern const Image_t Image_wifi_off;    // 无WiFi图标
extern const Image_t Image_location;    // 定位图标
extern const Image_t Image_no_location; // 无定位图标
extern const Image_t Image_temperature; // 温度图标
extern const Image_t Image_celsius;     // 摄氏度图标
extern const Image_t Image_humidity;    // 湿度图标
extern const Image_t Image_thermometer; // 温湿度计
/* 天气引用图片 */
extern const Image_t Image_sunny;                   // 晴,code:0
extern const Image_t Image_star;                    // 晴(夜),code:1
extern const Image_t Image_cloudy;                  // 多云,code:4
extern const Image_t Image_overcast;                // 阴,code:9
extern const Image_t Image_shower;                  // 阵雨,code:10
extern const Image_t Image_thundershower;           // 雷阵雨,code:11
extern const Image_t Image_thundershower_with_hail; // 雷雨冰雹,code:12
extern const Image_t Image_light_rain;              // 小雨,code:13
extern const Image_t Image_moderate_rain;           // 中雨,code:14
extern const Image_t Image_heavy_rain;              // 大雨,code:15
extern const Image_t Image_storm;                   // 暴雨,code:16
extern const Image_t Image_heavy_storm;             // 大暴雨,code:17
extern const Image_t Image_severe_storm;            // 特大暴雨,code:18
extern const Image_t Image_ice_rain;                // 冻雨,code:19
extern const Image_t Image_sleet;                   // 雨夹雪,code:20
extern const Image_t Image_snow_flurry;             // 阵雪,code:21
extern const Image_t Image_light_snow;              // 小雪,code:22
extern const Image_t Image_moderate_snow;           // 中雪,code:23
extern const Image_t Image_heavy_snow;              // 大雪,code:24
extern const Image_t Image_snowstorm;               // 暴雪,code:25
extern const Image_t Image_foggy;                   // 雾天,code:30
extern const Image_t Image_haze;                    // 霾天,code:31

#endif /* __IMAGE_H__ */
