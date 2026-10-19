#ifndef __UI_TASK_H__
#define __UI_TASK_H__

#include "main.h"
#include "App.h"
#include "Asset.h"
#include "Board.h"
#include "Page.h"
#include "Task_Common.h"
#include "Task_Config.h"
#include "DHT22_Task.h"
#include "Net_Task.h"
#include "LightSensor_Task.h"
#include "Key_Task.h"
#include "Task_Monitor.h"
#include "UI_Mode.h"
#include "UI_Input.h"

/* 事件组中 uiTask 关心的位: 其余位(如 ACK、更新请求)只在别的任务之间传递 */
#define UI_TASK_EVENTS        (EV_WEATHER | EV_DHT22 | EV_WIFI | EV_LOWERPOWER | EV_WAKEUP | \
                               EV_WIFI_KB | EV_LOCATION_KB | EV_MAIN_SCREEN)

void UI_Task_Start(void);

#endif /* __UI_TASK_H__ */
