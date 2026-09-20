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

DHT22_Data_t room_info = {0};

/* ================ 软件时钟(基于TIM5) ================
 * 线程安全: netTask写(Clock_Sync), uiTask读(Clock_IsSynced/GetDateTime),
 * 读写均在临界区内进行, 避免64位字段(epoch/sync_ms)撕裂读。
 */
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
    uint64_t epoch = (uint32_t)(days * 86400 + date_info->hour * 3600 + date_info->minute * 60 + date_info->second);
    uint64_t ms = TIM5_Get_ms();

    taskENTER_CRITICAL();
    clock_epoch = epoch;
    clock_sync_ms = ms;
    clock_synced = true;
    taskEXIT_CRITICAL();
}

/**
 * @brief 检查软件时钟是否已校准
 * @return true 已校准,false 未校准
 */
bool Clock_IsSynced(void)
{
    bool synced;

    taskENTER_CRITICAL();
    synced = clock_synced;
    taskEXIT_CRITICAL();

    return synced;
}

/**
 * @brief 获取当前本地时间
 * @param time_out 输出时间结构体
 * @return None
 */
void Clock_GetDateTime(AT_Date_Info_t *time_out)
{
    bool synced;
    uint64_t epoch;
    uint64_t sync_ms;
    uint64_t now_ms;

    if (time_out == NULL)
        return;
    memset(time_out, 0, sizeof(*time_out));

    /* 临界区内一次性读取时钟状态(避免与netTask的写入撕裂) */
    taskENTER_CRITICAL();
    synced = clock_synced;
    epoch = clock_epoch;
    sync_ms = clock_sync_ms;
    now_ms = TIM5_Get_ms();
    taskEXIT_CRITICAL();

    if (synced == false)
        return;

    uint64_t elapsed_ms = now_ms - sync_ms;               // 从同步时刻到当前时间的毫秒数
    uint64_t now = epoch + (uint64_t)(elapsed_ms / 1000); // 当前时间的秒数
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

/* ================ WiFi 初始化 ================ */

/* 开机阶段 AT/WiFi 初始化的最大尝试次数。
 * 偶发失败的真实原因是"模组还没准备好": 上电或重启途中它会先吐 ready / busy p... /
 * ERROR 而不是我们要的 OK, 隔一会儿重试一次通常就正常了。 */
#define WIRELESS_INIT_RETRY 3U
#define WIRELESS_INIT_GAP_MS 200U // 两次尝试之间的等待时间(ms)

/**
 * @brief 初始化无线网络, 包括AT命令和WiFi
 * @return true 初始化成功
 * @return false 初始化失败
 */
bool Wireless_Init(void)
{
    bool at_ok = false;

    /* 初始化AT命令 */
    for (uint8_t retry = 1; retry <= WIRELESS_INIT_RETRY; retry++)
    {
        if (AT_Init())
        {
            at_ok = true;
            break;
        }

        printf("[NET] AT init FAILED, retry=%u/%u, rx len=%u: %s\r\n",
               (unsigned)retry, (unsigned)WIRELESS_INIT_RETRY,
               (unsigned)strlen(AT_Get_Response()), AT_Get_Response());

        /* 第一次失败后才动用 AT+RESTORE: 正常开机不会走到这里, 只有模组确实不应答
         * (配置损坏、停在不正常状态等)时才付出这次"擦配置 + 重启"的代价。
         * 平时开机不做恢复出厂, 也就没有那次重启带来的 busy/ready 抢跑窗口。 */
        if (retry == 1)
        {
            printf("[NET] AT init failed, try factory reset (AT+RESTORE)\r\n");

            if (!AT_Factory_Reset())
                printf("[NET] factory reset FAILED, rx len=%u: %s\r\n",
                       (unsigned)strlen(AT_Get_Response()), AT_Get_Response());
        }

        if (retry < WIRELESS_INIT_RETRY)
            vTaskDelay(pdMS_TO_TICKS(WIRELESS_INIT_GAP_MS));
    }

    if (!at_ok)
    {
        printf("[NET] AT init FAILED, max retry reached\r\n");
        return false; /* AT 都没起来, 后面的 WiFi 命令不可能成功 */
    }
    printf("[NET] AT init OK\r\n");

    /* 初始化WiFi */
    for (uint8_t retry = 1; retry <= WIRELESS_INIT_RETRY; retry++)
    {
        if (AT_WiFi_Init())
        {
            printf("[NET] WiFi init OK\r\n");
            return true;
        }

        printf("[NET] WiFi init FAILED, retry=%u/%u, rx len=%u: %s\r\n",
               (unsigned)retry, (unsigned)WIRELESS_INIT_RETRY,
               (unsigned)strlen(AT_Get_Response()), AT_Get_Response());

        if (retry < WIRELESS_INIT_RETRY)
            vTaskDelay(pdMS_TO_TICKS(WIRELESS_INIT_GAP_MS));
    }

    printf("[NET] WiFi init FAILED, max retry reached\r\n");
    return false;
}

/* ================ WiFi 连接 ================ */

/**
 * @brief 整体替换wifi_info(临界区内赋值, 避免UI读到半写状态)
 * @param info 新的WiFi信息
 * @return None
 */
static void WiFi_Info_Store(const AT_WiFi_Info_t *info)
{
    taskENTER_CRITICAL();
    wifi_info = *info;
    taskEXIT_CRITICAL();
}

/**
 * @brief 连接WiFi并刷新wifi_info
 * @return true 已连接
 */
bool Service_WiFi_Connect(void)
{
    AT_WiFi_Info_t tmp;
    bool ok = false;

    printf("[NET] WiFi connecting to %s ...\r\n", ssid);
    if (!AT_Connect_WiFi(ssid, password, mac))
    {
        printf("[NET] WiFi connect FAILED\r\n");
        return false;
    }

    /* 读取WiFi信息(失败重试; 避免栈上未初始化值被当成结果) */
    for (int i = 0; i < 3 && !ok; i++)
    {
        memset(&tmp, 0, sizeof(tmp)); /* 清空结构体, 避免未初始化值被当成结果 */
        ok = AT_Get_WiFi_Info(&tmp);
        if (!ok)
            vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (ok)
        WiFi_Info_Store(&tmp); /* 一次性整体替换, 避免ui读到半写状态 */

    if (ok && wifi_info.connected)
    {
        printf("[NET] WiFi connected: ssid=%s bssid=%s channel=%d rssi=%d\r\n",
               wifi_info.ssid, wifi_info.bssid, wifi_info.channel, wifi_info.rssi);
    }
    else
    {
        printf("[NET] WiFi info FAILED\r\n");
    }
    return (ok && wifi_info.connected);
}

/* ================ WiFi 省电 ================ */

/* ESP32-C3的Wi-Fi睡眠档位(见 AT+SLEEP 命令):
 * 0=关闭睡眠(全速, 约80mA)
 * 1=Modem-sleep, RF按AP的DTIM周期关闭(ESP32-C3约20mA)  ← 夜间使用
 * 2=Light-sleep(约130uA, 但必须先用 AT+SLEEPWKCFG 配好唤醒源;
 *   ESP32-C3只支持定时器/GPIO唤醒, 当前硬件未连唤醒GPIO, 故不使用)
 * 3=Modem-sleep, RF按 AT+CWJAP 的 listen interval 关闭 */
#define WIFI_SLEEP_MODE_IDLE 1U /* 空闲(夜间无网络活动)档位 */
#define WIFI_SLEEP_MODE_FULL 0U /* 全速(白天需要快速响应)档位 */

/**
 * @brief 切换ESP32-C3的Wi-Fi省电档位
 * @param enable true=进入省电(夜间), false=恢复全速(白天)
 * @return true 设置成功
 * @note  只改变RF的开关占空比, Wi-Fi连接保持, 因此退出夜间无需重新连接;
 *        设置失败时调用方保持原状态, 下个周期会自动重试
 */
bool Service_WiFi_Sleep(bool enable)
{
    if (!AT_Set_Sleep(enable ? WIFI_SLEEP_MODE_IDLE : WIFI_SLEEP_MODE_FULL))
    {
        printf("[NET] WiFi sleep set FAILED\r\n");
        return false;
    }

    printf("[NET] WiFi %s\r\n", enable ? "modem-sleep" : "wake");
    return true;
}

/* ================ 周期任务(由netTask/sensorTask调度) ================ */

/**
 * @brief 周期任务: WiFi连接检查/重连
 * @note  "AT查询失败"与"模组确实没连上"是两回事: 前者只重试查询, 不触发重连,
 *        避免AT指令抖动(模组忙/串口残留)被误判成掉线而做一次无谓的CWJAP
 * @return 0=已连接, 1=本次刚重连成功, -1=查询失败或重连失败
 */
int Service_WiFi_Update(void)
{
    AT_WiFi_Info_t tmp;
    bool ok = false;

    /* 查询失败≠掉线: 先重试一次; 两次都失败则只报告, 不动连接状态 */
    for (int i = 0; i < 2 && !ok; i++)
    {
        memset(&tmp, 0, sizeof(tmp)); /* 清零, 失败时不残留上次结果 */
        ok = AT_Get_WiFi_Info(&tmp);
        if (!ok)
            vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (!ok)
    {
        printf("[NET] WiFi state query FAILED\r\n");
        return -1;
    }

    if (tmp.connected) /* 查询成功且已连接 */
    {
        WiFi_Info_Store(&tmp);
        printf("[NET] WiFi check OK: ssid=%s rssi=%d\r\n", tmp.ssid, tmp.rssi);
        return 0;
    }

    /* 查询成功但确实未连接: 重连 */
    printf("[NET] WiFi lost, reconnecting...\r\n");
    if (!AT_Connect_WiFi(ssid, password, mac)) /* 连接WiFi */
    {
        WiFi_Info_Store(&tmp); /* connected=0, 让UI同步切到离线图标 */
        printf("[NET] WiFi reconnect FAILED\r\n");
        return -1;
    }

    /* 重连后重试查询: 与Service_WiFi_Connect保持一致(模组状态更新有延迟) */
    for (int i = 0; i < 3; i++)
    {
        memset(&tmp, 0, sizeof(tmp)); /* 清零, 失败时不残留上次结果 */
        if (AT_Get_WiFi_Info(&tmp) && tmp.connected)
        {
            WiFi_Info_Store(&tmp);
            printf("[NET] WiFi reconnected: ssid=%s rssi=%d\r\n", tmp.ssid, tmp.rssi);
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    WiFi_Info_Store(&tmp); /* 保守置为离线, 下个周期重试会纠正 */
    printf("[NET] WiFi reconnect FAILED: no valid info\r\n");
    return -1;
}

/**
 * @brief 周期任务: SNTP时间同步
 * @return true 成功
 */
bool Service_Time_Sync(void)
{
    AT_Date_Info_t t;

    /* ESP 配网后SNTP需数秒才真正同步; 轮询直到取到有效时间(约15s), 期间得不到则重试 */
    for (int i = 0; i < 15; i++)
    {
        if (AT_SNTP_Get_Time(&t))
        {
            date_info = t;
            Clock_Sync(&date_info);
            printf("[NET] SNTP sync OK: %04u-%02u-%02u %02u:%02u:%02u\r\n",
                   t.year, t.month, t.day, t.hour, t.minute, t.second);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    printf("[NET] SNTP sync FAILED\r\n");
    return false;
}

/**
 * @brief 周期任务: 天气更新(每1h)
 * @return true 成功
 */
bool Service_Weather_Update(void)
{
    AT_Weather_Info_t tmp;

    http_response = AT_Get_HTTP(weather_url);
    if (http_response == NULL)
    {
        printf("[NET] Weather HTTP FAILED\r\n");
        return false;
    }
    if (!Parse_Weather_Response(http_response, &tmp))
    {
        printf("[NET] Weather parse FAILED\r\n");
        return false;
    }
    taskENTER_CRITICAL();
    weather_info = tmp; /* 一次性整体替换, 避免ui读到半写状态 */
    taskEXIT_CRITICAL();
    printf("[NET] Weather OK: %s, code=%d, temp=%.1f\r\n",
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
    uint8_t dht_ret;

    if (!dht22_inited)
    {
        DHT22_Init();
        dht22_inited = true;
    }

    dht_ret = DHT22_ReadData(&dht22);
    if (dht_ret == DHT22_OK)
    {
        taskENTER_CRITICAL();
        room_info.temperature = dht22.temperature;
        room_info.humidity = dht22.humidity;
        room_info.valid = true;
        taskEXIT_CRITICAL();
        printf("[SENSOR] DHT22 OK: T=%.1f H=%.1f\r\n", room_info.temperature, room_info.humidity);
    }
    else
    {
        taskENTER_CRITICAL();
        room_info.valid = false;
        taskEXIT_CRITICAL();
        printf("[SENSOR] DHT22 FAIL: code=%d (%s)\r\n", (int)dht_ret, DHT22_ErrString(dht_ret));
    }
    return true;
}
