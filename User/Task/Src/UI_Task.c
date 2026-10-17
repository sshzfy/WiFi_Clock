#include "UI_Task.h"

/* LCD/OLED 状态(仅uiTask访问) */
static bool s_lcd_on = true;
static bool s_oled_on = false;

static void RTC_Ensure_Init(void);
static bool RTC_Sync_From_SystemClock(void);
static bool RTC_ReadDataTime(AT_Date_Info_t *out);
static void LCD_Touch_DrawTestPoints(uint16_t x, uint16_t y);
static void LCD_TouchTest(void);
static void Detect_TouchRange(void);
static void UI_TopBar_Draw(void);
static void UI_Lvgl_Handler(void);
static void UI_Lvgl_Init(void);
static void UI_Keyboard_Open(UI_Screen_t screen);
static void UI_Keyboard_Close(void);
static void UI_Task(void *pvParameters);

/* ==================== 低功耗设计 ==================== */
static void RTC_Ensure_Init(void)
{
    static bool rtc_inited = false; // 是否初始化RTC
    if (!rtc_inited)
    {
        DS1302_Init();
        rtc_inited = true;
    }
}

/** @brief 从系统时钟同步RTC时间
 *  @return true 成功, false 失败
 *  @note 首次调用时, 初始化RTC, 后续调用仅检查是否已初始化
 *        首次调用时, 从系统时钟获取时间, 后续调用仅对比时间是否一致, 允许分钟误差2分钟
 */
static bool RTC_Sync_From_SystemClock(void)
{
    AT_Date_Info_t t;
    DS1302_Time_t rtc, check; // rtc-写入外部RTC的时间,check-回读对比时间

    if (!Clock_IsSynced())
        return false;

    Clock_GetDateTime(&t);
    if ((t.year < 2000))
        return false;

    rtc.year = t.year;
    rtc.month = t.month;
    rtc.day = t.day;
    rtc.hour = t.hour;
    rtc.min = t.minute;
    rtc.sec = t.second;
    rtc.week = (t.weekday >= 1 && t.weekday <= 7) ? t.weekday : 1;

    RTC_Ensure_Init();
    if (!DS1302_SetTime(&rtc))
        return false;

    if (!DS1302_ReadTime(&check))
        return false;

    /* 对比时间是否一致: 用"当日分钟总数"比较, 允许写入耗时造成的 2 分钟偏差(含跨小时) */
    int base_min = (int)rtc.hour * 60 + (int)rtc.min;
    int check_min = (int)check.hour * 60 + (int)check.min;
    int diff_min = check_min - base_min;

    bool ret = (check.year == rtc.year) &&
               (check.month == rtc.month) &&
               (check.day == rtc.day) &&
               (check.week == rtc.week) &&
               (diff_min >= 0) && (diff_min <= 2);

    return ret;
}

/** @brief 从RTC读取数据时间
 *  @param out 输出时间结构体指针
 *  @return true 成功, 从RTC读取时间, false 失败, 从系统时钟获取时间
 */
static bool RTC_ReadDataTime(AT_Date_Info_t *out)
{
    DS1302_Time_t rtc;

#if RTC_LP_CLK_SOURCE

    /* 从RTC读取时间 */
    RTC_Ensure_Init();
    if (DS1302_ReadTime(&rtc))
    {
        out->year = rtc.year;
        out->month = rtc.month;
        out->day = rtc.day;
        out->hour = rtc.hour;
        out->minute = rtc.min;
        out->second = rtc.sec;
        out->weekday = (rtc.week >= 1 && rtc.week <= 7) ? rtc.week : 1;

        return true;
    }
#endif

    /* 从系统时钟获取时间 */
    Clock_GetDateTime(out);

    return false;
}

/* ==================== 双屏切换 ==================== */
void UI_Task_EnterNight(void)
{
    static bool oled_inited = false; // 默认未初始化OLED
    AT_Date_Info_t t;

    /* 进入 lowpower 模式前, 同步RTC时间 */
    if (Clock_IsSynced())
    {
        bool ret = RTC_Sync_From_SystemClock();
        printf("[LP] Sync RTC time %s\r\n", ret ? "done" : "fail");
    }

    /* 进入低功耗模式 */
    APP_SET_LOWPOWER(true);

    /* 初始化OLED, 时间优先取RTC */
    if (!oled_inited)
    {
        OLED_Init();
        OLED_Clear();
        oled_inited = true;
    }
    OLED_Display_On(); // 0xAF 开显示 + 清屏

    bool time_source = RTC_ReadDataTime(&t);                  // 从RTC读取时间
    OLED_ShowClock(t.hour, t.minute, t.year, t.month, t.day); // 显示时间+日期

    /* 关闭LCD，并置全局状态位 */
    LCD_Display_Power(false); // 关闭背光 + 0x28 显示睡眠
    s_lcd_on = false;         // 关闭LCD
    s_oled_on = true;         // 显示OLED

    printf("[LP] Enter night: LCD off, OLED on, time_source=%s\r\n", time_source ? "RTC" : "SoftClock");
}

void UI_Task_EnterDay(void)
{
    OLED_Display_Off();
    LCD_Display_Power(true); // 0x29 显示开 + 开背光

    uint32_t prof_t0 = Prof_Cycles(); // 整页重绘耗时(DWT CYCCNT)
    Main_Page_Display();              // 夜间未刷新, 全量重绘一次
    UI_TopBar_Draw();                 // 顶部条一行式: 覆盖 Main_Page 里的旧双行
    printf("[PROF] main page render: %u us\r\n", (unsigned)Prof_Us(prof_t0));

    s_lcd_on = true;   // 开启LCD
    s_oled_on = false; // 关闭OLED

    /* 通知网络/传感器更新数据 */
    APP_SET_LOWPOWER(false); // 退出低功耗模式
    xEventGroupSetBits(Task_Events(), EV_NET_UPDATE_NOW | EV_DHT22_UPDATE_NOW);

    printf("[UI] Enter day: OLED off, LCD on\r\n");
}

/* 当前是否处于低功耗(夜间)模式 */
bool UI_Task_IsLowPower(void)
{
    return APP_IS_LOWPOWER();
}

/* ==================== 触控事件处理 ==================== */

/** @brief 校准点的绘制回调: 每个点先清屏再画十字, 用户照着十字点
 *  @param x,y 校准点的屏幕坐标
 */
static void LCD_Touch_DrawTestPoints(uint16_t x, uint16_t y)
{
    LCD_Fill_Color(0U, 0U, WIDTH - 1U, HEIGHT - 1U, COLOR_BLACK);
    /* 横线 + 竖线组成十字 */
    LCD_Fill_Color((uint16_t)(x - 10U), (uint16_t)(y - 1U), (uint16_t)(x + 10U), (uint16_t)(y + 1U), COLOR_WHITE);
    LCD_Fill_Color((uint16_t)(x - 1U), (uint16_t)(y - 10U), (uint16_t)(x + 1U), (uint16_t)(y + 10U), COLOR_WHITE);
}

/** @brief 触摸校准测试: 检查触摸中断是否正常, 校准触摸参数
 *  @note 校准失败时, 会提示用户点击5个十字点, 校准成功后, 会保存校准参数到Flash
 */
static void LCD_TouchTest(void)
{
    printf("[UI] LCD_TouchTest start\r\n");

    LCD_Fill_Color(0U, 0U, WIDTH - 1U, HEIGHT - 1U, COLOR_BLACK);

    if (!Touch_Init())
    {
        printf("[Touch] FAIL: touch init failed (SPI3 not ready?)\r\n");
        for (;;)
            ;
    }

    /* 检查触摸中断是否正常 */
    printf("[Touch] PENIRQ idle = %s\r\n",
           Touch_IsPressed() ? "LOW  (error)" : "HIGH (ok)");
    /* 注册校准点绘制回调 */
    Touch_SetPoints_RegisterCallback(LCD_Touch_DrawTestPoints);

    /* Touch_Init() 已经尝试从 /data/touch_cal.bin 读回上次的校准参数
     * TOUCH_FORCE_CALIBRATE 置 1 重新校准触摸参数 */
#if (TOUCH_FORCE_CALIBRATE == 1)
    printf("[Touch] FORCE_CALIBRATE on: re-calibrating\r\n");
    calibration_params.calibrated = false;
#endif

    /* 是否已校准触摸参数 */
    if (calibration_params.calibrated)
    {
        printf("[Touch] calibration loaded: kx=%.4f ky=%.4f bx=%d by=%d\r\n",
               calibration_params.kx, calibration_params.ky,
               (int)calibration_params.bx, (int)calibration_params.by);
        printf("[Touch] using saved calibration, no calibration needed\r\n");
    }
    else
    {
        printf("[Touch] no calibration data, click the 5 crosses on the panel\r\n");

        if (Touch_Calibrate())
            printf("[Touch] calibration done and saved: next boot uses it directly\r\n");
        else if (calibration_params.calibrated)
            printf("[Touch] calibration OK but NOT saved (RAM only), will redo next boot\r\n");
        else
            printf("[Touch] WARN: calibration failed, touch falls back to raw coordinates\r\n");
    }
}

static void Detect_TouchRange(void)
{
    TouchState_t status;
    uint16_t x, y;

    if (Touch_GetPoint(&status))
    {
        x = status.x_display;
        y = status.y_display;
        printf("[Touch] position: %d, %d\r\n", x, y);

        /* 点击了WiFi图标 → WiFi配网界面 */
        if (x >= TOUCH_X_WIFI && x < TOUCH_X_WIFI + TOUCH_SIZE_WIFI &&
            y >= TOUCH_Y_WIFI && y < TOUCH_Y_WIFI + TOUCH_SIZE_WIFI)
        {
            xEventGroupSetBits(Task_Events(), EV_KEYBOARD);
        }
        /* 点击了定位图标 → 定位输入界面 */
        else if (x >= TOUCH_X_LOC && x < TOUCH_X_LOC + TOUCH_SIZE_LOC &&
                 y >= TOUCH_Y_LOC && y < TOUCH_Y_LOC + TOUCH_SIZE_LOC)
        {
            xEventGroupSetBits(Task_Events(), EV_LOCATION_KB);
        }
    }
}

/* ==================== 顶部状态条(一行式) ====================
 * 布局: 左上角并排两个 20x20 图标(WiFi 在 x=5, 定位在 x=26, y 都是 5),
 *       右上角一条信息 "[WiFi名,城市名]", 两个名字各自最多 10 字符, 超长截断。
 * 信息宽度按 ASCII 字符算(16 号字单字符宽 = Font_16B.size / 2);
 * 中文暂不支持 —— 城市名是中文时宽度会算偏, 文字可能左移, 但不会越出屏幕。
 * 注: Main_Page.c 里的 Main_Page_Net_Update() 仍是旧的"双行"画法,
 *     每次 Main_Page_Display() 之后由本函数清屏重画成一行, 视觉上只有一行。
 * ============================================================ */
static void UI_TopBar_Draw(void)
{
    uint16_t back_color = MAIN_TOP_BACK_COLOR;
    char wifi_name[TOPBAR_NAME_CHARS + 1U] = {0};
    char city_name[TOPBAR_NAME_CHARS + 1U] = {0};
    char info[TOPBAR_INFO_SIZE] = {0};
    uint16_t len;
    uint16_t info_x;

    /* 先清掉这一行(图标 + 信息) */
    LCD_Fill_Color(TOPBAR_WIFI_X, TOPBAR_Y,
                   (uint16_t)(WIDTH - TOPBAR_MARGIN), (uint16_t)(TOPBAR_Y + TOPBAR_ICON_SIZE - 1U),
                   back_color);

    /* 左上角: WiFi 图标 */
    if (wifi_info.connected)
        LCD_Draw_Picture_AutoTransparent(TOPBAR_WIFI_X, TOPBAR_Y,
                                         (const Image_t *)&Image_wifi, back_color);
    else
        LCD_Draw_Picture_AutoTransparent(TOPBAR_WIFI_X, TOPBAR_Y,
                                         (const Image_t *)&Image_wifi_off, back_color);

    /* 左上角: 定位图标 */
    if (g_location[0] != '\0')
        LCD_Draw_Picture_AutoTransparent(TOPBAR_LOC_X, TOPBAR_Y,
                                         (const Image_t *)&Image_location, back_color);
    else
        LCD_Draw_Picture_AutoTransparent(TOPBAR_LOC_X, TOPBAR_Y,
                                         (const Image_t *)&Image_no_location, back_color);

    /* 右上角: [WiFi名,城市名]; 两个名字各自截断到 10 字符, 再拼成一条 */
    snprintf(wifi_name, sizeof(wifi_name), "%s",
             (wifi_info.ssid[0] != '\0') ? wifi_info.ssid : "--");
    snprintf(city_name, sizeof(city_name), "%s",
             (g_location[0] != '\0') ? g_location : "--");
    snprintf(info, sizeof(info), "[%s,%s]", wifi_name, city_name);

    len = (uint16_t)strlen(info);
    info_x = (uint16_t)(WIDTH - TOPBAR_MARGIN - len * (uint16_t)(Font_16B.size / 2U));
    LCD_Write_String(info_x, (uint16_t)(TOPBAR_Y + 2U), info, COLOR_BLACK, back_color, &Font_16B);
}

/* ================ LVGL: 输入界面(WiFi / 定位) ================
 * 主页面(时钟/日期/天气/室内温湿度)仍然是裸绘的, LVGL 只接管"点了左上角图标
 * 之后"的输入界面: WiFi 图标 → ui_WiFi(配网), 定位图标 → ui_location(省市)。
 * ================================ */
static bool s_lvgl_ready = false; // LVGL 是否已初始化(只初始化一次)
static uint32_t s_lvgl_tick_ms = 0;
static UI_Screen_t s_kb_screen = UI_SCR_MAIN; // 当前打开的输入界面

/* 推进 LVGL 心跳并处理一次它的定时器(读触摸 + 刷屏都在这里发生) */
static void UI_Lvgl_Handler(void)
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

/* 首次进入键盘界面时初始化 LVGL 并建立界面 */
static void UI_Lvgl_Init(void)
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
 *  @note  进入前会把输入状态复位: 上次退出时残留的按下/焦点状态会让
 *         这一次点击收不到 LV_EVENT_FOCUSED, 键盘就再也显示不出来。
 */
static void UI_Keyboard_Open(UI_Screen_t screen)
{
    if (!s_lvgl_ready)
        UI_Lvgl_Init();

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

        /* 直接把键盘显示出来并聚焦 SSID 输入框: 用户点进来就能打字,
         * 不依赖"先点一下输入框"才弹出键盘 */
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

    s_kb_screen = screen;
    printf("[UI] %s screen opened\r\n", (screen == UI_SCR_WIFI) ? "WiFi" : "location");
}

/** @brief 退出输入界面
 *  @note  只隐藏不销毁: 下次进来直接复用, 省下重新建对象的时间与内存
 */
static void UI_Keyboard_Close(void)
{
    UI_Screen_t closed = s_kb_screen;

    s_kb_screen = UI_SCR_MAIN;

    if (s_lvgl_ready)
    {
        /* 两个界面的软键盘都收起来, 免得下次进来带着上一次的状态 */
        if (ui_Keyboard1 != NULL)
        {
            lv_obj_add_flag(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN);
        }
        if (ui_Keyboard2 != NULL)
        {
            lv_obj_add_flag(ui_Keyboard2, LV_OBJ_FLAG_HIDDEN);
        }
    }

    printf("[UI] %s screen closed\r\n", (closed == UI_SCR_WIFI) ? "WiFi" : "location");
}

/* ==================== uiTask(LCD唯一写者) ==================== */
static void UI_Task(void *pvParameters)
{
    (void)pvParameters;

    EventBits_t bits = 0; // 事件位, 用于等待uiTask的事件

    /* 事件组: 仅被等待的bit置位才会唤醒 */
    Task_Common_Init();

    /* TIM5/LCD/USART2/LCD 和触摸初始化 */
    Board_Init();

    /* 开始屏幕触摸检测 */
    LCD_TouchTest();
    printf("[UI] UI_Task start\r\n");
    printf("[UI] Board init done, boot page\r\n");

    /* 取回上次保存的WiFi凭据与定位信息(Asset层只有uiTask访问, 所以放在这里载入):
     * 没有就是空, 后续按"未配置"处理 */
    WiFi_Config_Load();
    Location_Config_Load();

    /* 资源层未就绪时字模与图片都取不到, 屏幕只会出现纯色块。
     * 这里只提示, 不阻塞流程 —— 网络与传感器功能仍可正常使用。 */
    if (!Asset_Ready())
    {
        printf("[UI] WARN: asset layer not ready, screen shows blank blocks\r\n");
        printf("[UI] WARN: flash the provision firmware to write fonts/images\r\n");
    }

    /* 开机等待页面 */
    Boot_Page_Wait();

    /* 创建DHT22/服务任务(sensor=4保护DHT22时序, net=2低于ui避免饿死显示) */
    DHT22_Task_Start();
    Net_Task_Start();

    /* 等待开机网络阶段结束 */
    EventBits_t boot_bits = xEventGroupWaitBits(Task_Events(), EV_NET_READY, pdTRUE, pdFALSE,
                                                pdMS_TO_TICKS(UI_BOOT_NET_TIMEOUT_MS));
    if ((boot_bits & EV_NET_READY) == 0)
    {
        printf("[UI] WARN: boot net stage timeout, showing partial result\r\n");
    }

    /* 网络结果由 netTask 提供, UI 不再直接读它的内部状态 */
    bool boot_wifi = false, boot_service = false;
    Net_Task_GetBoot(&boot_wifi, &boot_service); // 获取开机网络结果
    printf("[UI] Boot net stage done: wifi=%d service=%d\r\n", (int)boot_wifi, (int)boot_service);

    /* 开机结果(连接详情)
     * 结果页停留, 便于查看详情 */
    Boot_Page_Show(boot_wifi, boot_service);
    vTaskDelay(pdMS_TO_TICKS(2500));
    printf("[UI] Enter main page\r\n");

    uint32_t prof_t0 = Prof_Cycles(); // 整页重绘耗时(DWT CYCCNT)

    /* 进入主页面(数据已就绪, 首绘即正确) */
    Main_Page_Display();
    /* 顶部条 */
    UI_TopBar_Draw();
    printf("[PROF] main page render: %u us\r\n", (unsigned)Prof_Us(prof_t0));

    /* 进入主页面后再启动光敏任务与按键任务: 保证昼夜切换只发生在开机完成之后 */
    LightSensor_Task_Start();
    Key_Task_Start();

    for (;;)
    {
        /* 键盘界面激活时用小心跳(10ms), 让 LVGL 及时响应触摸与绘制;
         *   白天主界面 50ms: Detect_TouchRange() 是轮询式的, 节拍必须明显小于一次点击的
         *   持续时间(约100~200ms), 否则整段点击会落在两次采样之间, 表现成"点了没反应";
         * 夜间放宽到 2s(那时没有触摸需求, 只按分钟刷 OLED) */
        TickType_t wait;

        if (KB_ACTIVE())
        {
            wait = pdMS_TO_TICKS(10);
        }
        else if (APP_IS_LOWPOWER())
        {
            wait = pdMS_TO_TICKS(EV_LP_UI_TICK_MS);
        }
        else
        {
            wait = pdMS_TO_TICKS(UI_TOUCH_POLL_MS);
        }

        bits = xEventGroupWaitBits(Task_Events(),
                                   EV_WEATHER | EV_DHT22 | EV_WIFI | EV_LOWERPOWER | EV_WAKEUP |
                                       EV_KEYBOARD | EV_LOCATION_KB | EV_MAIN_SCREEN,
                                   pdTRUE, pdFALSE, wait);

        /* 昼夜切换(阻塞等待ACK的光敏任务需要该确认) */
        if (bits & EV_LOWERPOWER)
        {
            /* 夜晚收起键盘界面 */
            if (KB_ACTIVE())
            {
                UI_Keyboard_Close();
            }

            UI_Task_EnterNight();
            xEventGroupSetBits(Task_Events(), EV_LOWPOWER_ACK); // 确认夜间切换
        }
        if (bits & EV_WAKEUP)
        {
            UI_Task_EnterDay();
            xEventGroupSetBits(Task_Events(), EV_LOWPOWER_ACK); // 确认白天切换
        }

        /* 输入界面: 进入由"点了左上角图标"触发, 退出由界面上的确认/取消按键触发 */
        if ((bits & EV_KEYBOARD) && s_lcd_on && !KB_ACTIVE())
        {
            UI_Keyboard_Open(UI_SCR_WIFI);
        }
        else if ((bits & EV_LOCATION_KB) && s_lcd_on && !KB_ACTIVE())
        {
            UI_Keyboard_Open(UI_SCR_LOCATION);
        }

        /* 主页面: 点击返回键收起键盘界面 */
        if (bits & EV_MAIN_SCREEN)
        {
            if (KB_ACTIVE())
            {
                UI_Keyboard_Close();
            }

            /* 界面提交过新内容就保存进 Flash */
            if (WiFi_Config_IsDirty())
            {
                (void)WiFi_Config_Save();
            }
            if (Location_Config_IsDirty())
            {
                (void)Location_Config_Save();
            }

            if (s_lcd_on)
            {
                Main_Page_Display();
                /* 顶部条 */
                UI_TopBar_Draw();
            }
        }

        if (KB_ACTIVE())
        {
            /* 输入界面占据LCD: 主页面的局部刷新会把界面画面盖掉, 一律推迟到退出时整屏重画 */
            UI_Lvgl_Handler();
        }
        else if (s_lcd_on)
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

        /* 夜间OLED仅显示时间+日期(分钟变化才刷新); 白天刷新LCD时钟 */
        if (s_oled_on)
        {
            static uint8_t last_h = 0xFF, last_m = 0xFF, last_d = 0xFF;
            AT_Date_Info_t t;

            /* 夜间时间优先取外部RTC, 失败时内部已回退软件时钟 */
            (void)RTC_ReadDataTime(&t);
            if (t.hour != last_h || t.minute != last_m || t.day != last_d)
            {
                last_h = t.hour;
                last_m = t.minute;
                last_d = t.day;
                OLED_ShowClock(t.hour, t.minute, t.year, t.month, t.day);
            }
        }
        /* 时钟/日期刷新
         * 检测是否有按下左上角图标(WiFi/定位); 输入界面期间由 LVGL 独占触摸, 不再手工判区域 */
        else if (!KB_ACTIVE())
        {
            Main_Page_Clock_Update();
            Detect_TouchRange();
        }
    }
}

/* uiTask: LCD唯一写者 + 开机流程编排者(创建 net/sensor 任务) */
void UI_Task_Start(void)
{
    xTaskCreate(UI_Task, "ui", UI_TASK_STACK_SIZE, NULL, UI_TASK_PRIORITY, NULL);
}
