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
#define EV_WEATHER                     (1UL << 0) // 天气已更新, 请刷新天气模块
#define EV_DHT22                       (1UL << 1) // 房间温湿度已更新, 请刷新房间模块
#define EV_NET_READY                   (1UL << 2) // 开机网络阶段完成(无论成败)
#define EV_WIFI                        (1UL << 3) // WiFi连接状态变化, 请刷新顶部状态条
#define EV_LOWERPOWER                  (1UL << 4) // 进入低功耗模式
#define EV_LOWPOWER_ACK                (1UL << 5) // 低功耗模式确认
#define EV_WAKEUP                      (1UL << 6) // 从低功耗模式唤醒
#define EV_NET_UPDATE_NOW              (1UL << 7) // 立即更新网络状态, 包括WiFi连接状态、SNTP时间、天气
#define EV_DHT22_UPDATE_NOW            (1UL << 8) // 立即更新DHT22传感器状态
/* 注: GPS + 高德逆地理编码的省市显示暂未接入(接口保留, 见 4.13) */
/* (1UL << 9) 预留给将来的 EV_LOCATION */

/* ================ 事件组 ================ */

void Task_Common_Init(void);
EventGroupHandle_t Task_Events(void);

/* ================ 低功耗标志 ================
 * 白天=false, 夜间进入低功耗=true; 置位/清零会联动网络与DHT22的补更请求。
 * 保持 volatile 语义: 原实现即为无锁的 volatile bool, 这里不做加锁以免改变行为。
 */
extern volatile bool g_lowpower;

#define APP_IS_LOWPOWER()              (g_lowpower)
#define APP_SET_LOWPOWER(on)           (g_lowpower = (on))

#endif /* __TASK_COMMON_H__ */
