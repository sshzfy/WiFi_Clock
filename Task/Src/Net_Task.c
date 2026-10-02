#include "Net_Task.h"

/* ==================== netTask ==================== */

/* 开机网络阶段结果(仅uiTask在EV_NET_READY后读取) */
static NetBoot_t s_boot = {0};

static void Net_Task(void *pvParameters)
{
    (void)pvParameters;

    printf("[NET] Net_Task start\r\n");

    uint32_t sntp_cnt, wifi_cnt, weathercnt; // 三个周期计数器, 1s心跳时递减, 0时触发对应服务
    bool wifi_up;                            // 是否连接成功
    bool wifi_sleeping = false;              // 模组当前是否处于省电档位(仅本任务访问)

    printf("[NET] Init Start\r\n");

    /* 开机阶段: AT/WiFi初始化 + 连接 */
    s_boot.wifi_ok = Wireless_Init(); // 此处 s_boot.wifi_ok 表示初始化无线网络是否成功
    if (s_boot.wifi_ok)
        s_boot.wifi_ok = Service_WiFi_Connect(); // 此处 s_boot.wifi_ok 表示连接WiFi是否成功
    wifi_up = s_boot.wifi_ok;                    // 是否连接成功

    /* 开机阶段: SNTP + 天气(需WiFi) */
    if (wifi_up)
        AT_SNTP_Init();
    bool time_ok = wifi_up && Service_Time_Sync();         // SNTP同步是否成功
    bool weather_ok = wifi_up && Service_Weather_Update(); // 天气更新是否成功

    s_boot.service_ok = wifi_up && time_ok && weather_ok; // 是否所有服务都启动成功

    /* 通知UI: 开机阶段完成(无论成败), 之后后台继续重试 */
    xEventGroupSetBits(Task_Events(), EV_NET_READY);

    /*  周期调度初值  */
    wifi_cnt = wifi_up ? WIFI_PERIOD_S : RETRY_WIFI_S;            // WiFi检查周期,连接上30min,未连10s
    sntp_cnt = time_ok ? SNTP_PERIOD_S : RETRY_SNTP_S;            // SNTP周期,成功后4h,未成功5s
    weathercnt = weather_ok ? WEATHER_PERIOD_S : RETRY_WEATHER_S; // 天气周期,成功后1h,未成功60s

    for (;;)
    {
        /* 1s 心跳; 同时等待"退出低功耗立即补更"请求(netTask 专属位) */
        EventBits_t bits = xEventGroupWaitBits(Task_Events(), EV_NET_UPDATE_NOW, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));

        /* 低功耗状态变化 → 同步模组的Wi-Fi省电档位。
         * 必须放在补更之前: 退出夜间时先让模组恢复全速, 再执行 WiFi/SNTP/天气 */
        if (APP_IS_LOWPOWER() != wifi_sleeping)
        {
            if (Service_WiFi_Sleep(APP_IS_LOWPOWER()))
                wifi_sleeping = APP_IS_LOWPOWER(); /* 失败则保持原状态, 下个周期自动重试 */
        }

        if (bits & EV_NET_UPDATE_NOW)
        {
            /* 三个计数器置 1: 下一拍连续执行 WiFi 检查 + SNTP + 天气 */
            wifi_cnt = 1U;
            sntp_cnt = 1U;
            weathercnt = 1U;

            printf("[NET] LowPower exit, reset update\r\n");
        }

        /* 低功耗期间: 冻结周期计数器, 不执行任何 update */
        if (APP_IS_LOWPOWER())
            continue;

        if (--wifi_cnt == 0U)
        {
            bool was_up = wifi_up;
            int ret = Service_WiFi_Update();
            if (ret < 0) /* 连接失败 */
            {
                wifi_up = false;
                wifi_cnt = RETRY_WIFI_S;
            }
            else /* 连接成功 */
            {
                wifi_up = true;
                wifi_cnt = WIFI_PERIOD_S;
                if (ret > 0) /* 刚重连成功: 重新配置SNTP并立即补SNTP+天气 */
                {
                    AT_SNTP_Init();
                    sntp_cnt = 1U;
                    weathercnt = 1U;
                }
            }
            if (ret != 0 || !was_up)
                xEventGroupSetBits(Task_Events(), EV_WIFI); /* 状态变化/首次连上: 刷顶部条 */
        }

        if (--sntp_cnt == 0U)
        {
            if (!wifi_up)
            {
                sntp_cnt = 1U; // WiFi未连, 每秒顺延, 等wifi恢复后触发
            }
            else if (Service_Time_Sync()) /* SNTP同步成功 */
            {
                sntp_cnt = SNTP_PERIOD_S;
            }
            else
            {
                sntp_cnt = RETRY_SNTP_S;
            }
        }

        if (--weathercnt == 0U)
        {
            if (!wifi_up)
            {
                weathercnt = 1U; // WiFi未连, 每秒顺延, 等wifi恢复后触发
            }
            else if (Service_Weather_Update()) /* 天气更新 */
            {
                weathercnt = WEATHER_PERIOD_S;
                xEventGroupSetBits(Task_Events(), EV_WEATHER); /* 天气更新成功: 刷新天气部分UI */
            }
            else
            {
                weathercnt = RETRY_WEATHER_S;
            }
        }
    }
}

void Net_Task_Start(void)
{
    xTaskCreate(Net_Task, "net", NET_TASK_STACK_SIZE, NULL, NET_TASK_PRIORITY, NULL);
}

void Net_Task_GetBoot(bool *wifi_ok, bool *service_ok)
{
    if (wifi_ok != NULL)
        *wifi_ok = s_boot.wifi_ok;
    if (service_ok != NULL)
        *service_ok = s_boot.service_ok;
}
