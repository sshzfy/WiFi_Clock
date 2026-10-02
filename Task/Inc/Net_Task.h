#ifndef __NET_TASK_H__
#define __NET_TASK_H__

#include "main.h"
#include "Task_Common.h"
#include "Task_Config.h"
#include "App.h"
#include "AT.h"

/* ================ 开机网络阶段结果 ================
 * 由 netTask 在 EV_NET_READY 置位前写好, 之后只读。
 * 原先定义在 app_task.h 中, 现收进本模块, 避免污染全局命名空间。
 */
typedef struct
{
    volatile bool wifi_ok;    // 开机WiFi初始化+连接是否完成(成功或失败)
    volatile bool service_ok; // 开机SNTP+天气是否都成功
} NetBoot_t;

void Net_Task_Start(void);
void Net_Task_GetBoot(bool *wifi_ok, bool *service_ok);

#endif /* __NET_TASK_H__ */
