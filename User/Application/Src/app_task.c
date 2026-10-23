#include "app_task.h"

#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx.h"

#include "boot_layout.h"
#include "boot_param.h"

/* ============================================================
 * 任务装配层
 * ------------------------------------------------------------
 * 这里只在启动调度器前创建 UI 任务。
 * 其余任务(网络/温湿度/光敏/按键)由 UI 任务按开机时序自行创建,
 * 以保证"开机画面优先、昼夜切换只在主页面完成后才可能发生"。
 *
 * 引入 Bootloader 后，这个文件多做了两件事：
 *   1. 把向量表重新指回 App 区；
 *   2. 启动一个 5 秒后写"确认标志"的小任务。
 * 这两件事都和 OTA 的回滚机制有关，下面分别解释。
 * ============================================================ */

/**
 * @brief  把中断向量表重新指向 App 起始地址
 * @note   为什么需要这一步？
 *
 *         Bootloader 跳转前已经把 SCB->VTOR 设成 0x08010000。
 *         但 App 启动文件会调用 SystemInit()，而 system_stm32f4xx.c
 *         里的 VECT_TAB_OFFSET 仍然是 0x00，于是 SystemInit() 又把
 *         VTOR 改回 0x08000000。
 *
 *         如果在改回来之后、App 重新修正之前发生了中断，
 *         CPU 会去 0x08000000 取向量——那是 Bootloader 的向量表，
 *         中断服务程序完全对不上，轻则功能异常，重则 HardFault。
 *
 *         main() 到 App_Task_Init() 之间目前没有开任何中断，
 *         所以这里是安全的修正点。以后如果要在更早的阶段开中断，
 *         应该改为在 system_stm32f4xx.c 里用 APP_BUILD 条件编译
 *         VECT_TAB_OFFSET，而不是依赖这个函数。
 */
static void Boot_Apply_VectorTable(void)
{
    SCB->VTOR = APP_BASE;
    __DSB();   /* 确保 VTOR 写入完成 */
    __ISB();   /* 清流水线，避免继续执行旧取指 */
}

/**
 * @brief  新固件启动确认任务
 * @param  pvParameters 未使用
 * @note   这是 OTA 回滚机制的"App 侧一半"：
 *
 *           Bootloader 把新固件写进 App 区后，会把参数状态置为
 *           APP_UNCONFIRMED，并且在每次跳转前把 boot_try 加 1。
 *           如果新 App 能正常跑起来，就应该在这里写 CONFIRMED，
 *           告诉 Bootloader"这个版本没问题"。
 *
 *           如果新 App 一启动就崩溃/复位，这个任务永远跑不到，
 *           boot_try 会一次次累加；达到 3 次后 Bootloader 就会
 *           从 /ota/backup.bin 回滚到上一个版本。
 *
 *         为什么延迟 5 秒而不是在 main 里立刻写？
 *           立刻写只能证明"代码能跑到 main"，证明不了 LCD、任务、
 *           堆栈这些初始化是否正常。延迟 5 秒让系统多跑一会儿，
 *           能过滤掉大部分"启动即崩溃"的情况。
 *
 *         为什么用独立任务而不是在 App_Task_Init 里直接写？
 *           App_Task_Init 运行在调度器启动之前，此时写 Flash 会
 *           长时间关中断，而且还没证明调度器能正常跑起来。
 *           放到任务里、延迟 5 秒，更接近"系统真的活了"。
 */
static void Boot_Confirm_Task(void *pvParameters)
{
    (void)pvParameters;

    /* 给系统 5 秒时间完成基本启动；期间如果崩溃复位，
     * 这个任务不会执行，boot_try 会在下次启动时继续累加。 */
    vTaskDelay(pdMS_TO_TICKS(5000));

    /* 写参数区。成功与否都不影响当前运行，失败最多导致下次
     * 启动时 Bootloader 认为仍未确认、继续累加 boot_try。 */
    (void)Boot_Param_Confirm();

    /* 一次性任务，做完就删掉自己，把栈还给堆 */
    vTaskDelete(NULL);
}

/**
 * @brief 初始化应用任务
 * @note 仅在启动调度器前创建 UI 任务
 */
void App_Task_Init(void)
{
    /* 抢在任何中断使能之前，把向量表修正到 App 区 */
    Boot_Apply_VectorTable();

    /* 创建启动确认任务。
     * 优先级 1 是最低档（0~4），它只延迟等待，不会和业务任务抢 CPU。
     * 栈 128 words = 512 B：Boot_Param_Confirm 内部只有一个
     * 64 B 的结构体和少量局部变量，够用。 */
    xTaskCreate(Boot_Confirm_Task, "boot_cfm", 128, NULL, 1, NULL);

    UI_Task_Start();
}
