#ifndef __KEY_TASK_H__
#define __KEY_TASK_H__

#include "main.h"
#include "Key.h"
#include "Task_Common.h"
#include "Task_Config.h"
#include "LightSensor_Task.h"

void Key_Task_Start(void);
void Key_Task_OnGesture(Key_Gesture_t gesture);

#endif /* __KEY_TASK_H__ */
