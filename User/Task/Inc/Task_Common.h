#ifndef __TASK_COMMON_H__
#define __TASK_COMMON_H__

#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"

#define APP_IS_LOWPOWER()              (g_lowpower)
#define APP_SET_LOWPOWER(on)           (g_lowpower = (on))

/* ================ UI 事件位(生产者→uiTask 通知) ================ */
#define EV_WEATHER                     (1UL << 0)  // 天气已更新, 刷天气模块
#define EV_DHT22                       (1UL << 1)  // 温湿度已更新, 刷房间模块
#define EV_NET_READY                   (1UL << 2)  // 开机网络阶段完成(配网成功)
#define EV_WIFI                        (1UL << 3)  // WiFi连接状态变化, 刷顶栏状态
#define EV_LOWERPOWER                  (1UL << 4)  // 进入低功耗模式
#define EV_LOWPOWER_ACK                (1UL << 5)  // 低功耗模式确认(uiTask 切换完成后置位)
#define EV_WAKEUP                      (1UL << 6)  // 从低功耗模式唤醒
#define EV_NET_UPDATE_NOW              (1UL << 7)  // 请求网络立即更新(WiFi/SNTP/天气)
#define EV_DHT22_UPDATE_NOW            (1UL << 8)  // 请求立即更新DHT22
#define EV_WIFI_CRED                   (1UL << 9)  // 输入提交了新的WiFi凭证, 保存并联网
#define EV_LOCATION_CRED               (1UL << 10) // 输入提交了新的定位信息, 重新定位取天气
#define EV_OTA_CHECK                   (1UL << 11) // 用户请求"检查固件更新"(uiTask -> netTask)

extern volatile bool g_lowpower;

void Task_Common_Init(void);
EventGroupHandle_t Task_Events(void);

#endif /* __TASK_COMMON_H__ */
