#include "Page.h"

#define WEATHER_MAP_SIZE (sizeof(weather_map) / sizeof(weather_map[0]))

extern AT_WiFi_Info_t wifi_info;
extern AT_Date_Info_t date_info;
extern AT_Weather_Info_t weather_info;
extern const char *weekdays[];
extern DHT22_Data_t room_info;

#define MAIN_TOP_BACK_COLOR COLOR_GRAY            /* 顶部模块背景色 */
#define MAIN_BOTTOM1_BACK_COLOR COLOR_DEEPSKYBLUE /* 底部模块背景色1 */
#define MAIN_BOTTOM2_BACK_COLOR COLOR_LAVENDER    /* 底部模块背景色2 */

/* Main Page: 顶部模块 */
Modle_Size_t modle_top = {
    .x_start = 5,
    .y_start = 5,
    .width = 230,
    .height = 135,
};
Modle_Size_t modle_wifi = {
    .x_start = 5,
    .y_start = 5,
    .width = 20,
    .height = 20,
};
/* Main Page: 底部模块1 */
Modle_Size_t modle_bottom1 = {
    .x_start = 5,
    .y_start = 150,
    .width = 115,
    .height = 165,
};
/* Main Page: 底部模块2 */
Modle_Size_t modle_bottom2 = {
    .x_start = 125,
    .y_start = 150,
    .width = 115,
    .height = 165,
};

/* 天气图标与中文映射表 */
static const Weather_Map_t weather_map[] = {
    {0, &Image_sunny, "晴"},
    {1, &Image_star, "晴"},
    {4, &Image_cloudy, "多云"},
    {5, &Image_cloudy, "多云"},
    {6, &Image_cloudy, "多云"},
    {7, &Image_cloudy, "多云"},
    {8, &Image_cloudy, "多云"},
    {9, &Image_overcast, "阴"},
    {10, &Image_shower, "阵雨"},
    {11, &Image_thundershower, "雷阵雨"},
    {12, &Image_thundershower_with_hail, "雷雨冰雹"},
    {13, &Image_light_rain, "小雨"},
    {14, &Image_moderate_rain, "中雨"},
    {15, &Image_heavy_rain, "大雨"},
    {16, &Image_storm, "暴雨"},
    {17, &Image_heavy_storm, "大暴雨"},
    {18, &Image_severe_storm, "特大暴雨"},
    {19, &Image_ice_rain, "冻雨"},
    {20, &Image_sleet, "雨夹雪"},
    {21, &Image_snow_flurry, "阵雪"},
    {22, &Image_light_snow, "小雪"},
    {23, &Image_moderate_snow, "中雪"},
    {24, &Image_heavy_snow, "大雪"},
    {25, &Image_snowstorm, "暴雪"},
    {30, &Image_foggy, "雾"},
    {31, &Image_haze, "霾"},
};

/* === Main Page: 时钟、日期、星期 === */

/* === Main Page: 时钟 === */

#define CLOCK_CHAR_W (Font_48B.size / 2)       /* 单字符宽=24 */
#define CLOCK_TOTAL_W (5 * CLOCK_CHAR_W)       /* 总宽=120 */
#define CLOCK_X0 ((WIDTH - CLOCK_TOTAL_W) / 2) /* 起始x=60 */
#define CLOCK_Y 40
#define CLOCK_HH_X CLOCK_X0
#define CLOCK_HH_W (2 * CLOCK_CHAR_W)              /* 48 */
#define CLOCK_COLON_X (CLOCK_X0 + CLOCK_HH_W)      /* 108 */
#define CLOCK_COLON_W CLOCK_CHAR_W                 /* 24 */
#define CLOCK_MM_X (CLOCK_COLON_X + CLOCK_COLON_W) /* 132 */
#define CLOCK_MM_W (2 * CLOCK_CHAR_W)              /* 48 */

static void Main_Page_Date_Draw(void); /* 前置声明 */

/**
 * @brief 仅绘制小时两位(HH)
 * @param hour 小时(0~23), 未同步时画"--"
 * @return None
 */
static void Main_Page_Clock_Draw_HH(uint8_t hour)
{
    char str[3] = "--";

    if (Clock_IsSynced())
        sprintf(str, "%02u", hour);

    ST7789_Fill_Color(CLOCK_HH_X, CLOCK_Y, CLOCK_HH_X + CLOCK_HH_W - 1, CLOCK_Y + Font_48B.size - 1, MAIN_TOP_BACK_COLOR);
    ST7789_Write_String(CLOCK_HH_X, CLOCK_Y, str, COLOR_BLACK, MAIN_TOP_BACK_COLOR, &Font_48B);
}

/**
 * @brief 仅绘制分钟两位(MM)
 * @param minute 分钟(0~59), 未同步时画"--"
 * @return None
 */
static void Main_Page_Clock_Draw_MM(uint8_t minute)
{
    char str[3] = "--";

    if (Clock_IsSynced())
        sprintf(str, "%02u", minute);

    ST7789_Fill_Color(CLOCK_MM_X, CLOCK_Y, CLOCK_MM_X + CLOCK_MM_W - 1, CLOCK_Y + Font_48B.size - 1, MAIN_TOP_BACK_COLOR);
    ST7789_Write_String(CLOCK_MM_X, CLOCK_Y, str, COLOR_BLACK, MAIN_TOP_BACK_COLOR, &Font_48B);
}

/**
 * @brief 单独绘制/擦除冒号, 不触碰时/分区域
 * @param show true=画冒号 false=擦除
 * @return None
 */
static void Main_Page_Clock_Colon_Draw(bool show)
{
    if (show)
    {
        ST7789_Write_String(CLOCK_COLON_X, CLOCK_Y, ":", COLOR_BLACK, MAIN_TOP_BACK_COLOR, &Font_48B);
    }
    else
    {
        ST7789_Fill_Color(CLOCK_COLON_X, CLOCK_Y, CLOCK_COLON_X + CLOCK_COLON_W - 1, CLOCK_Y + Font_48B.size - 1, MAIN_TOP_BACK_COLOR);
    }
}

/**
 * @brief 完整绘制时钟行(HH + 冒号 + MM), 冒号按当前秒奇偶显示
 * @param None
 * @return None
 */
static void Main_Page_Clock_Draw(void)
{
    AT_Date_Info_t time;

    ST7789_Fill_Color(CLOCK_X0, CLOCK_Y, CLOCK_X0 + CLOCK_TOTAL_W - 1, CLOCK_Y + Font_48B.size - 1, MAIN_TOP_BACK_COLOR);

    if (Clock_IsSynced())
    {
        Clock_GetDateTime(&time);
        Main_Page_Clock_Draw_HH(time.hour);
        Main_Page_Clock_Draw_MM(time.minute);
        Main_Page_Clock_Colon_Draw(time.second & 1);
    }
    else
    {
        Main_Page_Clock_Draw_HH(0xFF);    /* "--" */
        Main_Page_Clock_Draw_MM(0xFF);    /* "--" */
        Main_Page_Clock_Colon_Draw(true); /* 静态 "--:--" */
    }
}

/**
 * @brief 时钟刷新(主循环调用): 秒变只切换冒号, 分/时变才重绘对应数字
 * @param None
 * @return None
 */
void Main_Page_Clock_Update(void)
{
    static int last_second = -1;
    static int last_minute = -1;
    static int last_hour = -1;
    static uint8_t last_day = 0;

    if (!Clock_IsSynced())
    {
        if (last_second != -2)
        {
            last_second = -2;
            last_day = 0;
            last_hour = -1;
            last_minute = -1;
            Main_Page_Clock_Draw();
            Main_Page_Date_Draw();
        }
        return;
    }

    AT_Date_Info_t t;
    Clock_GetDateTime(&t);

    if ((int)t.hour != last_hour)
    {
        last_hour = t.hour;
        Main_Page_Clock_Draw_HH(t.hour);
    }
    if ((int)t.minute != last_minute)
    {
        last_minute = t.minute;
        Main_Page_Clock_Draw_MM(t.minute);
    }
    if ((int)t.second != last_second)
    {
        last_second = t.second;
        Main_Page_Clock_Colon_Draw(t.second & 1);
    }
    if (t.day != last_day)
    {
        last_day = t.day;
        Main_Page_Date_Draw();
    }
}

/**
 * @brief 绘制日期行(日期+星期)
 * @param None
 * @return None
 */
static void Main_Page_Date_Draw(void)
{
    AT_Date_Info_t t;
    char date_week_str[32] = {0};
    int w, x;

    if (Clock_IsSynced())
    {
        const char *weekday_name = "";
        Clock_GetDateTime(&t);
        if (t.weekday >= 1 && t.weekday <= 7)
            weekday_name = weekdays[t.weekday - 1];
        sprintf(date_week_str, "%04u/%02u/%02u %s", t.year, t.month, t.day, weekday_name);
    }

    w = strlen(date_week_str) * (Font_16B.size / 2);
    x = (WIDTH - w) / 2;
    if (x < 0)
        x = 0;

    ST7789_Fill_Color(x, 95, x + w - 1, 95 + Font_16B.size - 1, MAIN_TOP_BACK_COLOR);
    ST7789_Write_String(x, 105, date_week_str, COLOR_BLACK, MAIN_TOP_BACK_COLOR, &Font_16B);
}

/**
 * @brief 绘制主页面顶部区域
 * @param None
 * @return None
 */
static void Main_Page_Top(void)
{
    uint16_t back_color = MAIN_TOP_BACK_COLOR;
    ST7789_Fill_Color(0, 0, WIDTH - 1, HEIGHT - 1, COLOR_BLACK);                                                                                            // 清屏
    ST7789_Fill_Color(modle_top.x_start, modle_top.y_start, modle_top.x_start + modle_top.width - 1, modle_top.y_start + modle_top.height - 1, back_color); /* 填充顶部区域 */

    Main_Page_Net_Update(); /* 顶部网络条: WiFi图标/ssid/定位 (与运行时刷新共用) */

    Main_Page_Clock_Draw();
    Main_Page_Date_Draw();
}

/* === Main Page: 天气 === */

/**
 * @brief 获取天气图标映射
 *
 * @param code 天气代码
 * @return const Weather_Map_t* 匹配的天气图标指针
 */
static const Weather_Map_t *Weather_Get_Map(int code)
{
    for (int i = 0; i < WEATHER_MAP_SIZE; i++)
    {
        if (weather_map[i].code == code)
            return &weather_map[i];
    }
    return NULL;
}
/* 未知天气码: 返回默认项(晴天, 索引0), 避免NULL解引用 */
static const Weather_Map_t *Weather_Default(void)
{
    return &weather_map[0];
}

/**
 * @brief 绘制天气图标和温度
 * @param None
 * @return None
 */
static void Main_Page_Weather(void)
{
    ST7789_Fill_Color(modle_bottom1.x_start, modle_bottom1.y_start,
                      modle_bottom1.x_start + modle_bottom1.width - 1, modle_bottom1.y_start + modle_bottom1.height - 1, MAIN_BOTTOM1_BACK_COLOR);

    /* 绘制模块标题(实况天气), 居中显示 */
    char weather_str[16] = {0};
    sprintf(weather_str, "实况天气");
    uint8_t char_count = strlen(weather_str) / 2;
    uint16_t text_width = char_count * Font_22B.size;
    uint16_t x = modle_bottom1.x_start + (modle_bottom1.width - text_width) / 2;
    ST7789_Write_String(x, modle_bottom1.y_start, weather_str, COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B);

    /* 绘制天气图标 */
    const Weather_Map_t *weather_map = Weather_Get_Map(weather_info.weather_code);
    if (weather_map == NULL)
        weather_map = Weather_Default(); /* 未知天气码: 默认晴天 */
    ST7789_Draw_Picture_AutoTransparent(modle_bottom1.x_start, modle_bottom1.y_start + 33, weather_map->icon, MAIN_BOTTOM1_BACK_COLOR);
    /* 绘制天气状况文字 */
    const char *weather_text = weather_map->chinese;
    uint16_t text_len = strlen(weather_text) / 2;
    uint16_t weather_x = 5 + weather_map->icon->width + 10;
    uint16_t weather_y = modle_bottom1.y_start + 33;
    if (text_len <= 2)
    {
        ST7789_Write_String(weather_x, weather_y, (char *)weather_text, COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B);
    }
    else
    {
        char first_line[8] = {0};
        char second_line[8] = {0};
        strncpy(first_line, weather_text, 4);
        strcpy(second_line, weather_text + 4);
        ST7789_Write_String(weather_x, weather_y, first_line, COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B);
        ST7789_Write_String(weather_x, weather_y + Font_22B.size, second_line, COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B);
    }

    /* 绘制温度计和温度值 */
    ST7789_Draw_Picture_AutoTransparent(modle_bottom1.x_start, modle_bottom1.y_start + 100, (const Image_t *)&Image_thermometer, MAIN_BOTTOM1_BACK_COLOR);
    char temp_str[8] = {0};
    uint16_t temperature_x = weather_x;
    uint16_t temperature_y = modle_bottom1.y_start + 100;
    sprintf(temp_str, "%.1f", weather_info.temperature);
    ST7789_Write_String(temperature_x, temperature_y, temp_str, COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B);
    ST7789_Write_String(temperature_x, temperature_y + Font_22B.size, "C", COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B);
}

/* === Main Page: 室内温湿度 === */

/**
 * @brief 绘制室内温湿度
 * @param None
 * @return None
 */
static void Main_Page_Room(void)
{
    ST7789_Fill_Color(modle_bottom2.x_start, modle_bottom2.y_start,
                      modle_bottom2.x_start + modle_bottom2.width - 1, modle_bottom2.y_start + modle_bottom2.height - 1, MAIN_BOTTOM2_BACK_COLOR);

    /* 绘制模块标题(室内温湿度), 居中显示 */
    char room_str[16] = {0};
    sprintf(room_str, "室内温湿度");
    uint8_t char_count = strlen(room_str) / 2;
    uint16_t text_width = char_count * Font_22B.size;
    uint16_t x = modle_bottom2.x_start + (modle_bottom2.width - text_width) / 2;
    ST7789_Write_String(x, modle_bottom2.y_start, room_str, COLOR_BLACK, MAIN_BOTTOM2_BACK_COLOR, &Font_22B);

    /* 绘制温湿度图标 */
    ST7789_Draw_Picture_AutoTransparent(modle_bottom2.x_start + 3, modle_bottom2.y_start + 30, (const Image_t *)&Image_temperature, MAIN_BOTTOM2_BACK_COLOR);
    ST7789_Draw_Picture_AutoTransparent(modle_bottom2.x_start + 57, modle_bottom2.y_start + 30, (const Image_t *)&Image_humidity, MAIN_BOTTOM2_BACK_COLOR);

    /* 绘制温湿度值 */
    char temp_val[8] = {0};
    char humi_val[8] = {0};
    if (room_info.valid)
    {
        sprintf(temp_val, "%.1f", room_info.temperature);
        sprintf(humi_val, "%.1f", room_info.humidity);
    }
    else
    {
        strcpy(temp_val, "--");
        strcpy(humi_val, "--");
    }
    ST7789_Write_String(modle_bottom2.x_start + 3, modle_bottom2.y_start + 100, temp_val, COLOR_BLACK, MAIN_BOTTOM2_BACK_COLOR, &Font_22B);
    ST7789_Write_String(modle_bottom2.x_start + 3, modle_bottom2.y_start + 100 + Font_22B.size, "C", COLOR_BLACK, MAIN_BOTTOM2_BACK_COLOR, &Font_22B);
    ST7789_Write_String(modle_bottom2.x_start + 57, modle_bottom2.y_start + 100, humi_val, COLOR_BLACK, MAIN_BOTTOM2_BACK_COLOR, &Font_22B);
    ST7789_Write_String(modle_bottom2.x_start + 57, modle_bottom2.y_start + 100 + Font_22B.size, "%", COLOR_BLACK, MAIN_BOTTOM2_BACK_COLOR, &Font_22B);
}

/**
 * @brief 显示主页面
 * @param None
 * @return None
 */
/**
 * @brief 刷新天气模块显示(周期任务更新数据后调用)
 * @param None
 * @return None
 */
void Main_Page_Weather_Update(void)
{
    /* 绘制天气图标 */
    const Weather_Map_t *weather_map = Weather_Get_Map(weather_info.weather_code);
    if (weather_map == NULL)
        weather_map = Weather_Default(); /* 未知天气码: 默认晴天 */
    ST7789_Draw_Picture_AutoTransparent(modle_bottom1.x_start, modle_bottom1.y_start + 33, weather_map->icon, MAIN_BOTTOM1_BACK_COLOR);
    /* 绘制天气状况文字 */
    const char *weather_text = weather_map->chinese; // 获取天气状况文字
    uint16_t text_len = strlen(weather_text) / 2;    // 计算天气状况文字的字符数
    uint16_t weather_x = 5 + weather_map->icon->width + 10;
    uint16_t weather_y = modle_bottom1.y_start + 33;
    ST7789_Fill_Color(weather_x, weather_y, weather_x + Font_22B.size * 2 - 1, weather_y + Font_22B.size * 2 - 1, MAIN_BOTTOM1_BACK_COLOR); // 清除天气状况文字区域,避免上次数据残留
    if (text_len <= 2)
    {
        ST7789_Write_String(weather_x, weather_y, (char *)weather_text, COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B);
    }
    else
    {
        char first_line[8] = {0};
        char second_line[8] = {0};
        strncpy(first_line, weather_text, 4);
        strcpy(second_line, weather_text + 4);
        ST7789_Write_String(weather_x, weather_y, first_line, COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B);
        ST7789_Write_String(weather_x, weather_y + Font_22B.size, second_line, COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B);
    }

    /* 绘制温度计和温度值 */
    ST7789_Draw_Picture_AutoTransparent(modle_bottom1.x_start, modle_bottom1.y_start + 100, (const Image_t *)&Image_thermometer, MAIN_BOTTOM1_BACK_COLOR);
    char temp_str[8] = {0};
    uint16_t temperature_x = weather_x;
    uint16_t temperature_y = modle_bottom1.y_start + 100;
    sprintf(temp_str, "%.1f", weather_info.temperature);
    ST7789_Write_String(temperature_x, temperature_y, temp_str, COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B);
    ST7789_Write_String(temperature_x, temperature_y + Font_22B.size, "C", COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B);
}

/**
 * @brief 刷新房间温湿度模块显示(周期任务更新数据后调用)
 * @param None
 * @return None
 */
void Main_Page_Room_Update(void)
{
    char temp_val[8] = {0};
    char humi_val[8] = {0};
    if (room_info.valid)
    {
        sprintf(temp_val, "%.1f", room_info.temperature);
        sprintf(humi_val, "%.1f", room_info.humidity);
    }
    else
    {
        strcpy(temp_val, "--");
        strcpy(humi_val, "--");
    }
    ST7789_Fill_Color(modle_bottom2.x_start, modle_bottom2.y_start + 100,
                      modle_bottom2.x_start + modle_bottom2.width, modle_bottom2.y_start + 100 + Font_22B.size, MAIN_BOTTOM2_BACK_COLOR);               /* 清除温湿度值区域,避免上次数据残留 */
    ST7789_Write_String(modle_bottom2.x_start + 3, modle_bottom2.y_start + 100, temp_val, COLOR_BLACK, MAIN_BOTTOM2_BACK_COLOR, &Font_22B);             /* 绘制室内温度值 */
    ST7789_Write_String(modle_bottom2.x_start + 3, modle_bottom2.y_start + 100 + Font_22B.size, "C", COLOR_BLACK, MAIN_BOTTOM2_BACK_COLOR, &Font_22B);  /* 绘制室内温度符号 */
    ST7789_Write_String(modle_bottom2.x_start + 57, modle_bottom2.y_start + 100, humi_val, COLOR_BLACK, MAIN_BOTTOM2_BACK_COLOR, &Font_22B);            /* 绘制室内湿度值 */
    ST7789_Write_String(modle_bottom2.x_start + 57, modle_bottom2.y_start + 100 + Font_22B.size, "%", COLOR_BLACK, MAIN_BOTTOM2_BACK_COLOR, &Font_22B); /* 绘制室内湿度符号 */
}

void Main_Page_Display(void)
{

    Main_Page_Top();
    Main_Page_Weather();
    Main_Page_Room();
}
void Main_Page_Net_Update(void)
{
    uint16_t back_color = MAIN_TOP_BACK_COLOR;
    uint16_t text_y = modle_wifi.y_start;
    char ssid_str[32] = {0};
    int ssid_len;

    /* clear the whole status row first (old icons / old text) */
    ST7789_Fill_Color(modle_top.x_start, text_y,
                      modle_top.x_start + modle_top.width - 1,
                      text_y + modle_wifi.height - 1, back_color);

    ssid_len = strlen(wifi_info.ssid);
    if (ssid_len + 2 <= 10)
    {
        sprintf(ssid_str, "[%s]", wifi_info.ssid);
    }
    else
    {
        strncpy(ssid_str, wifi_info.ssid, 5);
        ssid_str[5] = '\0';
        sprintf(ssid_str, "[%s...]", ssid_str);
    }

    if (wifi_info.connected)
    {
        ST7789_Draw_Picture_AutoTransparent(modle_wifi.x_start, modle_wifi.y_start, (const Image_t *)&Image_wifi, back_color);
        ST7789_Write_String(155, text_y, ssid_str, COLOR_BLACK, back_color, &Font_16B);
    }
    else
    {
        ST7789_Draw_Picture_AutoTransparent(modle_wifi.x_start, modle_wifi.y_start, (const Image_t *)&Image_wifi_off, back_color);
    }
}
