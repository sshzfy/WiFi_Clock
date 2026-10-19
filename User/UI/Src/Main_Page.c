#include "Page.h"

extern AT_WiFi_Info_t wifi_info;
extern AT_Date_Info_t date_info;
extern AT_Weather_Info_t weather_info;
extern const char *weekdays[];
extern DHT22_Data_t room_info;

/* Main Page: 时间日期模块 */
static const Modle_Size_t modle_top =
    {
        .x_start = 5,
        .y_start = 5,
        .width = 230,
        .height = 135,
};
/* Main Page: WiFi 图标 */
static const Modle_Size_t modle_wifi =
    {
        .x_start = 5,
        .y_start = 5,
        .width = 20,
        .height = 20,
};
/* Main Page: 定位图标 */
static const Modle_Size_t modle_location =
    {
        .x_start = 26,
        .y_start = 5,
        .width = 20,
        .height = 20,
};
/* Main Page: 天气模块 */
static const Modle_Size_t modle_weather =
    {
        .x_start = 5,
        .y_start = 150,
        .width = 115,
        .height = 165,
};
/* Main Page: 室内模块 */
static const Modle_Size_t modle_room =
    {
        .x_start = 125,
        .y_start = 150,
        .width = 115,
        .height = 165,
};
/* 天气图标与中文映射表 */
static const Weather_Map_t weather_map[] =
    {
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

/* ================ static 函数 ================ */
static void Main_Page_Clock_Draw_HH(uint8_t hour);
static void Main_Page_Clock_Draw_MM(uint8_t minute);
static void Main_Page_Clock_Colon_Draw(bool show);
static void Main_Page_Date_Draw(void);
static void Main_Page_Top(void);
static const Weather_Map_t *Weather_Get_Map(int code);
static const Weather_Map_t *Weather_Default(void);
static void Main_Page_Weather(void);
static void Main_Page_Room(void);

/* ================ Main Page: 时钟 ================ */

/**
 * @brief 仅绘制小时两位(HH)
 * @param hour 小时(0~23), 未同步时画"--"
 */
static void Main_Page_Clock_Draw_HH(uint8_t hour)
{
    char str[3] = "--";

    if (Clock_IsSynced())
        sprintf(str, "%02u", hour);

    LCD_Fill_Color(CLOCK_HH_X, CLOCK_Y0, CLOCK_HH_X + CLOCK_HH_W - 1, CLOCK_Y0 + Font_48B.size - 1, MAIN_TOP_BACK_COLOR);
    LCD_Write_String(CLOCK_HH_X, CLOCK_Y0, str, COLOR_BLACK, MAIN_TOP_BACK_COLOR, &Font_48B);
}

/**
 * @brief 仅绘制分钟两位(MM)
 * @param minute 分钟(0~59), 未同步时画"--"
 */
static void Main_Page_Clock_Draw_MM(uint8_t minute)
{
    char str[3] = "--";

    if (Clock_IsSynced())
        sprintf(str, "%02u", minute);

    LCD_Fill_Color(CLOCK_MM_X, CLOCK_Y0, CLOCK_MM_X + CLOCK_MM_W - 1, CLOCK_Y0 + Font_48B.size - 1, MAIN_TOP_BACK_COLOR);
    LCD_Write_String(CLOCK_MM_X, CLOCK_Y0, str, COLOR_BLACK, MAIN_TOP_BACK_COLOR, &Font_48B);
}

/**
 * @brief 单独绘制/擦除冒号, 不触碰时/分区域
 * @param show true=画冒号 false=擦除
 */
static void Main_Page_Clock_Colon_Draw(bool show)
{
    if (show)
        LCD_Write_String(CLOCK_COLON_X, CLOCK_Y0, ":", COLOR_BLACK, MAIN_TOP_BACK_COLOR, &Font_48B);
    else
        LCD_Fill_Color(CLOCK_COLON_X, CLOCK_Y0, CLOCK_COLON_X + CLOCK_COLON_W - 1, CLOCK_Y0 + Font_48B.size - 1, MAIN_TOP_BACK_COLOR);
}

/** @brief 完整绘制时钟行(HH + 冒号 + MM), 冒号按当前秒奇偶显示 */
static void Main_Page_Clock_Draw(void)
{
    AT_Date_Info_t time;

    LCD_Fill_Color(CLOCK_X0, CLOCK_Y0, CLOCK_X0 + CLOCK_TOTAL_W - 1, CLOCK_Y0 + Font_48B.size - 1, MAIN_TOP_BACK_COLOR);

    if (Clock_IsSynced())
    {
        Clock_GetDateTime(&time);
        Main_Page_Clock_Draw_HH(time.hour);
        Main_Page_Clock_Draw_MM(time.minute);
        Main_Page_Clock_Colon_Draw(time.second & 1);
    }
    else
    {
        Main_Page_Clock_Draw_HH(0xFF);    // "--"
        Main_Page_Clock_Draw_MM(0xFF);    // "--"
        Main_Page_Clock_Colon_Draw(true); // 静态 "--:--"
    }
}

/** @brief 绘制日期行(日期+星期) */
static void Main_Page_Date_Draw(void)
{
    AT_Date_Info_t tmp;
    char date_week_str[32] = {0};
    int width, x;

    if (Clock_IsSynced())
    {
        const char *weekday_name = "";

        Clock_GetDateTime(&tmp);
        if (tmp.weekday >= 1 && tmp.weekday <= 7)
            weekday_name = weekdays[tmp.weekday - 1];
        sprintf(date_week_str, "%04u/%02u/%02u %s", tmp.year, tmp.month, tmp.day, weekday_name);
    }

    width = strlen(date_week_str) * (Font_16B.size / 2);
    x = (WIDTH - width) / 2;
    if (x < 0)
        x = 0;

    /* 绘制日期+星期 */
    LCD_Fill_Color(x, 95, x + width - 1, 95 + Font_16B.size - 1, MAIN_TOP_BACK_COLOR);
    LCD_Write_String(x, 105, date_week_str, COLOR_BLACK, MAIN_TOP_BACK_COLOR, &Font_16B);
}

/** @brief 绘制主页面顶部区域 */
static void Main_Page_Top(void)
{
    uint16_t back_color = MAIN_TOP_BACK_COLOR;

    /* 填充顶部区域 */
    LCD_Fill_Color(modle_top.x_start, modle_top.y_start,
                   modle_top.x_start + modle_top.width - 1, modle_top.y_start + modle_top.height - 1,
                   back_color);
    /* 顶部网络条: WiFi图标/ssid/定位 (与运行时刷新共用) */
    Main_Page_Net_Update();
    Main_Page_Clock_Draw();
    Main_Page_Date_Draw();
}

/* ================ Main Page: 天气 ================ */

/**
 * @brief 获取天气图标映射
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
    /* 未知天气码: 返回NULL */
    return NULL;
}

/** @brief 未知天气码: 返回默认项(晴天, 索引0), 避免NULL解引用 */
static const Weather_Map_t *Weather_Default(void)
{
    return &weather_map[0];
}

/** @brief 绘制天气图标和温度 */
static void Main_Page_Weather(void)
{
    /* 填充天气模块背景 */
    LCD_Fill_Color(modle_weather.x_start, modle_weather.y_start,
                   modle_weather.x_start + modle_weather.width - 1, modle_weather.y_start + modle_weather.height - 1, MAIN_BOTTOM1_BACK_COLOR);

    /* 绘制模块标题(实况天气), 居中显示
     * 绘制天气图标
     * 绘制天气状况文字 */
    char weather_str[16] = {0};
    sprintf(weather_str, "实况天气");
    uint8_t char_count = strlen(weather_str) / 2;
    uint16_t text_width = char_count * Font_22B.size;
    uint16_t x = modle_weather.x_start + (modle_weather.width - text_width) / 2;

    LCD_Write_String(x, modle_weather.y_start, weather_str, COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B); // 绘制模块标题

    const Weather_Map_t *weather_map = Weather_Get_Map(weather_info.weather_code);
    if (weather_map == NULL)
        weather_map = Weather_Default();

    LCD_Draw_Picture_AutoTransparent(modle_weather.x_start, modle_weather.y_start + 33, weather_map->icon, MAIN_BOTTOM1_BACK_COLOR); // 绘制天气图标

    const char *weather_text = weather_map->chinese;
    uint16_t text_len = strlen(weather_text) / 2;
    uint16_t weather_x = 5 + weather_map->icon->width + 10;
    uint16_t weather_y = modle_weather.y_start + 33;

    if (text_len <= 2)
        LCD_Write_String(weather_x, weather_y, (char *)weather_text, COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B); // 绘制天气状况文字
    else
    {
        char first_line[8] = {0};
        char second_line[8] = {0};
        strncpy(first_line, weather_text, 4);
        strcpy(second_line, weather_text + 4);
        LCD_Write_String(weather_x, weather_y, first_line, COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B);
        LCD_Write_String(weather_x, weather_y + Font_22B.size, second_line, COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B);
    }

    /* 绘制温度计和温度值 */
    LCD_Draw_Picture_AutoTransparent(modle_weather.x_start, modle_weather.y_start + 100, (const Image_t *)&Image_thermometer, MAIN_BOTTOM1_BACK_COLOR);
    char temp_str[8] = {0};
    uint16_t temperature_x = weather_x;
    uint16_t temperature_y = modle_weather.y_start + 100;
    sprintf(temp_str, "%.1f", weather_info.temperature);
    LCD_Write_String(temperature_x, temperature_y, temp_str, COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B);
    LCD_Write_String(temperature_x, temperature_y + Font_22B.size, "C", COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B);
}

/* ================ Main Page: 室内温湿度 ================ */

/** @brief 绘制室内温湿度 */
static void Main_Page_Room(void)
{
    /* 填充室内温湿度模块背景 */
    LCD_Fill_Color(modle_room.x_start, modle_room.y_start,
                   modle_room.x_start + modle_room.width - 1, modle_room.y_start + modle_room.height - 1, MAIN_BOTTOM2_BACK_COLOR);

    /* 绘制模块标题(室内温湿度), 居中显示 */
    char room_str[16] = {0};
    sprintf(room_str, "室内温湿度");
    uint8_t char_count = strlen(room_str) / 2;
    uint16_t text_width = char_count * Font_22B.size;
    uint16_t x = modle_room.x_start + (modle_room.width - text_width) / 2;
    LCD_Write_String(x, modle_room.y_start, room_str, COLOR_BLACK, MAIN_BOTTOM2_BACK_COLOR, &Font_22B); // 绘制模块标题
    /* 绘制温湿度图标 */
    LCD_Draw_Picture_AutoTransparent(modle_room.x_start + 3, modle_room.y_start + 30, (const Image_t *)&Image_temperature, MAIN_BOTTOM2_BACK_COLOR);
    LCD_Draw_Picture_AutoTransparent(modle_room.x_start + 57, modle_room.y_start + 30, (const Image_t *)&Image_humidity, MAIN_BOTTOM2_BACK_COLOR);

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
    LCD_Write_String(modle_room.x_start + 3, modle_room.y_start + 100, temp_val, COLOR_BLACK, MAIN_BOTTOM2_BACK_COLOR, &Font_22B);
    LCD_Write_String(modle_room.x_start + 3, modle_room.y_start + 100 + Font_22B.size, "C", COLOR_BLACK, MAIN_BOTTOM2_BACK_COLOR, &Font_22B);
    LCD_Write_String(modle_room.x_start + 57, modle_room.y_start + 100, humi_val, COLOR_BLACK, MAIN_BOTTOM2_BACK_COLOR, &Font_22B);
    LCD_Write_String(modle_room.x_start + 57, modle_room.y_start + 100 + Font_22B.size, "%", COLOR_BLACK, MAIN_BOTTOM2_BACK_COLOR, &Font_22B);
}

/* ================ public 函数 ================ */

/** @brief 时钟刷新(主循环调用): 秒变只切换冒号, 分/时变才重绘对应数字 */
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

/** @brief 刷新天气模块显示(周期任务更新数据后调用) */
void Main_Page_Weather_Update(void)
{
    /* 绘制天气图标 */
    const Weather_Map_t *weather_map = Weather_Get_Map(weather_info.weather_code);
    /* 未知天气码: 默认晴天 */
    if (weather_map == NULL)
        weather_map = Weather_Default();
    LCD_Draw_Picture_AutoTransparent(modle_weather.x_start, modle_weather.y_start + 33, weather_map->icon, MAIN_BOTTOM1_BACK_COLOR);

    /* 绘制天气状况文字 */
    const char *weather_text = weather_map->chinese; // 获取天气状况文字
    uint16_t text_len = strlen(weather_text) / 2;    // 计算天气状况文字的字符数
    uint16_t weather_x = 5 + weather_map->icon->width + 10;
    uint16_t weather_y = modle_weather.y_start + 33;
    LCD_Fill_Color(weather_x, weather_y, weather_x + Font_22B.size * 2 - 1, weather_y + Font_22B.size * 2 - 1, MAIN_BOTTOM1_BACK_COLOR); // 清除天气状况文字区域,避免上次数据残留
    if (text_len <= 2)
        LCD_Write_String(weather_x, weather_y, (char *)weather_text, COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B);
    else
    {
        char first_line[8] = {0};
        char second_line[8] = {0};
        strncpy(first_line, weather_text, 4);
        strcpy(second_line, weather_text + 4);
        LCD_Write_String(weather_x, weather_y, first_line, COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B);
        LCD_Write_String(weather_x, weather_y + Font_22B.size, second_line, COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B);
    }

    /* 绘制温度计和温度值 */
    LCD_Draw_Picture_AutoTransparent(modle_weather.x_start, modle_weather.y_start + 100, (const Image_t *)&Image_thermometer, MAIN_BOTTOM1_BACK_COLOR);
    char temp_str[8] = {0};
    uint16_t temperature_x = weather_x;
    uint16_t temperature_y = modle_weather.y_start + 100;
    sprintf(temp_str, "%.1f", weather_info.temperature);
    LCD_Write_String(temperature_x, temperature_y, temp_str, COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B);
    LCD_Write_String(temperature_x, temperature_y + Font_22B.size, "C", COLOR_BLACK, MAIN_BOTTOM1_BACK_COLOR, &Font_22B);
}

/** @brief 刷新房间温湿度模块显示(周期任务更新数据后调用) */
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

    /* 清除温湿度值区域,避免上次数据残留
     * 绘制室内温度值
     * 绘制室内温度符号
     * 绘制室内湿度值
     * 绘制室内湿度符号 */
    LCD_Fill_Color(modle_room.x_start, modle_room.y_start + 100,
                   modle_room.x_start + modle_room.width, modle_room.y_start + 100 + Font_22B.size, MAIN_BOTTOM2_BACK_COLOR);
    LCD_Write_String(modle_room.x_start + 3, modle_room.y_start + 100, temp_val, COLOR_BLACK, MAIN_BOTTOM2_BACK_COLOR, &Font_22B);
    LCD_Write_String(modle_room.x_start + 3, modle_room.y_start + 100 + Font_22B.size, "C", COLOR_BLACK, MAIN_BOTTOM2_BACK_COLOR, &Font_22B);
    LCD_Write_String(modle_room.x_start + 57, modle_room.y_start + 100, humi_val, COLOR_BLACK, MAIN_BOTTOM2_BACK_COLOR, &Font_22B);
    LCD_Write_String(modle_room.x_start + 57, modle_room.y_start + 100 + Font_22B.size, "%", COLOR_BLACK, MAIN_BOTTOM2_BACK_COLOR, &Font_22B);
}

void Main_Page_Display(void)
{
    LCD_Fill_Color(0, 0, WIDTH - 1, HEIGHT - 1, COLOR_BLACK); // 清屏
    Main_Page_Top();
    Main_Page_Weather();
    Main_Page_Room();
}
/**
 * @brief 刷新顶部状态条(WiFi图标 + SSID)
 * @note  不显示定位/城市信息: 城市名仍由天气接口写入 weather_info.city,
 *        只是不再参与显示(接口保留, 见 4.13)。
 */
/* ================ Main Page: 顶部状态条(WiFi + 定位) ================ */

/**
 * @brief 按 16 号字把文本截到指定像素宽度内(超出部分用 ".." 收尾)
 * @param src 源文本(GB2312 汉字与 ASCII 混排)
 * @param dst 输出缓冲
 * @param dst_size 输出缓冲字节数
 * @param max_px 允许的最大像素宽度
 * @note  16 号字: ASCII 宽 8 像素, 汉字宽 16 像素; 汉字按双字节整体取舍
 */
static void Main_Page_FitText(const char *src, char *dst, uint16_t dst_size, uint16_t max_px)
{
    uint16_t px = 0U;
    uint16_t n = 0U;

    if ((src == NULL) || (dst == NULL) || (dst_size < 4U))
        return;

    while ((*src != '\0') && ((uint16_t)(n + 3U) < dst_size))
    {
        uint16_t w = ((uint8_t)*src >= 0x81U) ? 16U : 8U;

        if ((uint16_t)(px + w) > max_px)
            break; /* 剩下的放不下了 */

        if (w == 16U)
        {
            if (src[1] == '\0')
                break; /* 半个汉字, 丢掉 */
            dst[n++] = *src++;
            dst[n++] = *src++;
        }
        else
        {
            dst[n++] = *src++;
        }

        px += w;
    }

    if (*src != '\0')
    {
        dst[n++] = '.';
        dst[n++] = '.';
    }

    dst[n] = '\0';
}

/**
 * @brief 刷新顶部状态条: 第一行 WiFi 图标+名称, 第二行 定位图标+省/市
 * @param None
 * @return None
 * @note  两行布局一致, 文字都紧贴各自图标的右侧; 定位图标在 WiFi 图标正下方。
 *        定位信息只用于显示, 天气查询仍按公网IP定位。
 */
void Main_Page_Net_Update(void)
{
    uint16_t back_color = MAIN_TOP_BACK_COLOR;
    uint16_t text_x = (uint16_t)(modle_wifi.x_start + modle_wifi.width + 2U);   /* 紧贴图标右侧 */
    uint16_t max_px = (uint16_t)(modle_top.x_start + modle_top.width - text_x); /* 可用到顶栏右边界 */
    char text[40] = {0};

    /* 一次清掉两行(WiFi 行 + 定位行), 避免上次数据残留 */
    LCD_Fill_Color(modle_top.x_start, modle_wifi.y_start,
                   modle_top.x_start + modle_top.width - 1,
                   modle_location.y_start + modle_location.height - 1, back_color);

    /* ---- 第一行: WiFi 图标 + 名称 ---- */
    if (wifi_info.connected)
        LCD_Draw_Picture_AutoTransparent(modle_wifi.x_start, modle_wifi.y_start,
                                         (const Image_t *)&Image_wifi, back_color);
    else
        LCD_Draw_Picture_AutoTransparent(modle_wifi.x_start, modle_wifi.y_start,
                                         (const Image_t *)&Image_wifi_off, back_color);

    if (wifi_info.ssid[0] != '\0')
        Main_Page_FitText(wifi_info.ssid, text, sizeof(text), max_px);
    else
        strcpy(text, "--");

    LCD_Write_String(text_x, (uint16_t)(modle_wifi.y_start + 2U), text, COLOR_BLACK, back_color, &Font_16B);

    /* ---- 第二行: 定位图标 + 省/市(布局与 WiFi 行一致) ---- */
    if (g_location[0] != '\0')
        LCD_Draw_Picture_AutoTransparent(modle_location.x_start, modle_location.y_start,
                                         (const Image_t *)&Image_location, back_color);
    else
        LCD_Draw_Picture_AutoTransparent(modle_location.x_start, modle_location.y_start,
                                         (const Image_t *)&Image_no_location, back_color);

    if (g_location[0] != '\0')
        Main_Page_FitText(g_location, text, sizeof(text), max_px);
    else
        strcpy(text, "--");

    LCD_Write_String(text_x, (uint16_t)(modle_location.y_start + 2U), text, COLOR_BLACK, back_color, &Font_16B);
}
