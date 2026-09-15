#include "Profiling.h"

/* DWT(Data Watchpoint and Trace) 的 CYCCNT 是一个自由运行的 32 位周期计数器,
 * 在 168MHz 下约 25.6 秒回绕一次, 足够覆盖单次渲染的测量窗口。 */

void Prof_Init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; // 使能跟踪与调试模块
    DWT->CYCCNT = 0;                                // 计数清零
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;            // 启动周期计数器
}

uint32_t Prof_Cycles(void)
{
    return DWT->CYCCNT;
}

uint32_t Prof_Us(uint32_t start)
{
    uint32_t delta = DWT->CYCCNT - start; // 无符号相减, 自动处理回绕
    uint32_t per_us = SystemCoreClock / 1000000U;

    return (per_us == 0U) ? 0U : (delta / per_us);
}
