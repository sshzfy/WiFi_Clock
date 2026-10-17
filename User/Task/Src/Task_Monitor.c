#include "Task_Monitor.h"
#include "BuildConfig.h"
#include "Task_Config.h"
#include "Task_Common.h"

#if (MONITOR_TASK_ENABLE == 1)

/* ============================================================
 * 任务内存监测(只读)
 * ------------------------------------------------------------
 * 输出两块内容:
 *   堆 —— 总量 / 当前已用 / 当前空闲 / 历史最小空闲(即峰值占用);
 *   栈 —— 每个任务的历史最小剩余(high water mark)与据此推算的最坏使用率。
 * 栈总大小 FreeRTOS 不保存, 所以下面用一张"任务名 → 创建时栈字数"的
 * 对照表反推使用率; 表里没有的任务只报剩余量(表格里显示 "-")。
 * ============================================================ */

static const Monitor_Stack_Map_t s_stack_map[] =
    {
        {"ui", UI_TASK_STACK_SIZE},                     // UI_TASK.c
        {"net", NET_TASK_STACK_SIZE},                   // Net_Task.c
        {"dht22", DHT22_TASK_STACK_SIZE},               // DHT22_Task.c
        {"light_sensor", LIGHT_SENSOR_TASK_STACK_SIZE}, // LightSensor_Task.c
        {"key", KEY_TASK_STACK_SIZE},                   // Key_Task.c
        {"monitor", MONITOR_TASK_STACK_SIZE},           // 本文件
        {"IDLE", configMINIMAL_STACK_SIZE},             // 内核空闲任务
};

static volatile bool s_printing = false; // 防重入: 周期报告与手动触发同时发生时, 后来者直接丢弃, 避免两份输出交叉

/* 任务状态枚举转可读文本 */
static const char *Monitor_StateText(eTaskState state)
{
    switch (state)
    {
    case eRunning:
        return "RUN";
    case eReady:
        return "READY";
    case eBlocked:
        return "BLOCK";
    case eSuspended:
        return "SUSP";
    case eDeleted:
        return "DEL";
    default:
        return "?";
    }
}

/** @brief 按任务名查创建时的栈字数
 *  @return 栈字数(word); 0 表示表里没有该任务
 */
static uint16_t Monitor_StackWords(const char *name)
{
    uint32_t i;

    for (i = 0U; i < STACK_MAP_COUNT; i++)
    {
        if (strcmp(s_stack_map[i].name, name) == 0)
            return s_stack_map[i].words;
    }

    return 0U;
}

void Task_Monitor_Print(void)
{
    static TaskStatus_t s_status[MONITOR_MAX_TASKS];
    UBaseType_t task_count;
    UBaseType_t snapshot;
    UBaseType_t i;
    uint32_t heap_total;
    uint32_t heap_free;
    uint32_t heap_low;
    uint32_t heap_used;
    uint32_t heap_peak;
    uint32_t stack_words_sum = 0U;
    TickType_t tick;

    if (s_printing)
        return;
    s_printing = true;

    tick = xTaskGetTickCount();
    task_count = uxTaskGetNumberOfTasks();
    snapshot = uxTaskGetSystemState(s_status, (UBaseType_t)MONITOR_MAX_TASKS, NULL);

    heap_total = (uint32_t)(configTOTAL_HEAP_SIZE);
    heap_free = (uint32_t)xPortGetFreeHeapSize();
    heap_low = (uint32_t)xPortGetMinimumEverFreeHeapSize();
    heap_used = heap_total - heap_free; // 当前已用
    heap_peak = heap_total - heap_low;  // 历史峰值占用

    DBG_PRINTF("\r\n[MEM] ============== FreeRTOS memory report ==============\r\n");
    DBG_PRINTF("[MEM] uptime %u s | tasks %u | snapshot %u\r\n",
           (unsigned)(tick / (TickType_t)configTICK_RATE_HZ),
           (unsigned)task_count,
           (unsigned)snapshot);

    if ((snapshot == 0U) && (task_count != 0U))
    {
        /* 数组掉小: 任务数已超过 MONITOR_MAX_TASKS, 加大它即可 */
        DBG_PRINTF("[MEM] WARN: snapshot failed, raise MONITOR_MAX_TASKS (>= %u)\r\n",
               (unsigned)task_count);
    }

    DBG_PRINTF("[MEM] heap total : %u B\r\n", (unsigned)heap_total);
    DBG_PRINTF("[MEM] heap used  : %u B (%u%%)\r\n",
           (unsigned)heap_used,
           (unsigned)((heap_total != 0U) ? (heap_used * 100U / heap_total) : 0U));
    DBG_PRINTF("[MEM] heap free  : %u B\r\n", (unsigned)heap_free);
    DBG_PRINTF("[MEM] heap peak  : used %u B (%u%%) | min-free-ever %u B\r\n",
           (unsigned)heap_peak,
           (unsigned)((heap_total != 0U) ? (heap_peak * 100U / heap_total) : 0U),
           (unsigned)heap_low);

    /* 栈单位统一换成字节: word 数 × 4, 便于直接和堆对比 */
    DBG_PRINTF("[MEM] name             prio state      stack   peak    rem   use%%\r\n");

    for (i = 0U; i < snapshot; i++)
    {
        uint16_t words = Monitor_StackWords(s_status[i].pcTaskName);
        uint16_t rem_w = (uint16_t)s_status[i].usStackHighWaterMark; // 历史最小剩余(word)

        if (words != 0U)
        {
            uint16_t peak_w = (words > rem_w) ? (uint16_t)(words - rem_w) : 0U; // 最坏已用
            uint32_t use_pct = ((uint32_t)peak_w * 100U) / words;

            stack_words_sum += words;
            DBG_PRINTF("[MEM] %-16s %4u %-8s %6u %6u %6u %5u\r\n",
                   s_status[i].pcTaskName,
                   (unsigned)s_status[i].uxCurrentPriority,
                   Monitor_StateText(s_status[i].eCurrentState),
                   (unsigned)(words * WORD_BYTES),
                   (unsigned)(peak_w * WORD_BYTES),
                   (unsigned)(rem_w * WORD_BYTES),
                   (unsigned)use_pct);
        }
        else
        {
            /* 未登记的任务: 只知道还剩多少, 不知道当初申请了多少 */
            DBG_PRINTF("[MEM] %-16s %4u %-8s      -      - %6u     -\r\n",
                   s_status[i].pcTaskName,
                   (unsigned)s_status[i].uxCurrentPriority,
                   Monitor_StateText(s_status[i].eCurrentState),
                   (unsigned)(rem_w * WORD_BYTES));
        }
    }

    /* 任务栈全部来自这 42KB 堆, 合计能直接看出堆被任务栈吃掉了多少 */
    DBG_PRINTF("[MEM] task stacks : %u B (%u%% of heap)\r\n",
           (unsigned)(stack_words_sum * WORD_BYTES),
           (unsigned)((heap_total != 0U) ? (stack_words_sum * WORD_BYTES * 100U / heap_total) : 0U));
    DBG_PRINTF("[MEM] ====================================================\r\n");

    s_printing = false;
}

/* 监测任务主循环: 周期报告, 夜间静默 */
static void Monitor_Task(void *pvParameters)
{
    (void)pvParameters;

    /* 开机阶段任务还没建齐, 早报的数字没有参考价值, 先静置一段时间 */
    vTaskDelay(pdMS_TO_TICKS(MONITOR_FIRST_DELAY_MS));

    for (;;)
    {
        if (APP_IS_LOWPOWER())
        {
            /* 夜间不打印: 串口活动会把功耗拉起来, 只需低频回来检查一次 */
            vTaskDelay(pdMS_TO_TICKS(MONITOR_LP_PERIOD_MS));
            continue;
        }

        Task_Monitor_Print();
        vTaskDelay(pdMS_TO_TICKS(MONITOR_PERIOD_MS));
    }
}

void Task_Monitor_Start(void)
{
    xTaskCreate(Monitor_Task, "monitor", MONITOR_TASK_STACK_SIZE, NULL, MONITOR_TASK_PRIORITY, NULL);
}

#else /* MONITOR_TASK_ENABLE == 0: 编译期剔除, 不建任务也不占堆 */

void Task_Monitor_Start(void)
{
}

void Task_Monitor_Print(void)
{
}

#endif /* MONITOR_TASK_ENABLE */
