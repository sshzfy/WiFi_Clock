#include "UI_Input.h"

/* 新增图标时只要在这张表里加一行, 不用改命中判断 */
static const UI_Hotspot_t s_hotspots[] = {
    {TOUCH_X_WIFI, TOUCH_Y_WIFI, TOUCH_SIZE_WIFI, EV_WIFI_KB},
    {TOUCH_X_LOC, TOUCH_Y_LOC, TOUCH_SIZE_LOC, EV_LOCATION_KB},
};

/* ==================== 模块内状态(仅uiTask访问) ==================== */
static bool s_lvgl_ready = false;                 // LVGL 是否已初始化(只初始化一次)
static uint32_t s_lvgl_tick_ms = 0;               // 上次喂给 LVGL 心跳的毫秒数
static UI_Screen_t s_active_screen = UI_SCR_MAIN; // 当前打开的输入界面

/* ==================== static 函数 ==================== */
static void UI_Input_LvglInit(void);
static void UI_Touch_DrawTestPoints(uint16_t x, uint16_t y);

/* ==================== 触摸校准 ==================== */
static void UI_Touch_DrawTestPoints(uint16_t x, uint16_t y)
{
    LCD_Fill_Color(0U, 0U, WIDTH - 1U, HEIGHT - 1U, COLOR_BLACK);
    /* 横线 + 竖线组成十字 */
    LCD_Fill_Color((uint16_t)(x - 10U), (uint16_t)(y - 1U), (uint16_t)(x + 10U), (uint16_t)(y + 1U), COLOR_WHITE);
    LCD_Fill_Color((uint16_t)(x - 1U), (uint16_t)(y - 10U), (uint16_t)(x + 1U), (uint16_t)(y + 10U), COLOR_WHITE);
}

/** @brief 触摸校准测试: 检查触摸中断是否正常, 校准触摸参数
 *  @note 校准失败时, 会提示用户点击5个十字点, 校准成功后, 会保存校准参数到Flash
 */
void UI_Input_Calibrate(void)
{
    DBG_PRINTF("[UI] LCD_TouchTest start\r\n");

    LCD_Fill_Color(0U, 0U, WIDTH - 1U, HEIGHT - 1U, COLOR_BLACK);

    if (!Touch_Init())
    {
        DBG_PRINTF("[Touch] FAIL: touch init failed (SPI3 not ready?)\r\n");
        for (;;)
            ;
    }

    /* 检查触摸中断是否正常 */
    DBG_PRINTF("[Touch] PENIRQ idle = %s\r\n",
               Touch_IsPressed() ? "LOW  (error)" : "HIGH (ok)");
    /* 注册校准点绘制回调 */
    Touch_SetPoints_RegisterCallback(UI_Touch_DrawTestPoints);

    /* Touch_Init() 已经尝试从 /data/touch_cal.bin 读回上次的校准参数
     * TOUCH_FORCE_CALIBRATE 置 1 重新校准触摸参数 */
#if (TOUCH_FORCE_CALIBRATE == 1)
    DBG_PRINTF("[Touch] FORCE_CALIBRATE on: re-calibrating\r\n");
    calibration_params.calibrated = false;
#endif

    /* 是否已校准触摸参数 */
    if (calibration_params.calibrated)
    {
        DBG_PRINTF("[Touch] calibration loaded: kx=%.4f ky=%.4f bx=%d by=%d\r\n",
                   calibration_params.kx, calibration_params.ky,
                   (int)calibration_params.bx, (int)calibration_params.by);
        DBG_PRINTF("[Touch] using saved calibration, no calibration needed\r\n");
    }
    else
    {
        DBG_PRINTF("[Touch] no calibration data, click the 5 crosses on the panel\r\n");

        if (Touch_Calibrate())
            DBG_PRINTF("[Touch] calibration done and saved: next boot uses it directly\r\n");
        else if (calibration_params.calibrated)
            DBG_PRINTF("[Touch] calibration OK but NOT saved (RAM only), will redo next boot\r\n");
        else
            DBG_PRINTF("[Touch] WARN: calibration failed, touch falls back to raw coordinates\r\n");
    }
}

/** @brief 轮询一次触摸, 命中左上角图标则置位对应事件 */
void UI_Input_PollTouch(void)
{
    TouchState_t status;
    uint16_t x, y;
    uint32_t i;

    if (!Touch_GetPoint(&status))
        return;

    x = (uint16_t)status.x_display;
    y = (uint16_t)status.y_display;
    DBG_PRINTF("[Touch] position: %d, %d\r\n", x, y);

    for (i = 0U; i < (uint32_t)(sizeof(s_hotspots) / sizeof(s_hotspots[0])); i++)
    {
        const UI_Hotspot_t *spot = &s_hotspots[i];

        if (x >= spot->x && x < (uint16_t)(spot->x + spot->size) &&
            y >= spot->y && y < (uint16_t)(spot->y + spot->size))
        {
            xEventGroupSetBits(Task_Events(), spot->event);
            return;
        }
    }
}

/* ==================== LVGL 输入界面 ==================== */

/** @brief 推进 LVGL 心跳并处理一次它的定时器(读触摸 + 刷屏都在这里发生) */
void UI_Input_Tick(void)
{
    uint32_t now = (uint32_t)TIM5_Get_ms();
    uint32_t elapsed = now - s_lvgl_tick_ms;

    if (elapsed != 0U)
    {
        s_lvgl_tick_ms = now;
        lv_tick_inc(elapsed); // LVGL 没有自己的时基, 由外部按毫秒喂
    }

    lv_timer_handler();
}

/** @brief 首次进入键盘界面时初始化 LVGL 并建立界面 */
static void UI_Input_LvglInit(void)
{
    lv_init();
    lv_port_disp_init();  // 注册显示: 刷屏借 LCD 的整行缓冲, 不额外占 SRAM
    lv_port_indev_init(); // 注册触摸: XPT2046 → 指针设备
    ui_init();            // SquareLine 生成的界面

    s_lvgl_tick_ms = (uint32_t)TIM5_Get_ms();
    s_lvgl_ready = true;
}

/** @brief 调出输入界面(WiFi 配网 / 定位信息)
 *  @param screen 要打开的界面
 */
void UI_Input_Open(UI_Screen_t screen)
{
    if (!s_lvgl_ready)
        UI_Input_LvglInit();

    /* 复位输入设备并重新对表: LVGL 的心跳按"距上次喂食的毫秒差"推进,
     * 而界面可能隔几小时才再次打开, 不重置会把所有定时器一次性推到超期 */
    lv_indev_reset(NULL, NULL);
    s_lvgl_tick_ms = (uint32_t)TIM5_Get_ms();

    /* 打开 WiFi 配网界面 */
    if (screen == UI_SCR_WIFI)
    {
        /* 无条件载屏: 不依赖 ui_init() 内部那一句, 保证一定停在目标界面 */
        lv_disp_load_scr(ui_WiFi);

        lv_obj_clear_state(ui_TextArea1, LV_STATE_FOCUSED);
        lv_obj_clear_state(ui_TextArea2, LV_STATE_FOCUSED);

        /* 清掉上一次"SSID 不能为空"的红色提示 */
        lv_label_set_text(ui_Label1, "SSID:");
        lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);

        /* 清空输入区 */
        lv_textarea_set_text(ui_TextArea1, "");
        lv_textarea_set_text(ui_TextArea2, "");

        /* 把键盘显示出来并聚焦 SSID 输入框  */
        lv_keyboard_set_textarea(ui_Keyboard1, ui_TextArea1);
        lv_obj_clear_flag(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_state(ui_TextArea1, LV_STATE_FOCUSED);
        /* 再补一次 FOCUSED 事件: 输入框的光标闪烁动画是在这个事件里启动的 */
        (void)lv_event_send(ui_TextArea1, LV_EVENT_FOCUSED, NULL);
    }
    /* 打开定位输入界面 */
    else
    {
        lv_disp_load_scr(ui_location);

        lv_obj_clear_state(ui_TextArea4, LV_STATE_FOCUSED);

        lv_label_set_text(ui_Label4, "location(province+city):");
        lv_obj_set_style_text_color(ui_Label4, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);

        /* 清空输入区 */
        lv_textarea_set_text(ui_TextArea4, "");

        lv_keyboard_set_textarea(ui_Keyboard2, ui_TextArea4);
        lv_obj_clear_flag(ui_Keyboard2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_state(ui_TextArea4, LV_STATE_FOCUSED);
        (void)lv_event_send(ui_TextArea4, LV_EVENT_FOCUSED, NULL);
    }

    /* 主页面是裸绘的, LVGL 内部并不知道屏幕已被覆盖, 强制整屏重绘一次 */
    lv_obj_invalidate(lv_scr_act());

    s_active_screen = screen;
    DBG_PRINTF("[UI] %s screen opened\r\n", (screen == UI_SCR_WIFI) ? "WiFi" : "location");
}

/** @brief 退出输入界面 */
void UI_Input_Close(void)
{
    UI_Screen_t closed = s_active_screen;

    if (closed == UI_SCR_MAIN)
        return;

    s_active_screen = UI_SCR_MAIN;

    if (s_lvgl_ready)
    {
        /* 隐藏键盘 */
        if (ui_Keyboard1 != NULL)
        {
            lv_obj_add_flag(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN);
        }
        if (ui_Keyboard2 != NULL)
        {
            lv_obj_add_flag(ui_Keyboard2, LV_OBJ_FLAG_HIDDEN);
        }
    }

    DBG_PRINTF("[UI] %s screen closed\r\n", (closed == UI_SCR_WIFI) ? "WiFi" : "location");
}

/** @brief 当前是否停在输入界面
 *  @return true 表示当前在输入界面, false 表示在主界面
 */
bool UI_Input_IsActive(void)
{
    return (s_active_screen != UI_SCR_MAIN);
}

/** @brief 处理输入界面的开关与输入结果落地
 *  @param bits       本轮等待到的事件位
 *  @param allow_open 是否允许打开输入界面(夜间为 false)
 *  @return true 表示本轮收到了"返回主页面"请求, 调用方需要重绘主页面
 */
bool UI_Input_HandleEvent(EventBits_t bits, bool allow_open)
{
    bool back_home = false; // 是否返回主页面

    /* 打开输入界面,只有在主界面时才允许 */
    if (allow_open && !UI_Input_IsActive())
    {
        if (bits & EV_WIFI_KB)
        {
            UI_Input_Open(UI_SCR_WIFI);
        }
        else if (bits & EV_LOCATION_KB)
        {
            UI_Input_Open(UI_SCR_LOCATION);
        }
    }

    /* 关闭输入界面 */
    if (bits & EV_MAIN_SCREEN)
    {
        UI_Input_Close();

        /* 界面提交过新内容就保存进 Flash */
        if (WiFi_Config_IsDirty())
        {
            (void)WiFi_Config_Save();
        }
        if (Location_Config_IsDirty())
        {
            (void)Location_Config_Save();
        }

        back_home = true;
    }

    return back_home;
}
