#ifndef __IMAGE_H__
#define __IMAGE_H__

#include "main.h"
#include "BuildConfig.h"

/* ============================================================
 * 图片资源描述
 * ------------------------------------------------------------
 * 像素数据(RGB565, 小端) 全部存放在 W25Q64 的 littlefs 文件里,
 * MCU Flash 里只保留宽高与文件路径。文件格式为:
 *   width(2, 大端) + height(2, 大端) + 像素数据
 * 读取时跳过 4 字节头即可, 格式与 LFS_Operation 的 SaveImage 一致。
 * ============================================================ */

typedef struct
{
    uint16_t width;
    uint16_t height;
    const char *path; // W25Q64 上的 littlefs 路径
} Image_t;

/* 全部图片资源清单, 供 Asset 自检与 Provision 烧录遍历 */
extern const Image_t *const g_AllImages[]; // 图片资源清单
extern const uint32_t g_AllImageCount;     // 图片资源总数

/* ------------------------------------------------------------
 * 资源数据本体声明
 * 只在烧录固件(RESOURCE_DATA_IN_ROM=1)里编译, 正式固件不引用。
 * ------------------------------------------------------------ */
#if (RESOURCE_DATA_IN_ROM == 1)
extern const unsigned char gImage_Boot_Page_Waitconnect[153600]; // 开机页面, 等待连接
extern const unsigned char gImage_Main_Page[153600];             // 主页面

extern const unsigned char gImage_err[3200];         // 错误图片
extern const unsigned char gImage_wifi[800];         // WiFi图标
extern const unsigned char gImage_wifi_off[800];     // 无WiFi图标
extern const unsigned char gImage_location[800];     // 定位图标
extern const unsigned char gImage_no_location[800];  // 无定位图标
extern const unsigned char gImage_temperature[5000]; // 温度图标
extern const unsigned char gImage_humidity[5000];    // 湿度图标
extern const unsigned char gImage_thermometer[5000]; // 温湿度计

extern const unsigned char gImage_sunny[5000];                   // 晴,code:0
extern const unsigned char gImage_star[5000];                    // 晴(夜),code:1
extern const unsigned char gImage_overcast[5000];                // 阴,code:9
extern const unsigned char gImage_cloudy[5000];                  // 多云,code:4
extern const unsigned char gImage_light_rain[5000];              // 小雨,code:13
extern const unsigned char gImage_moderate_rain[5000];           // 中雨,code:14
extern const unsigned char gImage_heavy_rain[5000];              // 大雨,code:15
extern const unsigned char gImage_storm[5000];                   // 暴雨,code:16
extern const unsigned char gImage_heavy_storm[5000];             // 大暴雨,code:17
extern const unsigned char gImage_severe_storm[5000];            // 极端暴雨,code:18
extern const unsigned char gImage_ice_rain[5000];                // 冰雹,code:19
extern const unsigned char gImage_shower[5000];                  // 阵雨,code:10
extern const unsigned char gImage_thundershower[5000];           // 雷阵雨,code:11
extern const unsigned char gImage_thundershower_with_hail[5000]; // 雷雨冰雹,code:12
extern const unsigned char gImage_sleet[5000];                   // 雪,code:14
extern const unsigned char gImage_snow_flurry[5000];             // 雪,code:15
extern const unsigned char gImage_light_snow[5000];              // 小雪,code:16
extern const unsigned char gImage_moderate_snow[5000];           // 中雪,code:17
extern const unsigned char gImage_heavy_snow[5000];              // 大雪,code:18
extern const unsigned char gImage_snowstorm[5000];               // 雪,code:19
extern const unsigned char gImage_foggy[5000];                   // 雾,code:20
extern const unsigned char gImage_haze[5000];                    // 雾,code:21
#endif /* RESOURCE_DATA_IN_ROM */

/* 开机引用图片 */
extern const Image_t Image_err;             // 错误图片
extern const Image_t Boot_Page_Waitconnect; // 开机页面, 等待连接
extern const Image_t Image_Main_Page;       // 主页面全屏底图

/* 主页面引用图片 */
extern const Image_t Image_wifi;        // WiFi图标
extern const Image_t Image_wifi_off;    // 无WiFi图标
extern const Image_t Image_location;    // 定位图标
extern const Image_t Image_no_location; // 无定位图标
extern const Image_t Image_temperature; // 温度图标
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
