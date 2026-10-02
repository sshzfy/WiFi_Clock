#ifndef __AT_H__
#define __AT_H__

#include "main.h"
#include "stm32f4xx.h"
#include "Timer.h"
#include "FreeRTOS.h"
#include "task.h"
#include "Usart.h"

#define AT_USART1_GPIO_PORT GPIOA         // USART1_TX和USART1_RX连接在GPIOA上
#define AT_USART1_GPIO_PIN_TX GPIO_Pin_9  // USART1_TX
#define AT_USART1_GPIO_PIN_RX GPIO_Pin_10 // USART1_RX

typedef enum
{
    AT_ACK_NONE,  // 本行无回复
    AT_ACK_OK,    // 成功
    AT_ACK_ERROR, // 错误
    AT_ACK_BUSY,  // 繁忙
    AT_ACK_READY, // 就绪
} AT_ACK_t;

typedef struct
{
    AT_ACK_t ack;
    const char *string;
} AT_ACK_Match_t;

/* ==================== AT 模组解析出的数据结构 ====================
 * 内容全部由 AT.c 的解析函数填充, 应用层(App.c)只做整体拷贝后上屏,
 * 所以定义跟 AT 驱动放在一起, 由 AT.h 统一提供给上层。
 * 字段长度必须与 AT.c 里的 sscanf 宽度说明符匹配(见每行注释),
 * 宽度不够会写溢出。
 * ================================================================ */

/* AT+CWSTATE? / AT+CWJAP? —— WiFi 连接信息 */
typedef struct
{
    char ssid[64];  // SSID, 网络名称  (sscanf %63)
    char bssid[18]; // BSSID, MAC地址 (sscanf %17)
    int channel;    // 信道
    int rssi;       // 信号强度(dBm)
    bool connected; // 是否已连接
} AT_WiFi_Info_t;

/* HTTP 天气接口 —— 城市 / 天气 / 温度 */
typedef struct
{
    char city[32];      // 城市                    (sscanf %31)
    char location[128]; // 位置路径(城市,省份,国家)  (sscanf %127)
    char weather[32];   // 天气描述                (sscanf %31)
    int weather_code;   // 天气码
    float temperature;  // 温度(摄氏度)
} AT_Weather_Info_t;

/* AT+CIPSNTPTIME? —— UTC 时间 */
typedef struct
{
    uint16_t year;   // 年份
    uint8_t month;   // 月份 1~12
    uint8_t day;     // 日期 1~31
    uint8_t hour;    // 小时
    uint8_t minute;  // 分钟
    uint8_t second;  // 秒
    uint8_t weekday; // 星期 1=周一 ... 7=周日
} AT_Date_Info_t;

/* 高德逆地理编码 —— 省 / 市(UTF-8 原文) */
typedef struct
{
    char city[32];     // 城市 (sscanf %31)
    char province[32]; // 省份 (sscanf %31)
} AT_Location_Info_t;

bool AT_WiFi_Init(void);
bool AT_Init(void);
bool AT_Factory_Reset(void);
bool AT_Wait_Ready(uint32_t timeout);
bool AT_Send_Command(const char *command, uint32_t timeout);
const char *AT_Get_Response(void);
bool AT_Connect_WiFi(const char *ssid, const char *password, const char *mac);
bool AT_Get_WiFi_Info(AT_WiFi_Info_t *info);
bool AT_Set_Sleep(uint8_t mode);
bool AT_Is_WiFi_Conected(void);
bool Parse_Http_Response(const char *response, AT_Weather_Info_t *info);
bool AT_SNTP_Init(void);
bool AT_SNTP_Get_Time(AT_Date_Info_t *date_info);
const char *AT_Get_HTTP(const char *url);
bool AT_Get_IP(char *ip);
bool Parse_Http_IP_Response(const char *response, char *ip);
bool AT_Get_Location(float latitude, float longitude, AT_Location_Info_t *info);
bool Parse_Http_Location_Response(const char *response, AT_Location_Info_t *info);

#endif /* __AT_H__ */
