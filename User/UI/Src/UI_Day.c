#include "UI_Day.h"

static void UI_TopBar_Draw(void);

/** @brief 顶部状态条绘制 */
static void UI_TopBar_Draw(void)
{
    uint16_t back_color = MAIN_TOP_BACK_COLOR;
    char wifi_name[TOPBAR_NAME_CHARS + 1U] = {0};
    char city_name[TOPBAR_NAME_CHARS + 1U] = {0};
    char info[TOPBAR_INFO_SIZE] = {0};
    uint16_t len;
    uint16_t info_x;

    /* 先清掉顶部状态条 */
    LCD_Fill_Color(TOPBAR_WIFI_X, TOPBAR_Y,
                   (uint16_t)(WIDTH - TOPBAR_MARGIN), (uint16_t)(TOPBAR_Y + TOPBAR_ICON_SIZE - 1U),
                   back_color);

    /* WiFi 图标 */
    if (wifi_info.connected)
        LCD_Draw_Picture_AutoTransparent(TOPBAR_WIFI_X, TOPBAR_Y,
                                         (const Image_t *)&Image_wifi, back_color);
    else
        LCD_Draw_Picture_AutoTransparent(TOPBAR_WIFI_X, TOPBAR_Y,
                                         (const Image_t *)&Image_wifi_off, back_color);

    /* 定位图标 */
    if (g_location[0] != '\0')
        LCD_Draw_Picture_AutoTransparent(TOPBAR_LOC_X, TOPBAR_Y,
                                         (const Image_t *)&Image_location, back_color);
    else
        LCD_Draw_Picture_AutoTransparent(TOPBAR_LOC_X, TOPBAR_Y,
                                         (const Image_t *)&Image_no_location, back_color);

    /* [WiFi名,城市名]; 两个名字各自截断到 10 字符, 再拼成一条 */
    snprintf(wifi_name, sizeof(wifi_name), "%s",
             (wifi_info.ssid[0] != '\0') ? wifi_info.ssid : "--");
    snprintf(city_name, sizeof(city_name), "%s",
             (g_location[0] != '\0') ? g_location : "--");
    snprintf(info, sizeof(info), "[%s,%s]", wifi_name, city_name);

    len = (uint16_t)strlen(info);
    info_x = (uint16_t)(WIDTH - TOPBAR_MARGIN - len * (uint16_t)(Font_16B.size / 2U));
    LCD_Write_String(info_x, (uint16_t)(TOPBAR_Y + 2U), info, COLOR_BLACK, back_color, &Font_16B);
}

/* ==================== LCD 开关 ==================== */

/** @brief 进入白天模式: 打开 LCD 背光 */
void UI_Day_Enter(void)
{
    LCD_Display_Power(true); // 0x29 显示开 + 开背光
}

/** @brief 退出白天: 关闭 LCD 背光并让显示进入睡眠 */
void UI_Day_Leave(void)
{
    LCD_Display_Power(false); // 关闭背光 + 0x28 显示睡眠
}

/** @brief 整页重绘主页面
 *  @note  开机首绘、夜间回到白天、从输入界面返回主页面
 */
void UI_Day_Repaint(void)
{
    uint32_t prof_t0 = Prof_Cycles(); // 整页重绘耗时(DWT CYCCNT)

    Main_Page_Display();
    UI_TopBar_Draw();

    DBG_PRINTF("[PROF] main page render: %u us\r\n", (unsigned)Prof_Us(prof_t0));
}

/** @brief 时钟/日期刷新(内部按秒/分/时变化才真正重画) */
void UI_Day_RefreshClock(void)
{
    Main_Page_Clock_Update();
}

/** @brief 白天页面数据事件: WiFi / 天气 / 室内温湿度 */
void UI_Day_HandleEvent(EventBits_t bits)
{
    /* WiFi连接状态变化: 刷顶部状态条 */
    if (bits & EV_WIFI)
    {
        UI_TopBar_Draw();
    }
    /* 天气到位后同步城市名 */
    if (bits & EV_WEATHER)
    {
        Main_Page_Weather_Update();
        UI_TopBar_Draw();
    }
    /* 房间温度更新 */
    if (bits & EV_DHT22)
    {
        Main_Page_Room_Update();
    }
}
