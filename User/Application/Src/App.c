#include "App.h"

char g_ssid[WIFI_SSID_MAX_LEN + 1U] = {0};     // WiFi SSID
char g_password[WIFI_PASS_MAX_LEN + 1U] = {0}; // WiFi 密码
const char *mac = WIFI_MAC;                    // WiFi MAC地址(一般不需要)
char g_location[LOCATION_MAX_LEN + 1U] = {0};  // 定位信息((省+)市,可避免城市拼音重复造成的定位错误)

AT_WiFi_Info_t wifi_info = {0};
AT_Date_Info_t date_info = {0};
AT_Weather_Info_t weather_info = {0};
DHT22_Data_t room_info = {0};
// AT_Location_Info_t location_info = {0}; // 逆地理编码省/市; 当前应用未接入(见 Service_Location_Update)

const char *weekdays[] =
    {"Monday",
     "Tuesday",
     "Wednesday",
     "Thursday",
     "Friday",
     "Saturday",
     "Sunday"};
static char weather_url[WEATHER_URL_SIZE] =
    "https://api.seniverse.com/v3/weather/now.json"
    "?key=" WEATHER_KEY "&location=" WEATHER_IP_LOCATION "&language=en&unit=c"; // 中文: 把 language=en 换成 language=zh-Hans 即可(location 由公网IP动态填入)
static char weather_ip[16] = {0};                                               // 最近一次成功解析到的公网IP(仅netTask访问)
const char *http_response = NULL;                                               // HTTP 响应缓冲区

/* ================ 软件时钟(基于TIM5) ================
 * 线程安全: netTask写(Clock_Sync), uiTask读(Clock_IsSynced/GetDateTime),
 * 读写均在临界区内进行, 避免64位字段(epoch/sync_ms)撕裂读。
 * ================================*/
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

/* ================ WiFi 凭据(键盘输入 → 掉电保存) ================ */
static volatile bool s_wifi_cfg_dirty = false; // 内存里的凭据还没落盘(键盘刚提交过) —— 只由 uiTask 读写

/* 是否已经拿到过 WiFi 凭据 */
bool WiFi_Config_IsSet(void)
{
    return (g_ssid[0] != '\0');
}

/* 内存里的凭据是否还没落盘 */
bool WiFi_Config_IsDirty(void)
{
    return s_wifi_cfg_dirty;
}

/**
 * @brief 更新 WiFi 凭据
 * @param ssid     WiFi 名称, 不允许为 NULL
 * @param password WiFi 密码, 传 NULL 表示开放网络
 */
void WiFi_Config_Set(const char *ssid, const char *password)
{
    if (ssid == NULL)
        return;

    /* 临界区内整体替换: netTask 会在重连时读这两个串, 不能被它看到半截新值 */
    taskENTER_CRITICAL();

    strncpy(g_ssid, ssid, sizeof(g_ssid) - 1U);
    g_ssid[sizeof(g_ssid) - 1U] = '\0';

    if (password != NULL)
    {
        strncpy(g_password, password, sizeof(g_password) - 1U);
        g_password[sizeof(g_password) - 1U] = '\0';
    }
    else
    {
        g_password[0] = '\0';
    }

    taskEXIT_CRITICAL();

    s_wifi_cfg_dirty = true;
}

/**
 * @brief 把当前凭据写入外部 Flash
 * @return true 写入成功
 * @note  只在 uiTask 里调用(退出键盘界面时)。放在那里而不是 LVGL 的事件回调里,
 *        是因为 littlefs 的写时复制要擦一个块, 耗时可到几十 ms, 会卡住界面交互。
 */
bool WiFi_Config_Save(void)
{
    WiFi_Cfg_t cfg;

    if (!WiFi_Config_IsSet())
        return false;

    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = WIFI_CFG_MAGIC;
    cfg.version = WIFI_CFG_VERSION;
    cfg.size = (uint16_t)sizeof(cfg);
    strncpy(cfg.ssid, g_ssid, sizeof(cfg.ssid) - 1U);
    strncpy(cfg.password, g_password, sizeof(cfg.password) - 1U);

    if (Asset_SaveData(WIFI_CFG_NAME, &cfg, sizeof(cfg)) != ASSET_OK)
    {
        printf("[NET] WiFi config save FAILED\r\n");
        return false;
    }

    s_wifi_cfg_dirty = false;
    printf("[NET] WiFi config saved: ssid=%s\r\n", g_ssid);

    return true;
}

/**
 * @brief 从外部 Flash 载入 WiFi 凭据
 * @note  文件不存在或校验不过时清空凭据, 由调用方按"未配置"处理
 */
void WiFi_Config_Load(void)
{
    WiFi_Cfg_t cfg;

    g_ssid[0] = '\0';
    g_password[0] = '\0';

    if (!Asset_HasData(WIFI_CFG_NAME))
    {
        printf("[NET] no saved WiFi config\r\n");
        return;
    }

    int len = Asset_LoadData(WIFI_CFG_NAME, &cfg, sizeof(cfg));
    if (len < (int)sizeof(cfg) || cfg.magic != WIFI_CFG_MAGIC ||
        cfg.version != WIFI_CFG_VERSION || cfg.size != (uint16_t)sizeof(cfg))
    {
        printf("[NET] WiFi config invalid (len=%d), ignored\r\n", len);
        return;
    }

    cfg.ssid[sizeof(cfg.ssid) - 1U] = '\0';
    cfg.password[sizeof(cfg.password) - 1U] = '\0';
    strncpy(g_ssid, cfg.ssid, sizeof(g_ssid) - 1U);
    strncpy(g_password, cfg.password, sizeof(g_password) - 1U);
    g_ssid[sizeof(g_ssid) - 1U] = '\0';
    g_password[sizeof(g_password) - 1U] = '\0';

    printf("[NET] WiFi config loaded: ssid=%s\r\n", g_ssid);
    s_wifi_cfg_dirty = false;
}

/* ================ 定位信息(定位界面输入 → 掉电保存) ================ */
static volatile bool s_location_cfg_dirty = false; // 内存里的定位信息还没落盘 —— 只由 uiTask 读写

/* 是否已经设置过定位信息 */
bool Location_Config_IsSet(void)
{
    return (g_location[0] != '\0');
}

/* 内存里的定位信息是否还没落盘 */
bool Location_Config_IsDirty(void)
{
    return s_location_cfg_dirty;
}

/**
 * @brief 更新内存中的定位信息(不写 Flash, 由 uiTask 稍后落盘)
 * @param location 省+市, 不允许为 NULL
 */
void Location_Config_Set(const char *location)
{
    if (location == NULL)
        return;

    taskENTER_CRITICAL();

    strncpy(g_location, location, sizeof(g_location) - 1U);
    g_location[sizeof(g_location) - 1U] = '\0';

    taskEXIT_CRITICAL();

    s_location_cfg_dirty = true;
}

/**
 * @brief 把当前定位信息写入外部 Flash
 * @return true 写入成功
 */
bool Location_Config_Save(void)
{
    Location_Cfg_t cfg;

    if (!Location_Config_IsSet())
        return false;

    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = LOCATION_CFG_MAGIC;
    cfg.version = LOCATION_CFG_VERSION;
    cfg.size = (uint16_t)sizeof(cfg);
    strncpy(cfg.text, g_location, sizeof(cfg.text) - 1U);

    if (Asset_SaveData(LOCATION_CFG_NAME, &cfg, sizeof(cfg)) != ASSET_OK)
    {
        printf("[UI] location save FAILED\r\n");
        return false;
    }

    s_location_cfg_dirty = false;
    printf("[UI] location saved: %s\r\n", g_location);

    return true;
}

/**
 * @brief 从外部 Flash 载入定位信息
 * @note  文件不存在或校验不过时清空, 由调用方按"未设置"处理
 */
void Location_Config_Load(void)
{
    Location_Cfg_t cfg;

    g_location[0] = '\0';

    if (!Asset_HasData(LOCATION_CFG_NAME))
    {
        printf("[UI] no saved location\r\n");
        return;
    }

    int len = Asset_LoadData(LOCATION_CFG_NAME, &cfg, sizeof(cfg));
    if (len < (int)sizeof(cfg) || cfg.magic != LOCATION_CFG_MAGIC ||
        cfg.version != LOCATION_CFG_VERSION || cfg.size != (uint16_t)sizeof(cfg))
    {
        printf("[UI] location invalid (len=%d), ignored\r\n", len);
        return;
    }

    cfg.text[sizeof(cfg.text) - 1U] = '\0';
    strncpy(g_location, cfg.text, sizeof(g_location) - 1U);
    g_location[sizeof(g_location) - 1U] = '\0';

    printf("[UI] location loaded: %s\r\n", g_location);
    s_location_cfg_dirty = false;
}

/* ================ WiFi 初始化 ================ */

/* 初始化无线网络, 包括AT命令和WiFi */
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
         * (配置损坏、停在不正常状态等)时才"擦配置 + 重启"
         * 平时开机不做恢复出厂, 也就没有那次重启带来的 busy/ready 抢跑窗口 */
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
        return false;
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
 */
static void WiFi_Info_Store(const AT_WiFi_Info_t *info)
{
    taskENTER_CRITICAL();
    wifi_info = *info;
    taskEXIT_CRITICAL();
}

/**
 * @brief 只获取不连接: 等模组用它自己记忆的 AP 连上, 最多等 timeout_ms
 * @param timeout_ms 最长等待时间
 * @return true 已连接
 */
static bool Service_WiFi_WaitConnected(uint32_t timeout_ms)
{
    AT_WiFi_Info_t tmp;
    uint32_t waited = 0U;

    for (;;)
    {
        if (AT_Get_WiFi_Info(&tmp) && tmp.connected)
        {
            WiFi_Info_Store(&tmp);
            printf("[NET] WiFi already connected: ssid=%s rssi=%d\r\n", tmp.ssid, tmp.rssi);
            return true;
        }

        if (waited >= timeout_ms)
            break;

        vTaskDelay(pdMS_TO_TICKS(500));
        waited += 500U;
    }

    memset(&tmp, 0, sizeof(tmp));
    WiFi_Info_Store(&tmp);
    printf("[NET] WiFi not connected (no credentials stored)\r\n");

    return false;
}

/* 连接WiFi并刷新wifi_info */
bool Service_WiFi_Connect(void)
{
    AT_WiFi_Info_t tmp;
    bool ok = false;

    if (!WiFi_Config_IsSet())
    {
        printf("[NET] no WiFi credentials, wait for module auto-reconnect\r\n");
        return Service_WiFi_WaitConnected(WIFI_BOOT_WAIT_MS);
    }

    printf("[NET] WiFi connecting to %s ...\r\n", g_ssid);
    if (!AT_Connect_WiFi(g_ssid, g_password, mac))
    {
        printf("[NET] WiFi connect FAILED\r\n");
        return false;
    }

    /* 读取WiFi信息(失败重试; 避免栈上未初始化值被当成结果) */
    for (int i = 0; i < 3 && !ok; i++)
    {
        memset(&tmp, 0, sizeof(tmp));
        ok = AT_Get_WiFi_Info(&tmp);
        if (!ok)
            vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* 一次性整体替换, 避免ui读到半写状态 */
    if (ok)
        WiFi_Info_Store(&tmp);

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
 * @brief WiFi连接检查/重连
 * @return 0=已连接, 1=本次刚重连成功, -1=获取失败或重连失败
 */
int Service_WiFi_Update(void)
{
    AT_WiFi_Info_t tmp;
    bool ok = false;

    /* 获取失败≠掉线:两次都失败则只报告, 不动连接状态 */
    for (int i = 0; i < 2 && !ok; i++)
    {
        memset(&tmp, 0, sizeof(tmp));
        ok = AT_Get_WiFi_Info(&tmp); // 获取 WiFi 信息
        if (!ok)
            vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (!ok)
    {
        printf("[NET] WiFi state query FAILED\r\n");
        return -1;
    }

    /* 获取成功且已连接 */
    if (tmp.connected)
    {
        WiFi_Info_Store(&tmp);
        printf("[NET] WiFi check OK: ssid=%s rssi=%d\r\n", tmp.ssid, tmp.rssi);
        return 0;
    }

    /* 未配置过凭据 */
    if (!WiFi_Config_IsSet())
    {
        WiFi_Info_Store(&tmp); // connected=0, 让UI同步切到离线图标
        printf("[NET] WiFi offline, no credentials stored\r\n");
        return -1;
    }

    /* 获取成功也配置过凭据但确实未连接 */
    printf("[NET] WiFi lost, reconnecting...\r\n");
    if (!AT_Connect_WiFi(g_ssid, g_password, mac))
    {
        WiFi_Info_Store(&tmp); // connected=0, 让UI同步切到离线图标
        printf("[NET] WiFi reconnect FAILED\r\n");
        return -1;
    }

    /* 重连后重试获取: 与Service_WiFi_Connect保持一致(模组状态更新有延迟) */
    for (int i = 0; i < 3; i++)
    {
        memset(&tmp, 0, sizeof(tmp));
        if (AT_Get_WiFi_Info(&tmp) && tmp.connected)
        {
            WiFi_Info_Store(&tmp);
            printf("[NET] WiFi reconnected: ssid=%s rssi=%d\r\n", tmp.ssid, tmp.rssi);
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    WiFi_Info_Store(&tmp); // connected=0, 让UI同步切到离线图标
    printf("[NET] WiFi reconnect FAILED: no valid info\r\n");

    return -1;
}

/* SNTP时间同步 */
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
 * @brief 把定位串按URL规则编码(只放行 unreserved 字符, 其余按UTF-8逐字节转%XX)
 * @param in 原始定位串
 * @param out 编码结果缓冲区
 * @param out_size out 的字节数
 * @note  用户在定位界面输入的可能是中文("辽宁大连")或带空格("shanxi yulin"),
 *        直接拼进URL会破坏HTTP请求行, 这里统一编码
 */
static void Weather_URL_Encode(const char *in, char *out, size_t out_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;

    for (; *in != '\0' && (used + 4U) <= out_size; in++)
    {
        unsigned char c = (unsigned char)*in;

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
        {
            out[used++] = (char)c;
        }
        else
        {
            out[used++] = '%';
            out[used++] = hex[c >> 4];
            out[used++] = hex[c & 0x0FU];
        }
    }

    out[used] = '\0';
}

/**
 * @brief 用给定定位串重建天气URL
 * @param location 已编码的定位串(城市名 / 公网IP / "ip")
 */
static void Weather_URL_Set_Location(const char *location)
{
    snprintf(weather_url, sizeof(weather_url), WEATHER_URL_FMT, location);
}

/**
 * @brief 回退定位: 按公网IP重建天气URL
 * @note  公网IP会随宽带重拨/换网变化, 所以每次回退前重新解析一次;
 *        取不到IP时先沿用上次的, 从未取到过就交给服务端按请求来源IP定位
 */
static void Weather_Update_Location(void)
{
    char ip[16] = {0};

    if (!AT_Get_IP(ip))
    {
        /* 取IP失败不阻塞天气: 有缓存就用缓存, 否则退到服务端按来源IP定位 */
        if (weather_ip[0] != '\0')
        {
            printf("[NET] public IP FAILED, keep location=%s\r\n", weather_ip);
            Weather_URL_Set_Location(weather_ip);
        }
        else
        {
            printf("[NET] public IP FAILED, fallback to location=%s\r\n", WEATHER_IP_LOCATION);
            Weather_URL_Set_Location(WEATHER_IP_LOCATION);
        }
        return;
    }

    /* 出口IP有变化才刷新缓存 */
    if (strcmp(ip, weather_ip) != 0)
    {
        strncpy(weather_ip, ip, sizeof(weather_ip) - 1);
        weather_ip[sizeof(weather_ip) - 1] = '\0';
        printf("[NET] public IP = %s\r\n", weather_ip);
    }

    /* 无条件重建: 上一步的URL里装的可能是用户输入的省市 */
    Weather_URL_Set_Location(weather_ip);
}

/**
 * @brief 用当前 weather_url 取一次天气并解析
 * @param info 解析结果输出
 * @return true 成功
 */
static bool Weather_Fetch(AT_Weather_Info_t *info)
{
    http_response = AT_Get_HTTP(weather_url);
    if (http_response == NULL)
    {
        printf("[NET] Weather HTTP FAILED\r\n");
        return false;
    }

    if (!Parse_Http_Response(http_response, info))
    {
        printf("[NET] Weather parse FAILED\r\n");
        return false;
    }

    return true;
}

/**
 * @brief 天气更新(每1h)
 * @note  定位优先级: 定位界面输入的"(省+)市" > 网络IP。
 *        用户输入的定位取不到数据(城市名写错等, 表现为响应里没有 results)时,
 *        自动回退到网络IP再取一次, 保证界面上始终有天气可显示
 */
bool Service_Weather_Update(void)
{
    AT_Weather_Info_t tmp;
    char location[LOCATION_MAX_LEN + 1U] = {0};
    char encoded[LOCATION_MAX_LEN * 3U + 1U]; // 每字节最坏编码成3字符
    bool ok = false;

    /* 临界区内取一份定位信息: uiTask 随时可能改它 */
    taskENTER_CRITICAL();
    strncpy(location, g_location, sizeof(location) - 1U);
    location[sizeof(location) - 1U] = '\0';
    taskEXIT_CRITICAL();

    /* 优先用用户输入的定位 */
    if (location[0] != '\0')
    {
        Weather_URL_Encode(location, encoded, sizeof(encoded));
        Weather_URL_Set_Location(encoded);
        printf("[NET] Weather by user location: %s\r\n", location);

        ok = Weather_Fetch(&tmp);
        if (!ok)
            printf("[NET] user location FAILED, fallback to network IP\r\n");
    }

    /* 回退网络IP定位(未输入定位时也走这里) */
    if (!ok)
    {
        Weather_Update_Location();
        ok = Weather_Fetch(&tmp);
    }

    if (!ok)
        return false;

    taskENTER_CRITICAL();
    weather_info = tmp; // 一次性整体替换, 避免ui读到半写状态
    taskEXIT_CRITICAL();

    printf("[NET] Weather OK: %s, code=%d, temp=%.1f\r\n",
           weather_info.weather, weather_info.weather_code, weather_info.temperature);

    return true;
}

// /**
//  * @brief 按GPS经纬度反查省/市(高德逆地理编码)
//  * @param latitude  纬度(十进制度, 南纬为负)
//  * @param longitude 经度(十进制度, 西经为负)
//  * @return true 成功
//  * @note  **当前应用未接入** —— 界面上的城市名改由天气接口按公网IP定位提供
//  *        (weather_info.city), 所以这里没有任何调用者。接口与实现保留, 将来
//  *        要做"按实际位置显示省市"时, 在 netTask 里调它即可(见 4.13)。
//  *        调用方约定: 失败时本函数**不清空** location_info, 界面可继续显示上一次
//  *        的结果, 避免信号短暂丢失就把顶部条闪成空白。
//  */
// bool Service_Location_Update(float latitude, float longitude)
// {
//     AT_Location_Info_t tmp;

//     if (!AT_Get_Location(latitude, longitude, &tmp))
//     {
//         printf("[NET] Location FAILED\r\n");
//         return false;
//     }

//     taskENTER_CRITICAL();
//     location_info = tmp; // 一次性整体替换, 避免ui读到半写状态
//     taskEXIT_CRITICAL();

//     /* 打印的是接口返回的UTF-8原文, 调试终端需按UTF-8显示 */
//     printf("[NET] Location OK: %s%s\r\n", location_info.province, location_info.city);
//     return true;
// }

/* 房间温湿度更新(每20min, DHT22) */
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
