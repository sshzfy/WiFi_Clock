#ifndef __TASK_COMMON_H__
#define __TASK_COMMON_H__

/* ============================================================
 * 任务层共享底座
 * ------------------------------------------------------------
 * 拆分任务后, 只有两类东西真正被多个任务同时使用, 统一放在这里:
 *   1) 事件组(g_evt)      —— 生产者置位, uiTask 唯一消费者
 *   2) 低功耗标志          —— uiTask 写, netTask/dht22Task 读
 * 其余状态(昼夜模式、句柄、LCD/OLED 开关等)都收进各自任务文件, 不对外暴露。
 * ============================================================ */

#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"

/* ================ UI 事件位(生产者→uiTask 通知) ================ */
#define EV_WEATHER                     (1UL << 0) // 天气已更新, 刷新天气模块
#define EV_DHT22                       (1UL << 1) // 房间温湿度已更新, 刷新房间模块
#define EV_NET_READY                   (1UL << 2) // 开机网络阶段完成(无论成败)
#define EV_WIFI                        (1UL << 3) // WiFi连接状态变化, 刷新顶部状态条
#define EV_LOWERPOWER                  (1UL << 4) // 进入低功耗模式
#define EV_LOWPOWER_ACK                (1UL << 5) // 低功耗模式确认
#define EV_WAKEUP                      (1UL << 6) // 从低功耗模式唤醒
#define EV_NET_UPDATE_NOW              (1UL << 7) // 立即更新网络状态, 包括WiFi连接状态、SNTP时间、天气
#define EV_DHT22_UPDATE_NOW            (1UL << 8) // 立即更新DHT22传感器状态
#define EV_KEYBOARD                    (1UL << 9)  // 点了WiFi图标, 刷WiFi配网界面
#define EV_MAIN_SCREEN                 (1UL << 10) // 刷新主界面
#define EV_WIFI_CRED                   (1UL << 11) // 键盘提交了新的WiFi凭据, 保存并重连
#define EV_LOCATION_KB                 (1UL << 12) // 点了定位图标, 刷定位输入界面
#define EV_LOCATION_CRED               (1UL << 13) // 键盘提交了新的定位信息, 立即按新定位取天气
/* 注: GPS + 高德逆地理编码的省市显示暂未接入(接口保留) */

extern volatile bool g_lowpower;

void Task_Common_Init(void);
EventGroupHandle_t Task_Events(void);

#define APP_IS_LOWPOWER()              (g_lowpower)
#define APP_SET_LOWPOWER(on)           (g_lowpower = (on))

#endif /* __TASK_COMMON_H__ */
