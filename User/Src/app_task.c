#include "app_task.h"
#include "main.h"
#include "Board.h"
#include "Page.h"
#include "App.h"
#include "event_groups.h"

/* ================ 任务角色 ================
 * uiTask    (prio 3): LCD唯一写者。做板级初始化→开机等待画面→
 *                     创建net/sensor任务→开机结果→主页面→事件驱动刷新
 * netTask   (prio 2): 独占USART1/AT。开机连网+SNTP+天气; 之后按周期
 *                     WiFi保活/SNTP/天气更新, 数据就绪后通知uiTask
 * sensorTask(prio 4): DHT22温湿度周期采集, 更新后通知uiTask。
 *                     优先级高于ui, 避免UI抢占破坏DHT22的us级时序
 * ========================================== */

/* 开机网络阶段结果(仅uiTask在EV_NET_READY后读取) */
typedef struct
{
    volatile bool wifi_ok;    // 开机WiFi初始化+连接是否完成(成功或失败)
    volatile bool service_ok; // 开机SNTP+天气是否都成功
} NetBoot_t;

static NetBoot_t s_boot;
static EventGroupHandle_t g_evt = NULL;

/* 周期参数(单位: 秒) */
#define SNTP_PERIOD_S     (4UL * 3600UL)  /* SNTP 4h */
#define WEATHER_PERIOD_S  (3600UL)        /* 天气 1h */
#define WIFI_PERIOD_S     (60UL)          /* WiFi检查 60s(缩短以更快感知掉线) */
#define RETRY_SNTP_S      (5UL)           /* SNTP重试 5s */
#define RETRY_WEATHER_S   (60UL)          /* 天气重试 60s */
#define RETRY_WIFI_S      (10UL)          /* WiFi重试 10s */ 

/* ==================== sensorTask ==================== */

static void vTask_Sensor(void *param)
{
    (void)param;

    for (;;)
    {
        if (Service_Room_Update()) /* DHT22 读取, 成功失败都会刷新room_info */
        {
            xEventGroupSetBits(g_evt, EV_ROOM);
        }
        vTaskDelay(pdMS_TO_TICKS(10 * 1000));
    }
}

/* ==================== netTask ==================== */

static void vTask_Net(void *param)
{
    (void)param;
    TickType_t last;
    uint32_t sntp_c, wifi_c, weather_c;
    bool wifi_up;

    printf("[NET] Init Start\r\n");

    /* ---- 开机阶段: AT/WiFi初始化 + 连接 ---- */
    s_boot.wifi_ok = Wireless_Init();
    if (s_boot.wifi_ok)
        s_boot.wifi_ok = Service_WiFi_Connect();
    wifi_up = s_boot.wifi_ok;

    /* ---- 开机阶段: SNTP + 天气(需WiFi) ---- */
    if (wifi_up)
        AT_SNTP_Init();
    bool t_ok = Service_Time_Sync();
    bool w_ok = Service_Weather_Update();

    s_boot.service_ok = wifi_up && t_ok && w_ok;

    /* 通知UI: 开机阶段完成(无论成败), 之后后台继续重试 */
    xEventGroupSetBits(g_evt, EV_NET_READY);

    /* ---- 周期调度初值 ---- */
    wifi_c = wifi_up ? WIFI_PERIOD_S : RETRY_WIFI_S;       /* WiFi检查周期,连接上30min,未连10s */
    sntp_c = t_ok    ? SNTP_PERIOD_S : RETRY_SNTP_S;       /* SNTP周期,成功后4h,未成功5s */
    weather_c = w_ok ? WEATHER_PERIOD_S : RETRY_WEATHER_S; /* 天气周期,成功后1h,未成功60s */

    last = xTaskGetTickCount();
    for (;;)
    {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000)); /* 1s心跳 */

        if (--wifi_c == 0U)
        {
            int r = Service_WiFi_Update();
            if (r < 0)
            {
                wifi_up = false;
                wifi_c = RETRY_WIFI_S;
            }
            else
            {
                wifi_up = true;
                wifi_c = WIFI_PERIOD_S;
                if (r > 0) /* 刚重连成功: 重新配置SNTP并立即补SNTP+天气 */
                {
                    AT_SNTP_Init();
                    sntp_c = 1U;
                    weather_c = 1U;
                }
            }
            if (r != 0)
                xEventGroupSetBits(g_evt, EV_WIFI); /* 状态变化: 通知UI刷顶部条 */
        }

        if (--sntp_c == 0U)
        {
            if (!wifi_up)
            {
                sntp_c = 1U; /* WiFi未连, 每秒顺延, 等wifi恢复后触发 */
            }
            else if (Service_Time_Sync())
            {
                sntp_c = SNTP_PERIOD_S;
            }
            else
            {
                sntp_c = RETRY_SNTP_S;
            }
        }

        if (--weather_c == 0U)
        {
            if (!wifi_up)
            {
                weather_c = 1U;
            }
            else if (Service_Weather_Update())
            {
                weather_c = WEATHER_PERIOD_S;
                xEventGroupSetBits(g_evt, EV_WEATHER);
            }
            else
            {
                weather_c = RETRY_WEATHER_S;
            }
        }
    }
}

/* ==================== uiTask(LCD唯一写者) ==================== */

static void vTask_UI(void *param)
{
    (void)param;
    EventBits_t bits = 0;

    g_evt = xEventGroupCreate(); /* 事件组: 仅被等待的bit置位才会唤醒 */

    Board_Init(); /* TIM5/LCD/USART2/ST7789 初始化 */
    printf("[SYS]Build Date:%s %s\r\n", __DATE__, __TIME__);
    printf("[UI] Board init done, boot page\r\n");
    Boot_Page_Wait(); /* 开机等待画面 */

    /* 创建采集/服务任务(sensor=4保护DHT22时序, net=2低于ui避免饿死显示) */
    xTaskCreate(vTask_Sensor, "sensor", 512, NULL, 4, NULL);
    xTaskCreate(vTask_Net, "net", 1024, NULL, 2, NULL);

    /* 等待开机网络阶段结束(成功或失败都置EV_NET_READY), 超时30s兜底 */
    xEventGroupWaitBits(g_evt, EV_NET_READY, pdTRUE, pdFALSE, pdMS_TO_TICKS(30 * 1000));
    printf("[UI] Boot net stage done: wifi=%d service=%d\r\n", (int)s_boot.wifi_ok, (int)s_boot.service_ok);

    Boot_Page_Show(s_boot.wifi_ok, s_boot.service_ok); /* 开机结果(连接详情) */
    vTaskDelay(pdMS_TO_TICKS(2500)); /* 结果页停留, 便于查看详情 */
    printf("[UI] Enter main page\r\n");
    Main_Page_Display(); /* 进入主页面(数据已就绪, 首绘即正确) */

    for (;;)
    {
        /* 500ms心跳 + 事件唤醒(仅等待的bit被置位才返回) */
        bits = xEventGroupWaitBits(g_evt, EV_WEATHER | EV_ROOM | EV_WIFI, pdTRUE, pdFALSE,
                                   pdMS_TO_TICKS(500));

        if (bits & EV_WIFI)
            Main_Page_Net_Update(); /* WiFi连接状态变化: 刷顶部WiFi/定位条 */
        if (bits & EV_WEATHER)
        {
            Main_Page_Weather_Update();
            Main_Page_Net_Update(); /* 天气到位后同步城市/定位图标 */
        }
        if (bits & EV_ROOM)
            Main_Page_Room_Update();

        Main_Page_Clock_Update(); /* 时钟/日期刷新 */
    }
}

void App_Task_Init(void)
{
    xTaskCreate(vTask_UI, "ui", 1024, NULL, 3, NULL);
}
