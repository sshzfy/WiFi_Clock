#ifndef __TASK_CONFIG_H__
#define __TASK_CONFIG_H__

/* ================ 任务优先级与栈大小(单位: word) ================
 * 优先级数值越大越高(configMAX_PRIORITIES = 5, 可用 0~4)。
 * DHT22 优先级必须高于 UI: DHT22 的位操作时序在 us 级, 被 UI 抢占会读失败。
 * net(2) 低于 UI(3): net 每拍只做一次判断就让出, 不会饿死显示; 反过来说
 * 一次 AT 事务最长 10s, 优先级高于 UI 会把刷屏挡住。
 * ================================ */
#define UI_TASK_PRIORITY                    3
#define UI_TASK_STACK_SIZE                  1536 // 比其它任务大: LVGL 的按键处理与刷屏都跑在这个任务里

#define UI_BOOT_NET_TIMEOUT_MS              (60000UL) // 开机阶段等 EV_NET_READY 的上限(ms): 见 UI_Task.c 里的说明, 30s 在慢网下不够

#define NET_TASK_PRIORITY                   2
#define NET_TASK_STACK_SIZE                 1024

#define DHT22_TASK_PRIORITY                 4
#define DHT22_TASK_STACK_SIZE               512

#define LIGHT_SENSOR_TASK_PRIORITY          2
#define LIGHT_SENSOR_TASK_STACK_SIZE        512

#define KEY_TASK_PRIORITY                   2
#define KEY_TASK_STACK_SIZE                 1024

/* ================ 周期参数(单位: 秒) ================ */
#define SNTP_PERIOD_S           (4UL * 60UL * 60UL) // SNTP 4h
#define WEATHER_PERIOD_S        (60UL * 60UL)       // 天气 1h
#define WIFI_PERIOD_S           (10UL * 60UL)       // WiFi检查 10min
#define DHT22_PERIOD_S          (5UL * 60UL)        // 房间温湿度 5min
#define RETRY_SNTP_S            (5UL)               // SNTP重试 5s
#define RETRY_WEATHER_S         (60UL)              // 天气重试 60s
#define RETRY_WIFI_S            (10UL)              // WiFi重试 10s

/* ================ 昼夜切换与低功耗 ================ */
#define RTC_LP_CLK_SOURCE        (1)      // 低功耗模式时钟源选择: 1=DS1302, 0=软件时钟
#define DEBOUNCE_MS              (2000UL) // 昼夜切换去抖 2s
#define EV_LP_UI_TICK_MS         (2000UL) // 低功耗UI事件等待(ms), 夜间只显示到分钟
#define UI_TOUCH_POLL_MS         (50UL)   // 白天主界面的触摸轮询节拍(ms): 必须明显小于一次点击的
                                                // 持续时间(约100~200ms), 否则整段点击会落在两次采样之间被漏掉

/* ================ 按键手势参数(由 Key_Task 识别) ================ */
#define KEY_LONG_PRESS_MS          (800UL) // 按住达到该时长即触发长按(不等松开)
#define KEY_MULTI_GAP_MS           (300UL) // 连击窗口: 超时即结算累计击数
#define KEY_CLICK_MAX              (3U)    // 最多识别到三击, 达到即立即结算
#define KEY_RELEASE_POLL_MS        (20UL)  // 长按触发后等待松开的轮询步长

#endif /* __TASK_CONFIG_H__ */
