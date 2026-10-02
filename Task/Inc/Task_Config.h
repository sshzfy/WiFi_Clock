#ifndef __TASK_CONFIG_H__
#define __TASK_CONFIG_H__

/* ============================================================
 * 任务层集中配置
 * ------------------------------------------------------------
 * 这里只放"数值/开关", 不放任何函数原型与类型定义。
 * 任务优先级、栈大小、周期、时序参数集中在此, 避免散落在各个任务文件里。
 * ============================================================ */

/* ================ 任务优先级与栈大小(单位: word) ================
 * 优先级数值越大越高(configMAX_PRIORITIES = 5, 可用 0~4)。
 * DHT22 优先级必须高于 UI: DHT22 的位操作时序在 us 级, 被 UI 抢占会读失败。
 * net 与 UI 同级(2), 但 net 每拍只做一次判断就让出, 不会饿死显示。
 */
#define UI_TASK_PRIORITY               3
#define UI_TASK_STACK_SIZE             1024

#define NET_TASK_PRIORITY              2
#define NET_TASK_STACK_SIZE            1024

#define DHT22_TASK_PRIORITY            4
#define DHT22_TASK_STACK_SIZE          512

#define LIGHT_SENSOR_TASK_PRIORITY     2
#define LIGHT_SENSOR_TASK_STACK_SIZE   512

#define KEY_TASK_PRIORITY              2
#define KEY_TASK_STACK_SIZE            1024

/* ================ 周期参数(单位: 秒) ================ */
#define SNTP_PERIOD_S                  (4UL * 3600UL) // SNTP 4h
#define WEATHER_PERIOD_S               (3600UL)       // 天气 1h
#define WIFI_PERIOD_S                  (10UL * 60UL)  // WiFi检查 10min
#define DHT22_PERIOD_S                 (5UL * 60UL)   // 房间温湿度 5min
#define RETRY_SNTP_S                   (5UL)          // SNTP重试 5s
#define RETRY_WEATHER_S                (60UL)         // 天气重试 60s
#define RETRY_WIFI_S                   (10UL)         // WiFi重试 10s

/* ================ 昼夜切换与低功耗 ================ */
#define RTC_LOWPOWER_ENABLE            (1)      // 低功耗模式时钟源选择: 1=DS1302, 0=软件时钟
#define DEBOUNCE_MS                    (2000UL) // 昼夜切换去抖 2s
#define EV_LP_UI_TICK_MS               (2000UL) // 低功耗UI事件等待(ms), 夜间只显示到分钟

/* ================ 按键手势参数(由 Key_Task 识别) ================ */
#define KEY_LONG_PRESS_MS              (800UL) // 按住达到该时长即触发长按(不等松开)
#define KEY_MULTI_GAP_MS               (300UL) // 连击窗口: 超时即结算累计击数
#define KEY_CLICK_MAX                  (3U)    // 最多识别到三击, 达到即立即结算
#define KEY_RELEASE_POLL_MS            (20UL)  // 长按触发后等待松开的轮询步长

#endif /* __TASK_CONFIG_H__ */
