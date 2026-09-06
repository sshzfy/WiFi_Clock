#include "Timer.h"

static volatile uint64_t TIM5_ms = 0; // TIM5 毫秒计数器

/**
 * @brief 初始化TIM5为1ms定时器, 实现RTC时间基准
 *
 * TIM5挂在APB1总线上, 当APB1预分频≥2时, 定时器时钟 = APB1 * 2 = SystemCoreClock / 2
 * 例: SystemCoreClock=168MHz时, TIM5时钟=84MHz, PSC=83 → 1MHz, ARR=999 → 1kHz(1ms中断)
 *
 * @param None
 * @return None
 */
void TIM5_Init(void)
{
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure;
    TIM_TimeBaseStructInit(&TIM_TimeBaseStructure);

    uint32_t tim_clk = SystemCoreClock / 2; // APB1分频≥2时定时器时钟 = SystemCoreClock/2

    TIM_TimeBaseStructure.TIM_Prescaler = (uint16_t)(tim_clk / 1000000 - 1); // 计数时钟 = 1MHz
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseStructure.TIM_Period = (uint16_t)(1000 - 1); // 1kHz → 1ms
    TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseStructure.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(TIM5, &TIM_TimeBaseStructure);

    TIM_ITConfig(TIM5, TIM_IT_Update, ENABLE);

    NVIC_InitTypeDef NVIC_InitStructure;

    NVIC_InitStructure.NVIC_IRQChannel = TIM5_IRQn;
    /* 注意: 该优先级(2)高于configMAX_SYSCALL_INTERRUPT_PRIORITY(5),
       因此TIM5中断内禁止调用任何FreeRTOS API, 只能做计数等简单操作 */
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 2;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    TIM_Cmd(TIM5, ENABLE);
}

/**
 * @brief TIM5更新中断服务函数: 每1ms触发一次
 * @param None
 * @return None
 */
void TIM5_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM5, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(TIM5, TIM_IT_Update);
        TIM5_ms++;
    }
}

/**
 * @brief 获取TIM5毫秒计数器
 * @return uint64_t 自TIM5_Init以来经过的毫秒数
 */
uint64_t TIM5_Get_ms(void)
{
    return TIM5_ms;
}

/**
 * @brief 获取TIM5微秒计数器(单调递增)
 *
 * TIM5以1MHz计数(CNT: 0~999), 更新中断在回绕时对TIM5_ms+1。
 * 先读CNT再读ms, ms在采样期间变化则重读; 当CNT很小时复检ms是否已进位,
 * 避免回绕瞬间(中断尚未+1)读到偏小值。
 *
 * @return uint64_t 自TIM5_Init以来经过的微秒数
 */
uint64_t TIM5_Get_us(void)
{
    uint32_t cnt;
    uint64_t ms;

    do
    {
        cnt = TIM5->CNT;
        ms = TIM5_ms;
    } while (ms != TIM5_ms); // ms在采样期间被中断更新则重读

    if (cnt < 256U) // CNT接近回绕起点, 复检ms是否已进位
    {
        uint64_t ms2 = TIM5_ms;
        if (ms2 != ms)
            ms = ms2; // 已进入下一毫秒, 当前cnt属于ms2
    }

    return ms * 1000U + cnt;
}

void delay_us(uint32_t us)
{
    uint64_t now = TIM5_Get_us();
    while (TIM5_Get_us() - now < (uint64_t)us)
        ;
}

void delay_ms(uint32_t ms)
{
    uint64_t now = TIM5_Get_ms();
    while (TIM5_Get_ms() - now < (uint64_t)ms)
        ;
}
