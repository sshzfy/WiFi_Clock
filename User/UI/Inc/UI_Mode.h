#ifndef __UI_MODE_H__
#define __UI_MODE_H__

#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"

void UI_Mode_Init(void);
void UI_Mode_SwitchToNight(void);
void UI_Mode_SwitchToDay(void);
bool UI_Mode_IsNight(void);
TickType_t UI_Mode_WaitTicks(void);
void UI_Mode_HandleEvent(EventBits_t bits);
void UI_Mode_Tick(void);

#endif /* __UI_MODE_H__ */
