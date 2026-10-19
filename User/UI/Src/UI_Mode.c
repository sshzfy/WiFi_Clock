#include "UI_Mode.h"

/* ==================== 状态 ==================== */

/** @brief 是否夜间模式 */
bool UI_Mode_IsNight(void)
{
    return APP_IS_LOWPOWER();
}

/* ==================== 昼夜切换 ==================== */

/** @brief 开机首绘: 进入白天并画好主页面
 *  @note  此时 net/dht22 任务刚启动, 不发"立即更新"事件; 首绘数据已由开机流程取好
 */
void UI_Mode_Init(void)
{
    UI_Day_Enter();
    DBG_PRINTF("[UI] Enter main page\r\n");
    UI_Day_Repaint();
}

/** @brief 进入夜间
 *  @note  顺序: 收起输入界面 → 置低功耗 → 点亮 OLED → 最后关 LCD
 */
void UI_Mode_SwitchToNight(void)
{
    UI_Input_Close();       // 夜间不保留输入界面(LVGL 不能再刷 LCD)
    APP_SET_LOWPOWER(true); // 先置位: 网络与温湿度采集尽快停止
    UI_Night_Enter();       // 同步 RTC、点亮 OLED 并显示时间
    UI_Day_Leave();         // 最后关 LCD, 完成画面交接

    DBG_PRINTF("[LP] Enter night: LCD off, OLED on\r\n");
}

/** @brief 回到白天
 *  @note  顺序: 关 OLED → 开 LCD → 整页重绘 → 退出低功耗 → 通知网络/温湿度立即取数
 */
void UI_Mode_SwitchToDay(void)
{
    UI_Night_Leave();
    UI_Day_Enter();
    UI_Day_Repaint();

    APP_SET_LOWPOWER(false); // 退出低功耗模式

    /* 通知网络/传感器更新数据: 夜间它们停更, 回白天要立刻拿到新值 */
    xEventGroupSetBits(Task_Events(), EV_NET_UPDATE_NOW | EV_DHT22_UPDATE_NOW);

    DBG_PRINTF("[UI] Enter day: OLED off, LCD on\r\n");
}

/* ==================== 节拍与刷新 ==================== */

/** @brief 主循环本次等待的节拍 */
TickType_t UI_Mode_WaitTicks(void)
{
    /* 输入界面 10ms */
    if (UI_Input_IsActive())
        return pdMS_TO_TICKS(10);

    /* 夜间 2s */
    if (UI_Mode_IsNight())
        return pdMS_TO_TICKS(EV_LP_UI_TICK_MS);

    /* 白天 50ms */
    return pdMS_TO_TICKS(UI_TOUCH_POLL_MS);
}

/** @brief 当前模式的页面事件
 *  @param bits 本轮等待到的事件位
 *  @note  输入界面的开关/保存由 UI_Input 处理; 夜间不刷新页面数据。
 */
void UI_Mode_HandleEvent(EventBits_t bits)
{
    bool back_home; // 是否返回主页面

    /* 夜间不允许打开输入界面, 但"返回主页面"仍要处理(清理与保存) */
    back_home = UI_Input_HandleEvent(bits, !UI_Mode_IsNight());

    /* 夜间不刷新页面数据 */
    if (UI_Mode_IsNight())
        return;

    /* 从输入界面回到主页面,整页重绘 */
    if (back_home)
        UI_Day_Repaint();

    /* 输入界面激活时, 主页面的局部刷新会把画面盖掉, 一律推迟到退出时 */
    if (!UI_Input_IsActive())
        UI_Day_HandleEvent(bits);
}

/** @brief 当前模式的周期刷新: 键盘心跳 / 夜间分钟时钟 / 白天时钟 + 触摸轮询 */
void UI_Mode_Tick(void)
{
    if (UI_Input_IsActive())
    {
        UI_Input_Tick();
        return;
    }

    if (UI_Mode_IsNight())
    {
        UI_Night_Tick();
        return;
    }

    UI_Day_RefreshClock();
    UI_Input_PollTouch();
}
