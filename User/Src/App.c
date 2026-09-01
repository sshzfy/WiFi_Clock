#include "App.h"

/* 初始化WiFi信息 */
const char *ssid = "Jasmine";
const char *password = "Sun507109!";
const char *mac = NULL;
AT_WiFi_Info_t wifi_info = {0};
AT_Date_Info_t date_info = {0};
AT_Weather_Info_t weather_info = {0};
const char *weekdays[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
static const char *weather_url = "https://api.seniverse.com/v3/weather/now.json?key=SgM2NZE2Sghy4FOFh&location=dalian&language=en&unit=c";
const char *http_response = NULL;

Room_Info_t room_info = {0};

/* ================ 软件时钟(基于TIM5) ================ */
static bool clock_synced = false; // 是否同步
static uint64_t clock_epoch;      // 同步时刻的"Unix时间戳"(SNTP本地时间按UTC算法)
static uint64_t clock_sync_ms;    // 同步时刻的 TIM5_Get_ms()

static int64_t Clock_DaysFromCivil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;                                                   // 若月份 ≤ 2，年份减1（用于计算闰年）
    int64_t era = (y >= 0 ? y : y - 399) / 400;                    // 400年周期
    unsigned yoe = (unsigned)(y - era * 400);                      // 周期内年份
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // 年内第几天
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // 周期内第几天
    return era * 146097 + (int64_t)doe - 719468;                   // 1970-01-01 对应的儒略日偏移,146097天/400年,719468天为1970-01-01,纪元偏移量
}

static void Clock_CivilFromDays(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468; // z为自1970-01-01开始的天数,z+719468将基准点调整为公元1年3月1日
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int yy = (int)yoe + (int)era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yy + (*m <= 2);
}

/**
 * @brief 用SNTP本地时间校准软件时钟
 * @param date_info SNTP解析出的本地时间
 * @return None
 */
void Clock_Sync(AT_Date_Info_t *date_info)
{
    if (date_info == NULL || date_info->year < 2000 || date_info->month < 1 || date_info->month > 12)
        return;
    int64_t days = Clock_DaysFromCivil(date_info->year, date_info->month, date_info->day);
    clock_epoch = (uint32_t)(days * 86400 + date_info->hour * 3600 + date_info->minute * 60 + date_info->second);
    clock_sync_ms = TIM5_Get_ms();
    clock_synced = true;
}

/**
 * @brief 检查软件时钟是否已校准
 * @return true 已校准
 * @return false 未校准
 */
bool Clock_IsSynced(void)
{
    return clock_synced;
}

/**
 * @brief 获取当前本地时间
 * @param time_out 输出时间结构体
 * @return None
 */
void Clock_GetDateTime(AT_Date_Info_t *time_out)
{
    if (time_out == NULL)
        return;
    memset(time_out, 0, sizeof(*time_out));
    if (clock_synced == false)
        return;

    uint64_t elapsed_ms = TIM5_Get_ms() - clock_sync_ms;        // 从同步时刻到当前时间的毫秒数
    uint64_t now = clock_epoch + (uint64_t)(elapsed_ms / 1000); // 当前时间的秒数
    int64_t days = now / 86400;
    int64_t secs = now % 86400;
    int year;
    unsigned month, day;
    Clock_CivilFromDays(days, &year, &month, &day);
    time_out->year = (uint16_t)year;
    time_out->month = (uint8_t)month;
    time_out->day = (uint8_t)day;
    time_out->hour = (uint8_t)(secs / 3600);
    time_out->minute = (uint8_t)((secs % 3600) / 60);
    time_out->second = (uint8_t)(secs % 60);
    time_out->weekday = (uint8_t)(((days + 3) % 7 + 7) % 7 + 1);
}

/* === WiFi 初始化 === */

/**
 * @brief 初始化无线网络, 包括AT命令和WiFi
 *
 * @return true 初始化成功
 * @return false 初始化失败
 */
bool Wireless_Init(void)
{
    /* 初始化AT命令 */
    printf("AT_Init\r\n");
    if (!AT_Init())
    {
        printf("[AT_Init] failed\r\n");
        goto err;
    }

    /* 初始化WiFi */
    printf("AT_WiFi_Init\r\n");
    if (!AT_WiFi_Init())
    {
        printf("[AT_WiFi_Init] failed\r\n");
        goto err;
    }

    return true;
err:
    return false;
}

/* === Service 任务初始化(连接WiFi、初始化SNTP、获取时间信息、发送天气请求、解析HTTP请求) === */

/**
 * @brief 初始化服务, 包括连接WiFi并获取信息,
 *                       初始化SNTP并获取时间信息,
 *                       发送天气请求并解析HTTP请求获取天气信息
 *
 * @return true 初始化成功
 * @return false 初始化失败
 */
bool Service_Init(void)
{
    /* 连接WiFi */
    printf("AT_Connect_WiFi\r\n");
    if (!AT_Connect_WiFi(ssid, password, mac))
    {
        printf("[AT_Connect_WiFi] failed\r\n");
        // goto err;
    }
    // 获取WiFi信息
    AT_Get_WiFi_Info(&wifi_info);
    if (AT_Connect_WiFi(ssid, password, mac) && wifi_info.connected)
    {
        printf("[WiFi Connected] ssid: %s, bssid: %s, channel: %d, rssi: %d\r\n",
               wifi_info.ssid, wifi_info.bssid, wifi_info.channel, wifi_info.rssi);
    }
    delay_ms(500);

    /* 初始化SNTP */
    printf("AT_SNTP_Init\r\n");
    if (!AT_SNTP_Init())
    {
        printf("[AT_SNTP_Init] failed\r\n");
        goto err;
    }
    // 获取时间信息
    if (!AT_SNTP_Get_Time(&date_info))
    {
        printf("[AT_SNTP_Get_Time] failed\r\n");
        // goto err;
    }
    else if (date_info.weekday <= 0 || date_info.weekday > 7)
    {
        printf("%04u-%02u-%02u %02u:%02u:%02u %s\r\n",
               date_info.year, date_info.month, date_info.day, date_info.hour, date_info.minute, date_info.second,
               "Error");
    }
    else
    {
        printf("%04u-%02u-%02u %02u:%02u:%02u %s\r\n",
               date_info.year, date_info.month, date_info.day, date_info.hour, date_info.minute, date_info.second,
               weekdays[date_info.weekday - 1]);
    }
    delay_ms(500);

    /* 获取天气信息 */
    http_response = AT_Get_HTTP(weather_url);
    if (http_response == NULL)
    {
        printf("[AT_Get_HTTP] failed\r\n");
        goto err;
    }
    if (!parse_weather_response(http_response, &weather_info))
    {
        printf("[Parse_weather_response] failed\r\n");
        goto err;
    }
    printf("City: %s, Location: %s, Weather: %s, Weather_Code: %d, Temperature: %.1f\r\n",
           weather_info.city, weather_info.location, weather_info.weather, weather_info.weather_code, weather_info.temperature);

    return true;

err:
    return false;
}

/* === 周期任务(由main调度) === */

/**
 * @brief 周期任务: SNTP时间同步(每4h)
 * @return true 成功
 */
bool Service_Time_Sync(void)
{
    if (!AT_SNTP_Get_Time(&date_info))
    {
        printf("[Service_Time_Sync] failed\r\n");
        return false;
    }
    Clock_Sync(&date_info);
    return true;
}

/**
 * @brief 周期任务: WiFi连接检查/重连(每30min)
 * @return 0=已连接, 1=本次刚重连成功, -1=重连失败
 */
int Service_WiFi_Update(void)
{
    if (AT_Is_WiFi_Conected())
    {
        AT_Get_WiFi_Info(&wifi_info);
        return 0;
    }

    printf("[Service_WiFi_Update] WiFi lost, reconnecting...\r\n");
    if (!AT_Connect_WiFi(ssid, password, mac))
        return -1;

    AT_Get_WiFi_Info(&wifi_info);
    return 1;
}

/**
 * @brief 周期任务: 天气更新(每1h)
 * @return true 成功
 */
bool Service_Weather_Update(void)
{
    http_response = AT_Get_HTTP(weather_url);
    if (http_response == NULL)
    {
        printf("[Service_Weather_Update] HTTP failed\r\n");
        return false;
    }
    if (!parse_weather_response(http_response, &weather_info))
    {
        printf("[Service_Weather_Update] parse failed\r\n");
        return false;
    }
    printf("Weather: %s, Code: %d, Temp: %.1f\r\n",
           weather_info.weather, weather_info.weather_code, weather_info.temperature);
    return true;
}

/**
 * @brief 周期任务: 房间温湿度更新(每20min, DHT22)
 * @return true 数据已刷新
 */
bool Service_Room_Update(void)
{
    static bool dht22_inited = false;
    DHT22_Data_t dht22;

    if (!dht22_inited)
    {
        DHT22_Init();
        dht22_inited = true;
    }

    if (DHT22_ReadData(&dht22) == 0)
    {
        room_info.temperature = dht22.temperature;
        room_info.humidity = dht22.humidity;
        room_info.valid = true;
        printf("[DHT22] T=%.1f H=%.1f\r\n", room_info.temperature, room_info.humidity);
    }
    else
    {
        room_info.valid = false;
        printf("[DHT22] read failed\r\n");
    }
    return true;
}
