#ifndef __APP_TASK_H__
#define __APP_TASK_H__

#include "main.h"
#include "UI_Task.h"
#include "Task_Common.h"

/* ============================================================
 * 应用任务层入口(装配层)
 * ------------------------------------------------------------
 * 任务实现已按角色拆分到 Task/Src 下(头文件在 Task/Inc),
 * 每个任务只暴露自己的 *_Task_Start() / 必要的跨任务请求接口:
 *
 *   app_task.c        (本文件)  装配层: 启动UI任务, 等待开网完成
 *   UI_Task.c         (prio 3)  LCD唯一写者。板级初始化→开机等待画面→
 *                               创建net/sensor任务→开机结果→主页面→事件驱动刷新
 *   Net_Task.c        (prio 2)  独占USART1/AT。开机连网+SNTP+天气; 之后按周期
 *                               WiFi保活/SNTP/天气更新, 数据就绪后通知uiTask
 *   DHT22_Task.c      (prio 4)  DHT22温湿度周期采集, 更新后通知uiTask。
 *                               优先级高于ui, 避免UI抢占破坏DHT22的us级时序
 *   LightSensor_Task  (prio 2)  光敏传感器中断采集, 更新后通知uiTask
 *   Key_Task.c        (prio 2)  按键(PA0)手势识别, 产出单击/双击/三击/长按;
 *                               目前长按 → 请求切换昼夜(交LightSensorTask裁决)
 *
 * 注意: 事件位(EV_*)与低功耗标志等共享定义在 Task_Common.h;
 *       任务参数(优先级/栈/周期)集中在 Task_Config.h。
 * ============================================================ */

void App_Task_Init(void);

#endif /* __APP_TASK_H__ */
