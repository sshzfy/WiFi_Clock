#include "AT.h"
#include "FreeRTOS.h"
#include "task.h"

#define AT_DEBUG 0
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

static char rx_buf[1024];

static const AT_ACK_Match_t AT_ACK_Match[] =
    {
        {AT_ACK_OK, "OK\r\n"},
        {AT_ACK_ERROR, "ERROR\r\n"},
        {AT_ACK_BUSY, "busy p...\r\n"},
        {AT_ACK_READY, "ready\r\n"},
};

static void AT_Usart_Write(const char *data);
static bool AT_Wait_Ready(uint32_t timeout);
static AT_ACK_t AT_Match_Internal_ACK(const char *str);
static bool AT_Wait_Boot(uint32_t timeout);

static void AT_Usart_Write(const char *data)
{
    while (data && *data)
    {
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET)
            ;
        USART_SendData(USART1, *data++);
    }
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET)
        ;
    USART_SendData(USART1, '\r');
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET)
        ;
    USART_SendData(USART1, '\n');
}

static AT_ACK_t AT_Usart_Wait_Receive(uint32_t timeout)
{
    const char *line = rx_buf;
    uint32_t rx_len = 0;
    rx_buf[0] = '\0';

    uint64_t start = TIM5_Get_ms();

    while (rx_len < sizeof(rx_buf) - 1)
    {
        while (Usart1_RX_Count() == 0)
        {
            if (TIM5_Get_ms() - start >= timeout)
                return AT_ACK_NONE;
            if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
                vTaskDelay(1); /* 让出CPU, 字节由中断环形缓冲缓存, 不会丢失 */
        }
        rx_buf[rx_len++] = Usart1_RX_Read();
        rx_buf[rx_len] = '\0';
        if (rx_buf[rx_len - 1] == '\n')
        {
            AT_ACK_t ack = AT_Match_Internal_ACK(line);
            if (ack != AT_ACK_NONE)
                return ack;
            line = rx_buf + rx_len;
        }
    }
    return AT_ACK_NONE;
}

static AT_ACK_t AT_Match_Internal_ACK(const char *str)
{
    for (uint32_t i = 0; i < ARRAY_SIZE(AT_ACK_Match); i++)
    {
        if (strcmp(str, AT_ACK_Match[i].string) == 0)
            return AT_ACK_Match[i].ack;
    }
    return AT_ACK_NONE;
}

static void AT_USART_Init(void)
{
    // 初始化GPIOA
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource9, GPIO_AF_USART1);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource10, GPIO_AF_USART1);

    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_StructInit(&GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Pin = GPIO_Pin_9 | GPIO_Pin_10;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStruct.GPIO_Speed = GPIO_High_Speed;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;

    GPIO_Init(GPIOA, &GPIO_InitStruct);
    // 初始化USART1
    USART_InitTypeDef USART_InitStruct;
    USART_StructInit(&USART_InitStruct);

    USART_InitStruct.USART_BaudRate = 115200;
    USART_InitStruct.USART_WordLength = USART_WordLength_8b;
    USART_InitStruct.USART_StopBits = USART_StopBits_1;
    USART_InitStruct.USART_Parity = USART_Parity_No;
    USART_InitStruct.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_InitStruct.USART_HardwareFlowControl = USART_HardwareFlowControl_None;

    USART_Init(USART1, &USART_InitStruct);
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE); // RXNE中断 → Usart.c环形缓冲
    USART_Cmd(USART1, ENABLE);

    NVIC_InitTypeDef NVIC_InitStruct;
    NVIC_InitStruct.NVIC_IRQChannel = USART1_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 5;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStruct);
}

bool AT_Init(void)
{
    AT_USART_Init();

    if (!AT_Wait_Boot(3000))
        return false;
    if (!AT_Write_Command("AT+RESTORE", 2000))
        return false;
    if (!AT_Wait_Ready(5000))
        return false;
    return true;
}

bool AT_Wait_Ready(uint32_t timeout)
{
    return AT_Usart_Wait_Receive(timeout) == AT_ACK_READY;
}

bool AT_Write_Command(const char *command, uint32_t timeout)
{
#if AT_DEBUG
    printf("[EDBUG] Command: %s\n", command);
#endif
    AT_Usart_Write(command);

    AT_ACK_t ack = AT_Usart_Wait_Receive(timeout);
#if AT_DEBUG
    // printf("[EDBUG] ACK: %d\n", ack);
    printf("[EDBUG] Response: \n%s\n", rx_buf);
#endif

    return ack == AT_ACK_OK;
}

const char *AT_Get_Response(void)
{
    return rx_buf;
}

static bool AT_Wait_Boot(uint32_t timeout)
{
    for (int i = 0; i < timeout; i += 100)
    {
        if (AT_Write_Command("AT", 100))
            return true;
    }
    return false;
}

bool AT_WiFi_Init(void)
{
    return AT_Write_Command("AT+CWMODE=1", 2000);
}

bool AT_Connect_WiFi(const char *ssid, const char *password, const char *mac)
{
    if (ssid == NULL || password == NULL)
        return false;

    char cmd[128];
    if (mac == NULL)
    {
        snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", ssid, password);
    }
    else if (mac != NULL)
    {
        snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\",\"%s\"", ssid, password, mac);
    }
    return AT_Write_Command(cmd, 5000);
}

static bool parse_cwstate_response(const char *response, AT_WiFi_Info_t *info)
{
    // static const char *cwstate_response =
    //     "AT+CWSTATE?\r\n"
    //     "+CWSTATE:2,\"Laptop -S\"\r\n"
    //     "\r\n"
    //     "OK\r\n";
    response = strstr(response, "+CWSTATE:");
    if (response == NULL)
        return false;
    int wifi_state;
    if (sscanf(response, "+CWSTATE:%d,\"%63[^\"]\"", &wifi_state, info->ssid) != 2)
        return false;
    info->connected = (wifi_state == 2);
    return true;
}

static bool parse_cwjap_response(const char *response, AT_WiFi_Info_t *info)
{
    // static const char *cwjap_response =
    //     "AT+CWJAP?\r\n"
    //     "+CWJAP:\"Laptop-S\",\"7e:b5:66:b3:56:55\",1,-59,0,1,3,0,1\r\n"
    //     "\r\n"
    //     "OK\r\n";
    response = strstr(response, "+CWJAP:");
    if (response == NULL)
        return false;
    if (sscanf(response, "+CWJAP:\"%63[^\"]\",\"%17[^\"]\",%d,%d", info->ssid, info->bssid, &info->channel, &info->rssi) != 4)
        return false;
    return true;
}

bool AT_Get_WiFi_Info(AT_WiFi_Info_t *info)
{
    if (info == NULL)
        return false;

    if (!AT_Write_Command("AT+CWSTATE?", 2000))
        return false;
    if (!parse_cwstate_response(AT_Get_Response(), info))
        return false;

    if (!AT_Write_Command("AT+CWJAP?", 2000))
        return false;
    if (!parse_cwjap_response(AT_Get_Response(), info))
        return false;
    return true;
}

bool AT_Is_WiFi_Conected(void)
{
    AT_WiFi_Info_t info;

    if (!AT_Get_WiFi_Info(&info))
        return false;
    return info.connected;
}

bool AT_SNTP_Init(void)
{
    if (!AT_Write_Command("AT+CIPSNTPCFG=1,8", 2000))
        return false;
    return true;
}

static uint8_t month_str_to_num(const char *month_str)
{
    const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (uint8_t i = 0; i < 12; i++)
    {
        if (strcmp(month_str, months[i]) == 0)
            return i + 1;
    }
    return 0;
}

static uint8_t weekday_str_to_num(const char *weekday_str)
{
    const char *weekdays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    for (uint8_t i = 0; i < 7; i++)
    {
        if (strcmp(weekday_str, weekdays[i]) == 0)
            return i + 1;
    }
    return 0;
}

static bool parse_cipsntptime_response(const char *response, AT_Date_Info_t *date_info)
{
    // static const char *cipsntptime =
    //     "AT+CIPSNTPTIME?\r\n"
    //     "+CIPSNTPTIME:Tue Oct 19 17:47:56 2021\r\n"
    //     "OK\r\n";
    response = strstr(response, "+CIPSNTPTIME:");
    if (response == NULL)
        return false;
    char weekday_str[8];
    char month_str[4];
    if (sscanf(response, "+CIPSNTPTIME:%3s %3s %hhu %hhu:%hhu:%hhu %hu",
               weekday_str, month_str, &date_info->day, &date_info->hour, &date_info->minute, &date_info->second, &date_info->year) != 7)
        return false;
    date_info->weekday = weekday_str_to_num(weekday_str);
    date_info->month = month_str_to_num(month_str);

    return true;
}

bool AT_SNTP_Get_Time(AT_Date_Info_t *date_info)
{
    if (!AT_Write_Command("AT+CIPSNTPTIME?", 2000))
        return false;
    if (!parse_cipsntptime_response(AT_Get_Response(), date_info))
        return false;
    return true;
}

bool parse_weather_response(const char *response, AT_Weather_Info_t *info)
{
    // static const char *http_response =
    //     "AT+HTTPCLIENT=2,1,\"https://api.seniverse.com/v3/weather/now.json?key=SgM2NZE2Sghy4FOFh&location=dalian&language=en&unit=c\",,,2\r\n"
    //     "+HTTPCLIENT:267,{\"results\":[{\"location\":{\"id\":\"WWYMRT0VRMUG\",\"name\":\"Dalian\",\"country\":\"CN\",\"path\":\"Dalian,Dalian,Liaoning,China\",\"timezone\":\"Asia/Shanghai\",\"timezone_offset\":\"+08:00\"},\"now\":{\"text\":\"Cloudy\",\"code\":\"4\",\"temperature\":\"29\"},\"last_update\":\"2026-08-16T16:45:45+08:00\"}]}\r\n"
    //     "\r\n"
    //     "OK\r\n";
    response = strstr(response, "\"results\":");
    if (response == NULL)
        return false;
    const char *response_location = strstr(response, "\"location\":");
    if (response_location == NULL)
        return false;
    const char *response_location_name = strstr(response_location, "\"name\":");
    if (response_location_name)
        sscanf(response_location_name, "\"name\":\"%31[^\"]\"", info->city);
    const char *response_location_path = strstr(response_location, "\"path\":");
    if (response_location_path)
        sscanf(response_location_path, "\"path\":\"%127[^\"]\"", info->location);

    const char *response_now = strstr(response, "\"now\":");
    if (response_now == NULL)
        return false;
    const char *response_now_text = strstr(response_now, "\"text\":");
    if (response_now_text)
        sscanf(response_now_text, "\"text\":\"%15[^\"]\"", info->weather);
    const char *response_now_code = strstr(response_now, "\"code\":");
    if (response_now_code)
    {
        char code_str[8] = {0};
        if (sscanf(response_now_code, "\"code\":\"%7[^\"]\"", code_str) == 1)
            info->weather_code = atoi(code_str);
    }
    const char *response_now_temperature = strstr(response_now, "\"temperature\":");
    if (response_now_temperature)
    {
        char temperature_str[16] = {0};
        if (sscanf(response_now_temperature, "\"temperature\":\"%15[^\"]\"", temperature_str) == 1)
            info->temperature = atof(temperature_str);
    }
    return true;
}

const char *AT_Get_HTTP(const char *url)
{
    static char tx_buf[256];
    snprintf(tx_buf, sizeof(tx_buf), "AT+HTTPCLIENT=2,1,\"%s\",,,2", url);
    bool ret = AT_Write_Command(tx_buf, 5000);
    if (ret)
    {
        const char *response = AT_Get_Response();
        return response;
    }
    return NULL;
}
