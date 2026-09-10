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

typedef struct
{
    char ssid[64];  // SSID,网络名称
    char bssid[18]; // BSSID,mac地址
    int channel;    // 信道
    int rssi;       // 信号强度
    bool connected; // 是否连接
} AT_WiFi_Info_t;

typedef struct
{
    char city[32];      // 城市
    char location[128]; // 位置
    char weather[16];   // 天气
    int weather_code;   // 天气码
    float temperature;  // 温度
} AT_Weather_Info_t;

typedef struct
{
    uint16_t year;   // 年份
    uint8_t month;   // 月份
    uint8_t day;     // 日期
    uint8_t hour;    // 小时
    uint8_t minute;  // 分钟
    uint8_t second;  // 秒
    uint8_t weekday; // 星期几
} AT_Date_Info_t;

bool AT_WiFi_Init(void);
bool AT_Init(void);
bool AT_Wait_Ready(uint32_t timeout);
bool AT_Write_Command(const char *command, uint32_t timeout);
const char *AT_Get_Response(void);
bool AT_WiFi_Init(void);
bool AT_Connect_WiFi(const char *ssid, const char *password, const char *mac);
bool AT_Get_WiFi_Info(AT_WiFi_Info_t *info);
bool AT_Is_WiFi_Conected(void);
bool Parse_Weather_Response(const char *response, AT_Weather_Info_t *info);
bool AT_SNTP_Init(void);
bool AT_SNTP_Get_Time(AT_Date_Info_t *date_info);
const char *AT_Get_HTTP(const char *url);

#endif /* __AT_H__ */
