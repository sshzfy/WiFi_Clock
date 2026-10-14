#ifndef __LIGHT_SENSOR_TASK_H__
#define __LIGHT_SENSOR_TASK_H__

#include "main.h"
#include "Task_Common.h"
#include "Task_Config.h"
#include "Light_Sensor.h"
#include "UI_Task.h"

void LightSensor_Task_Start(void);
void LightSensor_Task_RequestToggleNight(void);

#endif /* __LIGHT_SENSOR_TASK_H__ */
