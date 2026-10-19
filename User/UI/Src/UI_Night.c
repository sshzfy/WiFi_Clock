#include "UI_Night.h"

/* ==================== 模块内状态(仅uiTask访问) ==================== */
static bool s_oled_inited = false; // OLED 是否已初始化(只初始化一次)
static bool s_rtc_inited = false;  // DS1302 是否已初始化
static uint8_t s_last_hour = 0xFF; // 上次显示的小时
static uint8_t s_last_min = 0xFF;  // 上次显示的分钟
static uint8_t s_last_day = 0xFF;  // 上次显示的日期

/* ==================== static 函数 ==================== */
static void UI_Night_EnsureRTC(void);
static bool UI_Night_SyncRTC(void);
static bool UI_Night_ReadTime(AT_Date_Info_t *out);

/** @brief 确保RTC已初始化 */
static void UI_Night_EnsureRTC(void)
{
    if (!s_rtc_inited)
    {
        DS1302_Init();
        s_rtc_inited = true;
    }
}

/** @brief 从系统时钟同步RTC时间
 *  @return true 成功, false 失败
 *  @note 首次调用时, 初始化RTC, 后续调用仅检查是否已初始化
 *        首次调用时, 从系统时钟获取时间, 后续调用仅对比时间是否一致, 允许分钟误差2分钟
 */
static bool UI_Night_SyncRTC(void)
{
    AT_Date_Info_t tmp;
    DS1302_Time_t rtc, check; // rtc-写入外部RTC的时间,check-回读对比时间

    if (!Clock_IsSynced())
        return false;

    Clock_GetDateTime(&tmp);
    if ((tmp.year < 2000 || tmp.year > 2099))
        return false;
    if (tmp.month < 1 || tmp.month > 12)
        return false;
    if (tmp.day < 1 || tmp.day > 31)
        return false;
    if (tmp.hour > 23 || tmp.minute > 59 || tmp.second > 59)
        return false;

    rtc.year = tmp.year;
    rtc.month = tmp.month;
    rtc.day = tmp.day;
    rtc.hour = tmp.hour;
    rtc.min = tmp.minute;
    rtc.sec = tmp.second;
    rtc.week = (tmp.weekday >= 1 && tmp.weekday <= 7) ? tmp.weekday : 1;

    UI_Night_EnsureRTC();
    if (!DS1302_SetTime(&rtc))
        return false;

    if (!DS1302_ReadTime(&check))
        return false;

    /* 对比时间是否一致: 用"当日秒总数"比较, 允许写入耗时造成的 2 秒偏差 */
    int base_sec = (int)rtc.hour * 60 + (int)rtc.min * 60 + (int)rtc.sec;
    int check_sec = (int)check.hour * 60 + (int)check.min * 60 + (int)check.sec;
    int diff_sec = check_sec - base_sec;

    /* 跨天处理 */
    bool crossed = (diff_sec < 0);
    if (crossed)
        diff_sec += 24 * 60 * 60;

    bool ret = (check.year == rtc.year) &&
               (check.month == rtc.month) &&
               (check.day == rtc.day) &&
               (check.week == rtc.week) &&
               (diff_sec >= 0) && (diff_sec <= 2);

    return ret;
}

/** @brief 从RTC读取数据时间
 *  @param out 输出时间结构体指针
 *  @return true 成功, 从RTC读取时间, false 失败, 从系统时钟获取时间
 */
static bool UI_Night_ReadTime(AT_Date_Info_t *out)
{
    DS1302_Time_t rtc;

#if RTC_LP_CLK_SOURCE

    /* 从RTC读取时间 */
    UI_Night_EnsureRTC();
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

/* ==================== 夜间显示 ==================== */

/** @brief 进入夜间显示 */
void UI_Night_Enter(void)
{
    AT_Date_Info_t t;
    bool time_source;

    /* 进入低功耗模式前, 同步RTC时间 */
    if (Clock_IsSynced())
    {
        bool ret = UI_Night_SyncRTC();
        DBG_PRINTF("[LP] Sync RTC time %s\r\n", ret ? "done" : "fail");
    }

    /* 初始化OLED, 时间优先取RTC */
    if (!s_oled_inited)
    {
        OLED_Init();
        OLED_Clear();
        s_oled_inited = true;
    }
    OLED_Display_On(); // 0xAF 开显示 + 清屏

    time_source = UI_Night_ReadTime(&t);
    OLED_ShowClock(t.hour, t.minute, t.year, t.month, t.day);

    /* 进入时已经画过一次, 同步缓存: 下一拍同一分钟不会重复刷 OLED */
    s_last_hour = t.hour;
    s_last_min = t.minute;
    s_last_day = t.day;

    DBG_PRINTF("[LP] OLED clock source=%s\r\n", time_source ? "RTC" : "SoftClock");
}

void UI_Night_Leave(void)
{
    OLED_Display_Off();
}

/** @brief 夜间周期刷新: 分钟或日期变化才重画 */
void UI_Night_Tick(void)
{
    AT_Date_Info_t tmp;

    /* 夜间时间优先取外部RTC, 失败时内部已回退软件时钟 */
    (void)UI_Night_ReadTime(&tmp);

    if (tmp.hour != s_last_hour || tmp.minute != s_last_min || tmp.day != s_last_day)
    {
        s_last_hour = tmp.hour;
        s_last_min = tmp.minute;
        s_last_day = tmp.day;
        OLED_ShowClock(tmp.hour, tmp.minute, tmp.year, tmp.month, tmp.day);
    }
}
