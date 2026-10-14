#include "Task_Common.h"

/* 共享事件组: 仅被等待的bit置位才会唤醒 uiTask */
static EventGroupHandle_t s_evt = NULL;

/* 低功耗模式(夜间进入: 网络活动与温湿度采集停止) */
volatile bool g_lowpower = false;

/** @brief 创建共享事件组(幂等), 由 UI_Task 入口第一行调用
 *  @note  放在任务上下文创建, 与拆分前行为一致; 必须先于任何生产者任务启动
 */
void Task_Common_Init(void)
{
    /* 幂等: 重复调用不会重建事件组, 避免丢失已置位的事件 */
    if (s_evt != NULL)
        return;

    taskENTER_CRITICAL();
    if (s_evt == NULL)
        s_evt = xEventGroupCreate();
    taskEXIT_CRITICAL();
}

/** @brief 取共享事件组句柄(替代原先的全局变量 g_evt)
 *  @return 事件组句柄, 未初始化时为 NULL
 */
EventGroupHandle_t Task_Events(void)
{
    return s_evt;
}
