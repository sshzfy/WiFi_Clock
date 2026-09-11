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
#include "LCD.h"
#include "OLED.h"

#define DEBOUNCE_MS                 (2000UL) /* 昼夜切换去抖 2s */

/* ================ 开机网络阶段结果(仅uiTask在EV_NET_READY后读取) ================ */
typedef struct
{
    volatile bool wifi_ok;    // 开机WiFi初始化+连接是否完成(成功或失败)
    volatile bool service_ok; // 开机SNTP+天气是否都成功
} NetBoot_t;

/* ================ 周期参数(单位: 秒) ================ */
#define SNTP_PERIOD_S               (4UL * 3600UL) /* SNTP 4h */
#define WEATHER_PERIOD_S            (3600UL)       /* 天气 1h */
#define WIFI_PERIOD_S               (60UL)         /* WiFi检查 60s(缩短以更快感知掉线) */
#define RETRY_SNTP_S                (5UL)          /* SNTP重试 5s */
#define RETRY_WEATHER_S             (60UL)         /* 天气重试 60s */
#define RETRY_WIFI_S                (10UL)         /* WiFi重试 10s */

/* ================ UI 事件位(生产者→uiTask 通知) ================ */
#define EV_WEATHER                  (1UL << 0) /* 天气已更新, 请刷新天气模块 */
#define EV_ROOM                     (1UL << 1) /* 房间温湿度已更新, 请刷新房间模块 */
#define EV_NET_READY                (1UL << 2) /* 开机网络阶段完成(无论成败) */
#define EV_WIFI                     (1UL << 3) /* WiFi连接状态变化, 请刷新顶部状态条 */
#define EV_LIGHT                    (1UL << 4) /* 光敏传感器值变化, 判断进入低功耗模式 */
#define EV_LOWERPOWER               (1UL << 5) /* 进入低功耗模式 */
#define EV_LOWPOWER_ACK             (1UL << 6) /* 低功耗模式确认 */
#define EV_WAKEUP                   (1UL << 7) /* 从低功耗模式唤醒 */

/* 创建UI任务(UI任务内部再创建 netTask/sensorTask), 由 main 调用 */
void App_Task_Init(void);

#endif /* __APP_TASK_H__ */
