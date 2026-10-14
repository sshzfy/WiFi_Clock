#ifndef __APP_H__
#define __APP_H__

#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "AT.h"
#include "DHT22.h"
#include "External_RTC.h"
#include "Secrets.h" 

/* WiFi 凭据: 由键盘界面输入, 保存到 W25Q64 的 /data/wifi_cfg.bin。
 * 长度上限按 ESP32-C3 AT 固件的限制取(SSID 32 字节、密码 64 字节), 多留 1 字节放结尾 '\0'。 */
#define WIFI_SSID_MAX_LEN 32U
#define WIFI_PASS_MAX_LEN 64U

extern char g_ssid[WIFI_SSID_MAX_LEN + 1U];
extern char g_password[WIFI_PASS_MAX_LEN + 1U];
extern AT_WiFi_Info_t wifi_info;
extern AT_Date_Info_t date_info;
extern AT_Weather_Info_t weather_info;
extern AT_Location_Info_t location_info; // 逆地理编码省/市(UTF-8原文); 当前应用未接入, 见 Service_Location_Update
extern DHT22_Data_t room_info;

void Clock_Sync(AT_Date_Info_t *date_info);
bool Clock_IsSynced(void);
void Clock_GetDateTime(AT_Date_Info_t *time_out);

/* WiFi 凭据的存取(键盘界面 → RAM → 外部 Flash)。
 * 注意: 这几个函数都要在 uiTask 里调用 —— Asset 层(littlefs)没有跨任务保护,
 * 全工程只有 uiTask 访问它(渲染字库/图片、触摸校准), 别的任务插进来会写坏文件系统。 */
bool WiFi_Config_IsSet(void);
bool WiFi_Config_IsDirty(void);
void WiFi_Config_Set(const char *ssid, const char *password);
bool WiFi_Config_Save(void);
void WiFi_Config_Load(void);

bool Wireless_Init(void);
bool Service_WiFi_Connect(void);
bool Service_WiFi_Sleep(bool enable);

bool Service_Time_Sync(void);
int Service_WiFi_Update(void);
bool Service_Weather_Update(void);
bool Service_Location_Update(float latitude, float longitude);
bool Service_Room_Update(void);

#endif /* __APP_H__ */
