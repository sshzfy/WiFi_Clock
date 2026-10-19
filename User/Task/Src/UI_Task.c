#include "UI_Task.h"

/* ================ static 函数 ================ */
static void UI_Boot_Sequence(void);
static void UI_Task(void *pvParameters);

/** @brief 开机流程: 板级初始化 → 开机画面 → 启动子任务 → 等待网络 → 结果页 */
static void UI_Boot_Sequence(void)
{
    /* 事件组: 仅被等待的bit置位才会唤醒 */
    Task_Common_Init();

    /* TIM5/LCD/USART2/LCD 和触摸初始化 */
    Board_Init();

    /* 开始屏幕触摸检测(校准/读回上次校准参数) */
    UI_Input_Calibrate();
    DBG_PRINTF("[UI] UI_Task start\r\n");
    DBG_PRINTF("[UI] Board init done, boot page\r\n");

    /* 取回上次保存的WiFi凭据与定位信息:
     * 没有就是空, 后续按"未配置"处理 */
    WiFi_Config_Load();
    Location_Config_Load();

    /* 资源层未就绪时字模与图片都取不到, 屏幕只会出现纯色块。
     * 这里只提示, 不阻塞流程 —— 网络与传感器功能仍可正常使用。 */
    if (!Asset_Ready())
    {
        DBG_PRINTF("[UI] WARN: asset layer not ready, screen shows blank blocks\r\n");
        DBG_PRINTF("[UI] WARN: flash the provision firmware to write fonts/images\r\n");
    }

    /* 开机等待页面 */
    Boot_Page_Wait();

    /* 创建DHT22/网络服务任务 */
    DHT22_Task_Start();
    Net_Task_Start();

    /* 等待开机网络阶段结束 */
    EventBits_t boot_bits = xEventGroupWaitBits(Task_Events(), EV_NET_READY, pdTRUE, pdFALSE,
                                                pdMS_TO_TICKS(UI_BOOT_NET_TIMEOUT_MS));
    if ((boot_bits & EV_NET_READY) == 0)
    {
        DBG_PRINTF("[UI] WARN: boot net stage timeout, showing partial result\r\n");
    }

    /* 网络结果 */
    bool boot_wifi = false, boot_service = false;
    Net_Task_GetBoot(&boot_wifi, &boot_service); // 获取开机网络结果
    DBG_PRINTF("[UI] Boot net stage done: wifi=%d service=%d\r\n", (int)boot_wifi, (int)boot_service);

    /* 开机结果(连接详情)
     * 结果页停留, 便于查看详情 */
    Boot_Page_Show(boot_wifi, boot_service);
    vTaskDelay(pdMS_TO_TICKS(2500));
}

/** @brief ui 任务主体: 开机 → 主页面 → 事件循环
 *  @note  本任务是 LCD 的唯一写者; 事件循环只做"等 → 切换 → 分派 → 刷新"
 */
static void UI_Task(void *pvParameters)
{
    (void)pvParameters;

    UI_Boot_Sequence();

    /* 进入主页面 */
    UI_Mode_Init();

    /* 进入主页面后再启动光敏任务与按键任务: 保证昼夜切换只发生在开机完成之后 */
    LightSensor_Task_Start();
    Key_Task_Start();

    /* 任务全部建齐后启用内存监测(开关见 BuildConfig.h 的 MONITOR_TASK_ENABLE) */
    Task_Monitor_Start();

    for (;;)
    {
        EventBits_t bits = xEventGroupWaitBits(Task_Events(), UI_TASK_EVENTS,
                                               pdTRUE, pdFALSE, UI_Mode_WaitTicks());

        /* 昼夜切换 */
        if (bits & EV_LOWERPOWER)
        {
            UI_Mode_SwitchToNight();
            xEventGroupSetBits(Task_Events(), EV_LOWPOWER_ACK); // 确认夜间切换
        }
        if (bits & EV_WAKEUP)
        {
            UI_Mode_SwitchToDay();
            xEventGroupSetBits(Task_Events(), EV_LOWPOWER_ACK); // 确认白天切换
        }

        /* 输入界面开关 / 页面数据刷新(由当前模式决定是否执行) */
        UI_Mode_HandleEvent(bits);

        /* 当前模式的周期刷新: 键盘心跳 / 夜间分钟时钟 / 白天时钟 + 触摸轮询 */
        UI_Mode_Tick();
    }
}

/* uiTask: LCD唯一写者 + 开机流程编排者(创建 net/sensor 任务) */
void UI_Task_Start(void)
{
    xTaskCreate(UI_Task, "ui", UI_TASK_STACK_SIZE, NULL, UI_TASK_PRIORITY, NULL);
}
