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
    RCC_ClocksTypeDef RCC_Clocks;
    RCC_GetClocksFreq(&RCC_Clocks);

    uint32_t hclk1 = RCC_Clocks.HCLK_Frequency;  // AHB时钟频率
    uint32_t pclk1 = RCC_Clocks.PCLK1_Frequency; // APB1时钟频率
    uint32_t tim5_clk;

    if (hclk1 == pclk1) /* APB时钟1分频时, 定时器时钟 = APB1 */
    {
        tim5_clk = pclk1;
    }
    else /* APB时钟分频≥2,时, 定时器时钟 = APB1 * 2 */
    {
        tim5_clk = 2 * pclk1;
    }

    TIM_TimeBaseStructure.TIM_Prescaler = (uint16_t)(tim5_clk / 1000000 - 1); // 计数时钟 = 1MHz
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;               // 向上计数
    TIM_TimeBaseStructure.TIM_Period = (uint16_t)(1000 - 1);                  // 1kHz → 1ms
    TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;                   // 1分频
    TIM_TimeBaseStructure.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(TIM5, &TIM_TimeBaseStructure);
    TIM_ITConfig(TIM5, TIM_IT_Update, ENABLE); /* 使能更新中断 */

    NVIC_InitTypeDef NVIC_InitStructure;
    NVIC_InitStructure.NVIC_IRQChannel = TIM5_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 2; /* 该优先级(2)高于configMAX_SYSCALL_INTERRUPT_PRIORITY(5), 因此TIM5中断内禁止调用任何FreeRTOS API, 只能做计数等简单操作 */
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE; // 使能TIM5_IRQn
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
    uint32_t cnt; // TIM5计数器值
    uint64_t ms;  // TIM5毫秒计数器值
    uint8_t uif;  // 更新事件标志位(1: 事件发生但中断尚未处理, 0: 事件未发生或中断已处理)

    do
    {
        cnt = TIM5->CNT;
        ms = TIM5_ms;
        uif = (TIM5->SR & TIM_SR_UIF) != 0; // 更新事件已发生但中断尚未处理
    } while (ms != TIM5_ms); /* ms在采样期间被中断更新则重读 */

    if (cnt < 256U) /* CNT接近回绕起点, 复检ms是否已进位 */
    {
        uint64_t ms2 = TIM5_ms;
        if (ms2 != ms)
            ms = ms2; // 已进入下一毫秒, 当前cnt属于ms2
    }

    /* CNT已回绕、但更新中断还没执行时, 上面的ms还是回绕前的值, 结果会比实际
     * 小1000us(时间倒流)。此时UIF仍置位, 据此补上这一毫秒, 保证本函数单调不减。 */
    if (uif && cnt < 500U)
        ms++;

    return ms * 1000U + cnt;
}

void delay_us(uint32_t us)
{
    uint64_t start = TIM5_Get_us();

    /* 用有符号差比较而不是无符号: 采样值一旦出现回退, 无符号相减会下溢成巨大值,
     * 循环条件立刻不成立而提前返回(实测会把DS1302的SCLK半周期压到200ns以下,
     * 表现为随机位错误)。有符号比较会把它看成负的"已过时间", 继续等待。 */
    while ((int64_t)(TIM5_Get_us() - start) < (int64_t)us)
        ;
}

void delay_ms(uint32_t ms)
{
    uint64_t now = TIM5_Get_ms();
    while (TIM5_Get_ms() - now < (uint64_t)ms)
        ;
}
