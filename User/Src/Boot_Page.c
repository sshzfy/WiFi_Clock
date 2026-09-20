#include "Page.h"

extern AT_WiFi_Info_t wifi_info;
static char fail_msg[32] = {0};
static char success_line_1[32] = {0};
static char success_line_2[32] = {0};
static char success_line_3[32] = {0};

/**
 * @brief 绘制开机等待画面(纯显示, 不执行任何网络操作)
 * @param None
 * @return None
 */
void Boot_Page_Wait(void)
{
    ST7789_Draw_Picture(0, 0, (const Image_t *)&Boot_Page_Waitconnect); // 绘制开机页面等待连接图片
}

/**
 * @brief 根据开机网络阶段结果绘制开机结果(纯显示)
 *
 * @param wifi_ok    开机WiFi初始化+连接是否完成
 * @param service_ok 开机SNTP+天气是否都成功
 * @return None
 */
void Boot_Page_Show(bool wifi_ok, bool service_ok)
{
    if (!wifi_ok)
    {
        sprintf(fail_msg, "[WiFi] Disconnect");

        int str_len = strlen(fail_msg);
        int str_width = str_len * (Font_16.size / 2);
        int x_center = (WIDTH - str_width) / 2;

        ST7789_Fill_Color(0, 170, WIDTH - 1, HEIGHT - 1, COLOR_WHITE);                     // 填充白色背景,清除旧信息
        ST7789_Draw_Picture(100, 180, (const Image_t *)&Image_err);                        // 绘制错误图片
        ST7789_Write_String(x_center, 230, fail_msg, COLOR_BLACK, COLOR_WHITE, &Font_16B); // 失败信息
    }
    else if (!service_ok)
    {
        sprintf(fail_msg, "[Service] Init failed");

        int str_len = strlen(fail_msg);
        int str_width = str_len * (Font_16.size / 2);
        int x_center = (WIDTH - str_width) / 2;

        ST7789_Fill_Color(0, 170, WIDTH - 1, HEIGHT - 1, COLOR_WHITE);                     // 填充白色背景,清除旧信息
        ST7789_Draw_Picture(100, 180, (const Image_t *)&Image_err);                        // 绘制错误图片
        ST7789_Write_String(x_center, 230, fail_msg, COLOR_BLACK, COLOR_WHITE, &Font_16B); // 失败信息
    }
    else
    {
        sprintf(success_line_1, "[%s] Connect", wifi_info.ssid);
        sprintf(success_line_2, "Mac: %s", wifi_info.bssid);
        sprintf(success_line_3, "Channel: %d, RSSI: %d", wifi_info.channel, wifi_info.rssi);

        int char_width = Font_16.size / 2;
        int len1 = strlen(success_line_1);
        int len2 = strlen(success_line_2);
        int len3 = strlen(success_line_3);

        int w1 = len1 * char_width;
        int w2 = len2 * char_width;
        int w3 = len3 * char_width;

        int x1 = (WIDTH - w1) / 2; // 第1行的起始X
        int x2 = (WIDTH - w2) / 2; // 第2行的起始X
        int x3 = (WIDTH - w3) / 2; // 第3行的起始X

        /* 防负数保护 */
        if (x1 < 0)
            x1 = 0;
        if (x2 < 0)
            x2 = 0;
        if (x3 < 0)
            x3 = 0;

        ST7789_Fill_Color(0, 200, WIDTH - 1, HEIGHT - 1, COLOR_WHITE);                     // 填充白色背景,清除旧信息
        ST7789_Write_String(x1, 230, success_line_1, COLOR_BLACK, COLOR_WHITE, &Font_16B); // 成功信息
        ST7789_Write_String(x2, 250, success_line_2, COLOR_BLACK, COLOR_WHITE, &Font_16B); // 成功信息
        ST7789_Write_String(x3, 270, success_line_3, COLOR_BLACK, COLOR_WHITE, &Font_16B); // 成功信息
    }
}
