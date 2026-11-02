#ifndef __SECRETS_H__
#define __SECRETS_H__

/* WiFi: ESP32-C3 只支持 2.4GHz; 密码填 "" 表示开放网络 */
#define WIFI_SSID            "SSID"
#define WIFI_PASSWORD        "PASSWORD"
#define WIFI_MAC             NULL
#define WEATHER_KEY          "KEY" // 心知天气私钥

/* OTA 固件仓库(Gitee): 仅用于拼接下载地址 */
#define OTA_GITEE_OWNER        "owner"
#define OTA_GITEE_REPO         "repo"

#endif /* __SECRETS_H__ */