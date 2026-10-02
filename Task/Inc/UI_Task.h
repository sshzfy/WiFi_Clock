#ifndef __UI_TASK_H__
#define __UI_TASK_H__

#include "main.h"
#include "Task_Common.h"
#include "Task_Config.h"
#include "DHT22_Task.h"
#include "Net_Task.h"
#include "LightSensor_Task.h"
#include "Key_Task.h"
#include "Board.h"
#include "Page.h"
#include "App.h"
#include "LCD.h"
#include "OLED.h"
#include "External_RTC.h"
#include "Asset.h"
#include "Profiling.h"

void UI_Task_Start(void);
void UI_Task_EnterNight(void);
void UI_Task_EnterDay(void);
bool UI_Task_IsLowPower(void);

#endif /* __UI_TASK_H__ */
