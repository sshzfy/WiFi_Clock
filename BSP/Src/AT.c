#include "AT.h"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0])) /* 数组元素个数 */

static char rx_buf[1024]; // 接收缓冲区

static const AT_ACK_Match_t AT_ACK_Match[] =
    {
        {AT_ACK_OK,    "OK\r\n"},
        {AT_ACK_ERROR, "ERROR\r\n"},
        {AT_ACK_BUSY,  "busy p...\r\n"},
        {AT_ACK_READY, "ready\r\n"},
};

static void AT_Usart_Send(const char *data);
static AT_ACK_t AT_Match_Internal_ACK(const char *str);
static bool AT_Wait_Boot(uint32_t timeout);
static bool Parse_CWSTATE_Response(const char *response, AT_WiFi_Info_t *info);
static bool Parse_CWJAP_Response(const char *response, AT_WiFi_Info_t *info);
static bool Parse_CIPSNTPTIME_Response(const char *response, AT_Date_Info_t *date_info);

/* ==================== AT 初始化相关函数 ==================== */

static void AT_GPIO_Init(void)
{
    /* 初始化GPIOA */
    GPIO_PinAFConfig(AT_USART1_GPIO_PORT, GPIO_PinSource9, GPIO_AF_USART1);
    GPIO_PinAFConfig(AT_USART1_GPIO_PORT, GPIO_PinSource10, GPIO_AF_USART1);

    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_StructInit(&GPIO_InitStruct);
    GPIO_InitStruct.GPIO_Pin = AT_USART1_GPIO_PIN_TX | AT_USART1_GPIO_PIN_RX;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_AF; // 复用模式
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP; // 推挽输出
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;   // 上拉
    GPIO_Init(AT_USART1_GPIO_PORT, &GPIO_InitStruct);
}

static void AT_USART_Init(void)
{
    USART1_Init();
}

static void AT_NVIC_Init(void)
{
    /* 初始化NVIC */
    NVIC_InitTypeDef NVIC_InitStruct;
    NVIC_InitStruct.NVIC_IRQChannel = USART1_IRQn;         // 使能USART1_IRQn
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 5; // 抢占优先级
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 0;        // 子优先级
    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStruct);
}

/* ==================== AT底层通信相关函数 ==================== */

bool AT_Init(void)
{
    /* 外设初始化 */
    AT_GPIO_Init();
    AT_USART_Init();
    AT_NVIC_Init();

    /* 等模组能应答 "AT": 拿到 OK 就说明它已经启动完成、可以接收命令了。
     * 这里**不再下发 AT+RESTORE**: 恢复出厂会擦掉模组保存的配置并强制重启,
     * 而那次重启正是"模组还没准备好, 先回 busy p... / ready"这类偶发失败的来源。
     * 需要恢复出厂时, 由上层在异常路径上调用 AT_Factory_Reset()。 */
    if (!AT_Wait_Boot(3000))
        return false;
        
    return true;
}

/**
 * @brief 恢复模组出厂设置, 并等它重启完成
 * @return true 成功
 * @note  AT+RESTORE 会擦掉模组内保存的配置(Wi-Fi 模式、已保存的 AP 等)并**强制重启**。
 *        重启期间模组先吐的是 ready / busy p..., 而不是 OK —— 这正是 AT_Init()
 *        偶发失败的来源(见 README 11.11)。因此它不再出现在每次开机的必经路径上,
 *        只作为"模组不应答或配置疑似损坏"时的恢复手段, 由上层按需调用。
 */
bool AT_Factory_Reset(void)
{
    if (!AT_Send_Command("AT+RESTORE", 2000))
        return false;

    return AT_Wait_Ready(5000); /* 重启完成前不能发别的命令 */
}

/**
 * @brief 通过Usart向ESP32发送AT指令
 * @param data 指令字符串
 */
static void AT_Usart_Send(const char *data)
{
    while (data && *data)
    {
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) /* 等待发送缓冲区为空 */
            ;
        USART_SendData(USART1, *data++);                            /* 发送字符 */
        while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET) /* 等待发送完成 */
            ;
    }

    /* 补充AT命令末尾的\r\n（AT命令格式：AT+命令\r\n） */
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET)
        ;
    USART_SendData(USART1, '\r');
    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET) /* 等待发送完成 */
        ;
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET)
        ;
    USART_SendData(USART1, '\n');
    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET) /* 等待发送完成 */
        ;
}

/**
 * @brief 匹配AT指令回复
 * @param str 回复字符串
 * @return AT_ACK_t 回复结果
 */
static AT_ACK_t AT_Match_Internal_ACK(const char *str)
{
    /* 遍历AT_ACK_Match数组, 匹配回复字符串 */
    for (uint32_t i = 0; i < ARRAY_SIZE(AT_ACK_Match); i++)
    {
        if (strcmp(str, AT_ACK_Match[i].string) == 0)
            return AT_ACK_Match[i].ack;
    }

    return AT_ACK_NONE;
}

/**
 * @brief 通过Usart等待接收AT指令回复
 * @param timeout 超时时间, 单位ms
 * @return AT_ACK_t 回复结果
 */
static AT_ACK_t AT_Usart_Wait_Receive(uint32_t timeout)
{
    const char *line = rx_buf; // 指向当前行的指针
    uint32_t rx_len = 0;
    rx_buf[0] = '\0';

    uint64_t start = TIM5_Get_ms();

    while (rx_len < sizeof(rx_buf) - 1)
    {
        /* 等待接收数据 */
        while (Usart1_RX_Count() == 0)
        {
            if (TIM5_Get_ms() - start >= timeout)
                return AT_ACK_NONE;
            if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
                vTaskDelay(1); /* 让出CPU, 字节由中断环形缓冲缓存, 不会丢失 */
        }

        /* 读取接收数据 */
        int ch = Usart1_RX_Read();

        if (ch < 0)
            continue;

        rx_buf[rx_len++] = (char)ch;
        rx_buf[rx_len] = '\0';

        if (rx_buf[rx_len - 1] == '\n')
        {
            AT_ACK_t ack = AT_Match_Internal_ACK(line); // 匹配回复字符串

            if (ack != AT_ACK_NONE)
                return ack;

            line = rx_buf + rx_len; // 指向下一行(每行以'\n'划分)
        }
    }

    return AT_ACK_NONE;
}

/**
 * @brief 等待AT模块引导完成
 * @param timeout 超时时间, 单位ms
 * @return true 成功,false 超时
 */
static bool AT_Wait_Boot(uint32_t timeout)
{
    for (int i = 0; i < timeout; i += 100)
    {
        if (AT_Send_Command("AT", 100))
            return true;
    }
    
    return false;
}

/**
 * @brief 发送AT指令并等待 "OK"
 * @param command 指令字符串
 * @param timeout 超时时间, 单位ms(含重发等待)
 * @return true 成功,false 失败
 *
 * @note  收到 "busy p..." 说明模组暂时拒绝该命令(常见于刚上电/重启后
 *        Wi-Fi协议栈还在初始化), 只要总时间还没用完就重发一次;
 *        其它 ACK(ERROR / ready / 超时)一律判失败, 不重发。
 *        发送前先清空环形缓冲, 避免上一次命令的残留被当成本次回复。
 */
bool AT_Send_Command(const char *command, uint32_t timeout)
{
    uint64_t start = TIM5_Get_ms();

    for (;;)
    {
        Usart1_RX_Flush(); /* 清场: 丢弃上一条命令的尾巴/启动日志/URC */

        /* 发送AT指令 */
        AT_Usart_Send(command);

        uint32_t used = (uint32_t)(TIM5_Get_ms() - start);
        if (used >= timeout)
            return false;

        /* 等待AT模块回复 */
        AT_ACK_t ack = AT_Usart_Wait_Receive(timeout - used);

        if (ack == AT_ACK_OK)
            return true;

        if (ack != AT_ACK_BUSY)
            return false;

        if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
            vTaskDelay(pdMS_TO_TICKS(100)); /* 忙时不急着重发, 等模组退出忙状态 */
    }
}

/**
 * @brief 等待模组输出 "ready"
 * @param timeout 超时时间, 单位ms
 * @return true 收到 ready
 *
 * @note  AT_Usart_Wait_Receive() 匹配到**任意**已知 ACK 就会返回, 直接拿它
 *        当"等 ready"用是错的: 模组重启途中先冒出的 OK / ERROR / busy 会让
 *        本函数立刻失败(实测 AT_Init() 因此只花约100ms就报失败, 而真正的超时
 *        应该是秒级)。这里显式忽略这些过渡 ACK, 一直等到 ready 或总超时。
 */
bool AT_Wait_Ready(uint32_t timeout)
{
    uint64_t start = TIM5_Get_ms();

    while ((uint32_t)(TIM5_Get_ms() - start) < timeout)
    {
        uint32_t remain = timeout - (uint32_t)(TIM5_Get_ms() - start);

        AT_ACK_t ack = AT_Usart_Wait_Receive(remain);

        if (ack == AT_ACK_READY)
            return true;
        if (ack == AT_ACK_NONE)
            return false; /* 剩余时间已耗尽 */
    }

    return false;
}

/**
 * @brief 获取AT指令回复
 * @return char* 回复字符串
 * @note 回复字符串会覆盖在rx_buf中, 不建议直接打印
 */
const char *AT_Get_Response(void)
{
    return rx_buf;
}

/* ==================== WiFi相关底层函数 ==================== */

bool AT_WiFi_Init(void)
{
    return AT_Send_Command("AT+CWMODE=1", 2000);
}

/**
 * @brief 获取WiFi信息
 * @param info WiFi信息结构体指针
 * @return true 成功,false 失败
 */
bool AT_Get_WiFi_Info(AT_WiFi_Info_t *info)
{
    if (info == NULL)
        return false;

    memset(info, 0, sizeof(*info)); /* 清零, 失败时不残留未初始化/旧值 */

    /* CWSTATE 提供 connected + ssid, 为必需项 */
    if (!AT_Send_Command("AT+CWSTATE?", 2000))
        return false;
    if (!Parse_CWSTATE_Response(AT_Get_Response(), info))
        return false;

    /* CWJAP 提供 bssid/channel/rssi, 不因它失败而使整体失败 */
    if (AT_Send_Command("AT+CWJAP?", 2000))
        Parse_CWJAP_Response(AT_Get_Response(), info);

    return true;
}

bool AT_Connect_WiFi(const char *ssid, const char *password, const char *mac)
{
    if (ssid == NULL || password == NULL)
        return false;

    char cmd[128];

    if (mac == NULL)
        snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", ssid, password);
    else if (mac != NULL)
        snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\",\"%s\"", ssid, password, mac);

    return AT_Send_Command(cmd, 10000);
}

bool AT_Is_WiFi_Conected(void)
{
    AT_WiFi_Info_t info;

    if (!AT_Get_WiFi_Info(&info))
        return false;

    return info.connected;
}

/**
 * @brief 设置ESP32-C3的Wi-Fi睡眠模式
 * @param mode 0=关闭睡眠(全速); 1=Modem-sleep按AP的DTIM; 2=Light-sleep; 3=Modem-sleep按listen interval
 * @return true 设置成功,false 失败
 * @note  该设置不写入flash, 模组一旦重启就会丢失, 因此每次上电都要重新下发。
 *        Modem-sleep只按周期关闭RF, Wi-Fi连接保持, 不会掉线。
 */
bool AT_Set_Sleep(uint8_t mode)
{
    char cmd[16];

    snprintf(cmd, sizeof(cmd), "AT+SLEEP=%u", (unsigned)mode);
    if (!AT_Send_Command(cmd, 2000))
        return false;

    return true;
}

/**
 * @brief 解析CWSTATE回复
 * @param response CWSTATE回复字符串
 * @param info WiFi信息结构体指针
 * @return true 成功,false 失败
 */
static bool Parse_CWSTATE_Response(const char *response, AT_WiFi_Info_t *info)
{
    // 命令和命令回复内容：
    //     "AT+CWSTATE?\r\n"
    //     "+CWSTATE:2,\"Laptop -S\"\r\n"
    //     "\r\n"
    //     "OK\r\n";
    response = strstr(response, "+CWSTATE:");

    if (response == NULL)
        return false;

    int wifi_state; // 0=断开, 1=连接中, 2=已连接

    /* 解析wifi_state和ssid */
    if (sscanf(response, "+CWSTATE:%d,\"%63[^\"]\"", &wifi_state, info->ssid) != 2)
        return false;

    info->connected = (wifi_state == 2);

    return true;
}

/**
 * @brief 解析CWJAP回复
 * @param response CWJAP回复字符串
 * @param info WiFi信息结构体指针
 * @return true 成功,false 失败
 */
static bool Parse_CWJAP_Response(const char *response, AT_WiFi_Info_t *info)
{
    // 命令和命令回复内容：
    //     "AT+CWJAP?\r\n"
    //     "+CWJAP:\"Laptop-S\",\"7e:b5:66:b3:56:55\",1,-59,0,1,3,0,1\r\n"
    //     "\r\n"
    //     "OK\r\n";
    response = strstr(response, "+CWJAP:");

    if (response == NULL)
        return false;

    /* 解析ssid/bssid/channel/rssi */
    if (sscanf(response, "+CWJAP:\"%63[^\"]\",\"%17[^\"]\",%d,%d", info->ssid, info->bssid, &info->channel, &info->rssi) != 4)
        return false;

    return true;
}

/* ==================== SNTP相关底层函数 ==================== */

/**
 * @brief 初始化SNTP模块
 * @return true 成功,false 失败
 */
bool AT_SNTP_Init(void)
{
    if (!AT_Send_Command("AT+CIPSNTPCFG=1,8", 2000))
        return false;

    return true;
}

/**
 * @brief 获取SNTP时间
 * @param date_info 日期信息结构体指针
 * @return true 成功,false 失败
 */
bool AT_SNTP_Get_Time(AT_Date_Info_t *date_info)
{
    if (date_info == NULL)
        return false;

    if (!AT_Send_Command("AT+CIPSNTPTIME?", 2000))
        return false;

    /* 解析日期时间 */
    if (!Parse_CIPSNTPTIME_Response(AT_Get_Response(), date_info))
        return false;

    /* ESP 尚未完成NTP同步时会返回 1970, 视为无效 */
    if (date_info->year < 2000 || date_info->month < 1 || date_info->month > 12)
        return false;

    return true;
}

/**
 * @brief 月份字符串转换为月份数字
 * @param month_str 月份字符串
 * @return 月份数字
 */
static uint8_t Month2Num(const char *month_str)
{
    const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    for (uint8_t i = 0; i < 12; i++)
    {
        if (strcmp(month_str, months[i]) == 0)
            return i + 1;
    }

    return 0;
}

/**
 * @brief 星期几字符串转换为星期几数字
 * @param weekday_str 星期几字符串
 * @return 星期几数字
 */
static uint8_t Weekday3Num(const char *weekday_str)
{
    const char *weekdays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

    for (uint8_t i = 0; i < 7; i++)
    {
        if (strcmp(weekday_str, weekdays[i]) == 0)
            return i + 1;
    }
    return 0;
}

/**
 * @brief 解析CIPSNTPTIME回复
 * @param response CIPSNTPTIME回复字符串
 * @param date_info 日期信息结构体指针
 * @return true 成功,false 失败
 */
static bool Parse_CIPSNTPTIME_Response(const char *response, AT_Date_Info_t *date_info)
{
    // 命令和命令回复内容：
    //     "AT+CIPSNTPTIME?\r\n"
    //     "+CIPSNTPTIME:Tue Oct 19 17:47:56 2021\r\n"
    //     "OK\r\n";
    response = strstr(response, "+CIPSNTPTIME:");

    if (response == NULL)
        return false;

    char weekday_str[8];
    char month_str[4];

    /* 解析日期时间 */
    if (sscanf(response, "+CIPSNTPTIME:%3s %3s %hhu %hhu:%hhu:%hhu %hu",
               weekday_str, month_str, &date_info->day, &date_info->hour, &date_info->minute, &date_info->second, &date_info->year) != 7)
        return false;

    date_info->weekday = Weekday3Num(weekday_str);
    date_info->month = Month2Num(month_str);

    return true;
}

/* ==================== HTTP相关底层函数 ==================== */

/**
 * @brief 发送HTTP请求
 * @param url HTTP请求URL
 * @return HTTP响应字符串指针
 */
const char *AT_Get_HTTP(const char *url)
{
    static char tx_buf[256];

    snprintf(tx_buf, sizeof(tx_buf), "AT+HTTPCLIENT=2,1,\"%s\",,,2", url);

    bool ret = AT_Send_Command(tx_buf, 10000);

    if (ret)
    {
        const char *response = AT_Get_Response();
        return response;
    }

    return NULL;
}

/**
 * @brief 解析天气回复
 * @param response 天气回复字符串
 * @param info 天气信息结构体指针
 * @return true 成功,false 失败
 */
bool Parse_Weather_Response(const char *response, AT_Weather_Info_t *info)
{
    // 命令和命令回复内容：
    //     "AT+HTTPCLIENT=2,1,\"https://api.seniverse.com/v3/weather/now.json?key=SgM2NZE2Sghy4FOFh&location=dalian&language=en&unit=c\",,,2\r\n"
    //     "+HTTPCLIENT:267,{\"results\":[{\"location\":{\"id\":\"WWYMRT0VRMUG\",\"name\":\"Dalian\",\"country\":\"CN\",\"path\":\"Dalian,Dalian,Liaoning,China\",\"timezone\":\"Asia/Shanghai\",\"timezone_offset\":\"+08:00\"},\"now\":{\"text\":\"Cloudy\",\"code\":\"4\",\"temperature\":\"29\"},\"last_update\":\"2026-08-16T16:45:45+08:00\"}]}\r\n"
    //     "\r\n"
    //     "OK\r\n";
    response = strstr(response, "\"results\":");

    if (response == NULL)
        return false;

    /* 解析location部分内容 */
    const char *response_location = strstr(response, "\"location\":");
    if (response_location == NULL)
        return false;
    /* 1. 解析城市 */
    const char *response_location_name = strstr(response_location, "\"name\":");
    if (response_location_name)
        sscanf(response_location_name, "\"name\":\"%31[^\"]\"", info->city);
    /* 2. 解析路径（城市，省份，国家） */
    const char *response_location_path = strstr(response_location, "\"path\":");
    if (response_location_path)
        sscanf(response_location_path, "\"path\":\"%127[^\"]\"", info->location);

    /* 解析now部分内容*/
    const char *response_now = strstr(response, "\"now\":");
    if (response_now == NULL)
        return false;
    /* 1. 解析天气描述 */
    const char *response_now_text = strstr(response_now, "\"text\":");
    if (response_now_text)
        sscanf(response_now_text, "\"text\":\"%15[^\"]\"", info->weather);
    /* 2. 解析天气代码 */
    const char *response_now_code = strstr(response_now, "\"code\":");
    if (response_now_code)
    {
        char code_str[8] = {0};

        if (sscanf(response_now_code, "\"code\":\"%7[^\"]\"", code_str) == 1)
            info->weather_code = atoi(code_str);
    }
    /* 3. 解析温度 */
    const char *response_now_temperature = strstr(response_now, "\"temperature\":");
    if (response_now_temperature)
    {
        char temperature_str[16] = {0};
        if (sscanf(response_now_temperature, "\"temperature\":\"%15[^\"]\"", temperature_str) == 1)
            info->temperature = atof(temperature_str);
    }
    
    return true;
}
