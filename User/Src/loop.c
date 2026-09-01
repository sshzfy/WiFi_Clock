#include "loop.h"
#include "Page.h"

#define MS(x) (x)
#define SECOND(x) MS((x) * 1000)
#define MINUTE(x) SECOND((x) * 60)
#define HOUR(x) MINUTE((x) * 60)
#define DAY(x) HOUR((x) * 24)

#define TIME_SYNC_PERIOD (HOUR(4))        // SNTP时间同步周期: 4h
#define TIME_UPDATE_PERIOD (HOUR(1))      // 本地时间戳刷新显示周期: 1h
#define WIFI_UPDATE_PERIOD (MINUTE(30))   // WiFi检查/重连周期: 30min
#define WEATHER_UPDATE_PERIOD (HOUR(1))   // 天气更新周期: 1h
#define ROOM_UPDATE_PERIOD (MINUTE(20))   // 房间温湿度更新周期: 20min
#define RETRY_SYNC_PERIOD (SECOND(5))     // SNTP同步失败重试: 5s
#define RETRY_WIFI_PERIOD (SECOND(10))    // WiFi重连失败重试: 10s
#define RETRY_WEATHER_PERIOD (SECOND(60)) // 天气更新失败重试: 60s

static uint32_t time_sync_period;
static uint32_t time_update_period;
static uint32_t wifi_update_period;
static uint32_t weather_update_period;
static uint32_t room_update_period;

static void period_callback(void)
{
    if (time_sync_period > 0)
        time_sync_period--;
    if (time_update_period > 0)
        time_update_period--;
    if (wifi_update_period > 0)
        wifi_update_period--;
    if (weather_update_period > 0)
        weather_update_period--;
    if (room_update_period > 0)
        room_update_period--;
}

/**
 * @brief 主页面循环: 开机初始化后进入周期调度, 永不返回
 * @param None
 * @return None
 */
void loop(void)
{
    register_period_callback(period_callback); // 注册1ms周期回调(TIM5中断递减周期计数器)

    Boot_Page_Display(); // 开机初始化WiFi/SNTP/天气(失败不阻塞)
    Main_Page_Display(); // 显示主页面(时钟、日期、天气、房间温湿度)

    /* 按开机状态设置周期计数器初值 */
    time_sync_period = Clock_IsSynced() ? TIME_SYNC_PERIOD : RETRY_SYNC_PERIOD;
    time_update_period = TIME_UPDATE_PERIOD;
    wifi_update_period = wifi_info.connected ? WIFI_UPDATE_PERIOD : RETRY_WIFI_PERIOD;
    weather_update_period = WEATHER_UPDATE_PERIOD;
    room_update_period = ROOM_UPDATE_PERIOD;

    while (1)
    {
        Main_Page_Clock_Update();
        delay_ms(20);

        /* SNTP时间同步: 每4h一次, 失败5s重试 */
        if (time_sync_period == 0)
        {
            if (Service_Time_Sync())
                time_sync_period = TIME_SYNC_PERIOD;
            else
                time_sync_period = RETRY_SYNC_PERIOD;
        }

        /* 从本地时间戳刷新时钟/日期显示: 每1h */
        if (time_update_period == 0)
        {
            time_update_period = TIME_UPDATE_PERIOD;
            Main_Page_Clock_Update();
        }

        /* WiFi检查/重连: 每30min, 失败10s重试 */
        if (wifi_update_period == 0)
        {
            int r = Service_WiFi_Update();
            if (r < 0)
            {
                wifi_update_period = RETRY_WIFI_PERIOD;
            }
            else
            {
                wifi_update_period = WIFI_UPDATE_PERIOD;
                if (r > 0) // 刚重连成功, 立即补一次SNTP和天气
                {
                    time_sync_period = 0;
                    weather_update_period = 0;
                }
            }
        }

        /* 天气更新: 每1h, 失败60s重试 */
        if (weather_update_period == 0)
        {
            if (Service_Weather_Update())
            {
                weather_update_period = WEATHER_UPDATE_PERIOD;
                Main_Page_Weather_Update();
            }
            else
            {
                weather_update_period = RETRY_WEATHER_PERIOD;
            }
        }

        /* 房间温湿度(DHT22): 每20min */
        if (room_update_period == 0)
        {
            room_update_period = ROOM_UPDATE_PERIOD;
            if (Service_Room_Update())
                Main_Page_Room_Update();
        }
    }
}
