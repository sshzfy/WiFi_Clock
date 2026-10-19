#ifndef __UI_NIGHT_H__
#define __UI_NIGHT_H__

#include "main.h"
#include "BuildConfig.h"
#include "App.h"
#include "OLED.h"
#include "External_RTC.h"
#include "Task_Config.h"

void UI_Night_Enter(void);
void UI_Night_Leave(void);
void UI_Night_Tick(void);

#endif /* __UI_NIGHT_H__ */
