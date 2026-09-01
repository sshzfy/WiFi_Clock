#include "delay.h"

#define SYSTICK_CNT_PER_US (SystemCoreClock / 1000000UL) // 每个SysTick周期的微秒数168
#define SYSTICK_CNT_PER_MS (SystemCoreClock / 1000UL)    // 每个SysTick周期的毫秒数168000

static volatile uint64_t SysTick_Cnt = 0; // SysTick 计数器

void SysTick_Init(void)
{
    SysTick->LOAD = SystemCoreClock / 1000 - 1;
    SysTick->VAL = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;
}

uint64_t Get_SysTick_Cnt(void) // get_now
{
    uint64_t last_cnt, cnt;

    do
    {
        last_cnt = SysTick_Cnt;                           // 记录上一次读取的SysTick计数
        cnt = SysTick_Cnt + SysTick->LOAD - SysTick->VAL; // 计算当前SysTick计数
    } while (last_cnt != SysTick_Cnt);

    return cnt; // now
}

uint64_t Get_us(void)
{
    return Get_SysTick_Cnt() / SYSTICK_CNT_PER_US;
}

uint64_t Get_ms(void)
{
    return Get_SysTick_Cnt() / SYSTICK_CNT_PER_MS;
}

void delay_us(uint32_t us)
{
    uint64_t now = Get_SysTick_Cnt();
    while (Get_SysTick_Cnt() - now < (uint64_t)us * SYSTICK_CNT_PER_US)
        ;
}

void delay_ms(uint32_t ms)
{
    uint64_t now = Get_SysTick_Cnt();
    while (Get_SysTick_Cnt() - now < (uint64_t)ms * SYSTICK_CNT_PER_MS)
        ;
}

void SysTick_Handler(void)
{
    SysTick_Cnt += SYSTICK_CNT_PER_MS;
}
