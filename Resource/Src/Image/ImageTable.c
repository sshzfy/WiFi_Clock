/*
 * 图片资源清单
 *
 * 供 Asset 开机自检与 Provision 烧录遍历使用。
 * 这里只是一张指针表(32 * 4 = 128 字节), 不含像素数据,
 * 因此正式固件也照常编译它。
 */
#include "Image.h"

const Image_t *const g_AllImages[] = {
    /* 开机与主页面全屏图 */
    &Boot_Page_Waitconnect,
    &Image_Main_Page,

    /* 通用图标 */
    &Image_err,
    &Image_wifi,
    &Image_wifi_off,
    &Image_location,
    &Image_no_location,
    &Image_temperature,
    &Image_humidity,
    &Image_thermometer,

    /* 天气图标 */
    &Image_sunny,
    &Image_star,
    &Image_cloudy,
    &Image_overcast,
    &Image_shower,
    &Image_thundershower,
    &Image_thundershower_with_hail,
    &Image_light_rain,
    &Image_moderate_rain,
    &Image_heavy_rain,
    &Image_storm,
    &Image_heavy_storm,
    &Image_severe_storm,
    &Image_ice_rain,
    &Image_sleet,
    &Image_snow_flurry,
    &Image_light_snow,
    &Image_moderate_snow,
    &Image_heavy_snow,
    &Image_snowstorm,
    &Image_foggy,
    &Image_haze,
};

const uint32_t g_AllImageCount = sizeof(g_AllImages) / sizeof(g_AllImages[0]);
