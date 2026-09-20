#ifndef __APP_TASK_H__
#define __APP_TASK_H__

#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"
#include "Board.h"
#include "Page.h"
#include "App.h"
#include "Light_Sensor.h"
#include "Key.h"
#include "LCD.h"
#include "OLED.h"
#include "External_RTC.h"
#include "Asset.h"
#include "Profiling.h"

#define RTC_LOWPOWER_ENABLE         1 // 低功耗模式时钟源选择: 1=DS1302, 0=软件时钟

#define DEBOUNCE_MS                 (2000UL) // 昼夜切换去抖 2s
#define EV_LP_UI_TICK_MS            (2000UL) // 低功耗UI事件等待(ms), 夜间只显示到分钟

/* ================ 按键手势参数(由 Key_Task 识别, 见 8.7) ================ */
#define KEY_LONG_PRESS_MS           (800UL) // 按住达到该时长即触发长按(不等松开)
#define KEY_MULTI_GAP_MS            (300UL) // 连击窗口: 超时即结算累计击数
#define KEY_CLICK_MAX               (3U)    // 最多识别到三击, 达到即立即结算
#define KEY_RELEASE_POLL_MS         (20UL)  // 长按触发后等待松开的轮询步长

/* ================ 开机网络阶段结果(仅uiTask在EV_NET_READY后读取) ================ */
typedef struct
{
    volatile bool wifi_ok;    // 开机WiFi初始化+连接是否完成(成功或失败)
    volatile bool service_ok; // 开机SNTP+天气是否都成功
} NetBoot_t;

/* ================ 周期参数(单位: 秒) ================ */
#define SNTP_PERIOD_S               (4UL * 3600UL) // SNTP 4h
#define WEATHER_PERIOD_S            (3600UL)       // 天气 1h
#define WIFI_PERIOD_S               (10UL * 60UL)  // WiFi检查 10min
#define DHT22_PERIOD_S              (5UL * 60UL)   // 房间温湿度 5min
#define RETRY_SNTP_S                (5UL)          // SNTP重试 5s
#define RETRY_WEATHER_S             (60UL)         // 天气重试 60s
#define RETRY_WIFI_S                (10UL)         // WiFi重试 10s

/* ================ UI 事件位(生产者→uiTask 通知) ================ */
#define EV_WEATHER                  (1UL << 0) // 天气已更新, 请刷新天气模块
#define EV_DHT22                    (1UL << 1) // 房间温湿度已更新, 请刷新房间模块
#define EV_NET_READY                (1UL << 2) // 开机网络阶段完成(无论成败)
#define EV_WIFI                     (1UL << 3) // WiFi连接状态变化, 请刷新顶部状态条
#define EV_LOWERPOWER               (1UL << 4) // 进入低功耗模式
#define EV_LOWPOWER_ACK             (1UL << 5) // 低功耗模式确认
#define EV_WAKEUP                   (1UL << 6) // 从低功耗模式唤醒
#define EV_NET_UPDATE_NOW           (1UL << 7) // 立即更新网络状态, 包括WiFi连接状态、SNTP时间、天气
#define EV_DHT22_UPDATE_NOW         (1UL << 8) // 立即更新DHT22传感器状态

/* 创建UI任务(UI任务内部再创建 netTask/sensorTask), 由 main 调用 */
void App_Task_Init(void);

#endif /* __APP_TASK_H__ */
