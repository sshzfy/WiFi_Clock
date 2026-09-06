#ifndef __APP_TASK_H__
#define __APP_TASK_H__

#include "FreeRTOS.h"
#include "task.h"

/* ================ UI 事件位(生产者→uiTask 通知) ================ */
#define EV_WEATHER   (1UL << 0) /* 天气已更新, 请刷新天气模块 */
#define EV_ROOM      (1UL << 1) /* 房间温湿度已更新, 请刷新房间模块 */
#define EV_NET_READY (1UL << 2) /* 开机网络阶段完成(无论成败) */
#define EV_WIFI      (1UL << 3) /* WiFi连接状态变化, 请刷新顶部状态条 */

/* 创建UI任务(UI任务内部再创建 netTask/sensorTask), 由 main 调用 */
void App_Task_Init(void);

#endif /* __APP_TASK_H__ */
