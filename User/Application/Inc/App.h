#ifndef __APP_H__
#define __APP_H__

#include "main.h"
#include "BuildConfig.h"
#include "FreeRTOS.h"
#include "task.h"
#include "AT.h"
#include "DHT22.h"
#include "External_RTC.h"
#include "Asset.h"
#include "Secrets.h"

#define WIFI_SSID_MAX_LEN        32U // SSID 最大长度
#define WIFI_PASS_MAX_LEN        64U // 密码最大长度

#define LOCATION_MAX_LEN        32U // 定位信息最大长度

/* ================ 天气接口参数 ================ */
#define WEATHER_URL_FMT            "https://api.seniverse.com/v3/weather/now.json?key=" WEATHER_KEY "&location=%s&language=en&unit=c" // 天气接口URL格式
#define WEATHER_IP_LOCATION        "ip" // 网络IP定位的兜底取值: 由服务端按请求来源IP定位
#define WEATHER_URL_SIZE           256  // URL缓冲区: 省市全中文编码后约190字节

/* ================ WiFi 凭据(键盘输入 → 掉电保存) ================ */
#define WIFI_CFG_NAME           "wifi_cfg"   // 资源层会拼成 /data/wifi_cfg.bin
#define WIFI_CFG_MAGIC          0x57494649UL // 'WIFI': 判断文件内容是不是本模块写的
#define WIFI_CFG_VERSION        1U           // 布局版本: 结构一改就 +1, 旧数据自动作废

/* ================ 定位信息(定位界面输入 → 掉电保存) ================ */
#define LOCATION_CFG_NAME           "location"   // 资源层会拼成 /data/location.bin
#define LOCATION_CFG_MAGIC          0x4C4F4321UL // 'LOC!': 判断文件内容是不是本模块写的
#define LOCATION_CFG_VERSION        1U           // 布局版本: 结构一改就 +1, 旧数据自动作废

/* ================ 无线初始化参数 ================ */
#define WIRELESS_INIT_RETRY         3U    // 最大尝试次数
#define WIRELESS_INIT_GAP_MS        200U  // 两次尝试之间的等待时间(ms)
#define WIFI_BOOT_WAIT_MS           8000U // 没有保存凭据时,最大等待时间(ms)
#define WIFI_SLEEP_MODE_IDLE        1U    // 空闲(夜间无网络活动)档位
#define WIFI_SLEEP_MODE_FULL        0U    // 全速(白天需要快速响应)档位

/* WiFi 凭据结构体 */
typedef struct
{
    uint32_t magic;   // WIFI_CFG_MAGIC
    uint16_t version; // WIFI_CFG_VERSION
    uint16_t size;    // 本结构体字节数
    char ssid[WIFI_SSID_MAX_LEN + 1U];
    char password[WIFI_PASS_MAX_LEN + 1U];
} WiFi_Cfg_t;

/* 定位信息结构体 */
typedef struct
{
    uint32_t magic;   // LOCATION_CFG_MAGIC
    uint16_t version; // LOCATION_CFG_VERSION
    uint16_t size;    // 本结构体字节数
    char text[LOCATION_MAX_LEN + 1U];
} Location_Cfg_t;

extern char g_ssid[WIFI_SSID_MAX_LEN + 1U];
extern char g_password[WIFI_PASS_MAX_LEN + 1U];
extern char g_location[LOCATION_MAX_LEN + 1U];
extern AT_WiFi_Info_t wifi_info;
extern AT_Date_Info_t date_info;
extern AT_Weather_Info_t weather_info;
// extern AT_Location_Info_t location_info; // 逆地理编码省/市(UTF-8原文); 当前应用未接入, 见 Service_Location_Update
extern DHT22_Data_t room_info;

void Clock_Sync(AT_Date_Info_t *date_info);
bool Clock_IsSynced(void);
void Clock_GetDateTime(AT_Date_Info_t *time_out);

bool WiFi_Config_IsSet(void);
bool WiFi_Config_IsDirty(void);
void WiFi_Config_Set(const char *ssid, const char *password);
bool WiFi_Config_Save(void);
void WiFi_Config_Load(void);

bool Location_Config_IsSet(void);
bool Location_Config_IsDirty(void);
void Location_Config_Set(const char *location);
bool Location_Config_Save(void);
void Location_Config_Load(void);

bool Wireless_Init(void);
bool Service_WiFi_Connect(void);
bool Service_WiFi_Sleep(bool enable);

bool Service_Time_Sync(void);
int Service_WiFi_Update(void);
bool Service_Weather_Update(void);
bool Service_Location_Update(float latitude, float longitude);
bool Service_Room_Update(void);

#endif /* __APP_H__ */
