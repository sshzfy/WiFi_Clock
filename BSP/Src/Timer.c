#include "Timer.h"

#include "stm32f4xx.h"

static volatile uint64_t TIM5_ms = 0; // TIM5 毫秒计数器

/**
 * @brief 初始化TIM5为1ms定时器, 实现RTC时间基准
 * @note TIM5挂在APB1总线上, 当APB1预分频≥2时, 定时器时钟 = APB1 * 2 = SystemCoreClock / 2
 *       例: SystemCoreClock=168MHz时, TIM5时钟=84MHz, PSC=83 → 1MHz, ARR=999 → 1kHz(1ms中断)
 */
void TIM5_Init(void)
{
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure;
    RCC_ClocksTypeDef RCC_Clocks;

    RCC_GetClocksFreq(&RCC_Clocks);

    uint32_t hclk1 = RCC_Clocks.HCLK_Frequency;  // AHB时钟频率
    uint32_t pclk1 = RCC_Clocks.PCLK1_Frequency; // APB1时钟频率
    uint32_t tim5_clk;                           // TIM5时钟频率
    /* APB时钟1分频时, 定时器时钟 = APB1 */
    if (hclk1 == pclk1)
    {
        tim5_clk = pclk1;
    }
    /* APB时钟分频≥2,时, 定时器时钟 = APB1 * 2 */
    else
    {
        tim5_clk = 2 * pclk1;
    }

    TIM_TimeBaseStructInit(&TIM_TimeBaseStructure);
    TIM_TimeBaseStructure.TIM_Prescaler = (uint16_t)(tim5_clk / 1000000 - 1); // 计数器时钟 = 1MHz
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;               // 向上计数
    TIM_TimeBaseStructure.TIM_Period = (uint16_t)(1000 - 1);                  // ARR = 999, 1kHz → 1ms
    TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;                   // 1分频
    TIM_TimeBaseStructure.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(TIM5, &TIM_TimeBaseStructure);

    TIM_ClearITPendingBit(TIM5, TIM_IT_Update); // 清除更新中断标志位
    TIM_ITConfig(TIM5, TIM_IT_Update, ENABLE);  // 使能更新中断

    NVIC_InitTypeDef NVIC_InitStructure;
    NVIC_InitStructure.NVIC_IRQChannel = TIM5_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 2; // 该优先级(2)高于configMAX_SYSCALL_INTERRUPT_PRIORITY(5),
                                                              // 因此TIM5中断内禁止调用任何FreeRTOS API, 只能做计数等简单操作
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE; // 使能TIM5_IRQn
    NVIC_Init(&NVIC_InitStructure);

    TIM_Cmd(TIM5, ENABLE);
}

/** @brief TIM5更新中断服务函数: 每1ms触发一次
 *  @note 先自增 TIM5_ms，再清 UIF。
 */
void TIM5_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM5, TIM_IT_Update) != RESET)
    {
        TIM5_ms++;
        TIM_ClearITPendingBit(TIM5, TIM_IT_Update);
    }
}

/**
 * @brief 获取TIM5毫秒计数器
 * @return 自TIM5_Init以来经过的毫秒数
 */
uint64_t TIM5_Get_ms(void)
{
    uint32_t primask = __get_PRIMASK();
    uint64_t ms;

    __disable_irq();
    ms = TIM5_ms;
    __set_PRIMASK(primask);

    return ms;
}

/**
 * @brief 获取TIM5微秒计数器
 * @return 自TIM5_Init以来经过的微秒数
 */
uint64_t TIM5_Get_us(void)
{
    uint32_t primask = __get_PRIMASK();
    uint32_t cnt; // TIM5计数器值
    uint64_t ms;  // TIM5毫秒计数器值

    __disable_irq();
    cnt = TIM5->CNT;
    ms = TIM5_ms;
    /* 更新事件已发生但 ISR 尚未执行: CNT 已回绕进入新周期, 重读;
     * TIM5_ms 还是旧值, 补上这一毫秒。 */
    if (TIM5->SR & TIM_SR_UIF)
    {
        cnt = TIM5->CNT;
        ms++;
    }
    __set_PRIMASK(primask);

    return ms * 1000U + cnt;
}

void delay_us(uint32_t us)
{
    uint64_t start = TIM5_Get_us();

    /* 用有符号差比较而不是无符号: 采样值一旦出现回退, 无符号相减会下溢成巨大值,
     * 循环条件立刻不成立而提前返回。有符号比较会把它看成负的"已过时间", 继续等待。 */
    while ((int64_t)(TIM5_Get_us() - start) < (int64_t)us)
    {
    }
}

void delay_ms(uint32_t ms)
{
    uint64_t now = TIM5_Get_ms();
    while (TIM5_Get_ms() - now < (uint64_t)ms)
    {
    }
}
