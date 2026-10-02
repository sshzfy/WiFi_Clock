#ifndef __APP_H__
#define __APP_H__

#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "AT.h"
#include "DHT22.h"
#include "External_RTC.h"

extern AT_WiFi_Info_t wifi_info;
extern AT_Date_Info_t date_info;
extern AT_Weather_Info_t weather_info;
extern AT_Location_Info_t location_info; // 逆地理编码省/市(UTF-8原文); 当前应用未接入, 见 Service_Location_Update
extern DHT22_Data_t room_info;

/* 上面这些结构体的定义在 AT.h —— 它们是 AT 驱动解析出来的内容,
 * 由 AT.h 统一提供(见该文件"AT 模组解析出的数据结构"), 这里不再重复定义。 */

void Clock_Sync(AT_Date_Info_t *date_info);
bool Clock_IsSynced(void);
void Clock_GetDateTime(AT_Date_Info_t *time_out);

bool Wireless_Init(void);
bool Service_WiFi_Connect(void);
bool Service_WiFi_Sleep(bool enable);

bool Service_Time_Sync(void);
int Service_WiFi_Update(void);
bool Service_Weather_Update(void);
bool Service_Location_Update(float latitude, float longitude);
bool Service_Room_Update(void);

#endif /* __APP_H__ */
