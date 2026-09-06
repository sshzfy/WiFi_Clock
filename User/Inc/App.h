#ifndef __APP_H__
#define __APP_H__

#include "main.h"
#include "AT.h"
#include "Timer.h"
#include "DHT22.h"

extern AT_WiFi_Info_t wifi_info;
extern AT_Date_Info_t date_info;
extern AT_Weather_Info_t weather_info;
extern DHT22_Data_t room_info;

void Clock_Sync(AT_Date_Info_t *date_info);
bool Clock_IsSynced(void);
void Clock_GetDateTime(AT_Date_Info_t *time_out);

bool Wireless_Init(void);
bool Service_WiFi_Connect(void);

bool Service_Time_Sync(void);
int Service_WiFi_Update(void);
bool Service_Weather_Update(void);
bool Service_Room_Update(void);

#endif /* __APP_H__ */
