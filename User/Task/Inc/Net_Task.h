#ifndef __NET_TASK_H__
#define __NET_TASK_H__

#include "main.h"
#include "BuildConfig.h"
#include "Task_Common.h"
#include "Task_Config.h"
#include "App.h"
#include "AT.h"

typedef struct
{
    volatile bool wifi_ok;    // 开机WiFi初始化+连接是否完成(成功或失败)
    volatile bool service_ok; // 开机SNTP+天气是否都成功
} NetBoot_t;

void Net_Task_Start(void);
void Net_Task_GetBoot(bool *wifi_ok, bool *service_ok);

#endif /* __NET_TASK_H__ */
