#include "Timer.h"

static volatile uint64_t TIM5_ms = 0; // TIM5 毫秒计数器
static period_callback_t timer_callback = NULL; // 周期回调(每1ms触发)

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
    NVIC_InitTypeDef NVIC_InitStructure;

    uint32_t tim_clk = SystemCoreClock / 2; // APB1分频≥2时定时器时钟 = SystemCoreClock/2

    TIM_TimeBaseStructure.TIM_Prescaler = (uint16_t)(tim_clk / 1000000 - 1); // 计数时钟 = 1MHz
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseStructure.TIM_Period = (uint16_t)(1000 - 1); // 1kHz → 1ms
    TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseStructure.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(TIM5, &TIM_TimeBaseStructure);

    TIM_ITConfig(TIM5, TIM_IT_Update, ENABLE);

    NVIC_InitStructure.NVIC_IRQChannel = TIM5_IRQn;
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
        if (timer_callback)
            timer_callback();
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
 * @brief 注册周期回调函数, 每个TIM5更新中断(1ms)调用一次
 * @param callback 回调函数指针, 可为NULL
 * @return None
 */
void register_period_callback(period_callback_t callback)
{
    timer_callback = callback;
}
