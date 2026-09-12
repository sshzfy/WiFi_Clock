#include "Light_Sensor.h"

volatile Light_Sensor_DO_State_t Light_Sensor_DO_State = LIGHT_SENSOR_DO_STATE_LOW;

/* ISR → RTOS 层回调(由 app_task 注册, 可为 NULL): ISR 内仅做通知 */
static Light_Sensor_IRQ_Callback_t s_irq_cb = NULL;

/** @brief 注册光敏传感器中断回调函数
 * @param cb 中断回调函数指针, 用于在ISR中通知任务
 */
void Light_Sensor_RegisterCallback(Light_Sensor_IRQ_Callback_t cb)
{
    s_irq_cb = cb;
}

static inline void Light_Sensor_Notify(void)
{
    if (s_irq_cb != NULL)
        s_irq_cb();
}

#if AO_DO_SWITCH == 1
/* ==================== AO: ADC + 模拟看门狗(AWD)中断 ==================== */

static volatile bool ao_dark = false; /* AWD锁存的昼夜状态 */

static void Light_Sensor_AO_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_StructInit(&GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Pin = LIGHT_SENSOR_GPIO_PIN;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_AN;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;

    GPIO_Init(LIGHT_SENSOR_GPIO_PORT, &GPIO_InitStruct);
}

/**
 * @brief 按方向装订AWD阈值窗口, 使转换值越界(离开窗口)时触发中断
 *        F4 AWD语义: 值 < LTR 或 值 > HTR 时置AWD
 * @param detect_dark true=装订"检测变暗"窗口; false=装订"检测变亮"窗口
 */
static void Light_Sensor_AWD_Arm(bool detect_dark)
{
#if LIGHT_SENSOR_AO_DARK_HIGH /* 暗 = ADC值大 */
    if (detect_dark)          /* 变暗: 值 > DARK_TH → [0, DARK_TH] */
        ADC_AnalogWatchdogThresholdsConfig(ADC1, LIGHT_SENSOR_AO_DARK_TH, 0);
    else /* 变亮: 值 < LIGHT_TH → [LIGHT_TH, 4095] */
        ADC_AnalogWatchdogThresholdsConfig(ADC1, 4095, LIGHT_SENSOR_AO_LIGHT_TH);
#else /* 暗 = ADC值小 */
    if (detect_dark) /* 变暗: 值 < DARK_TH → [DARK_TH, 4095] */
        ADC_AnalogWatchdogThresholdsConfig(ADC1, 4095, LIGHT_SENSOR_AO_DARK_TH);
    else /* 变亮: 值 > LIGHT_TH → [0, LIGHT_TH] */
        ADC_AnalogWatchdogThresholdsConfig(ADC1, LIGHT_SENSOR_AO_LIGHT_TH, 0);
#endif
}

static void Light_Sensor_ADC_Init(void)
{
    ADC_CommonInitTypeDef ADC_CommonInitStruct;
    ADC_InitTypeDef ADC_InitStruct;
    NVIC_InitTypeDef NVIC_InitStruct;
    ADC_StructInit(&ADC_InitStruct);

    ADC_CommonInitStruct.ADC_Mode = ADC_Mode_Independent;                     // 独立模式
    ADC_CommonInitStruct.ADC_Prescaler = ADC_Prescaler_Div4;                  // 84M/4=21M
    ADC_CommonInitStruct.ADC_DMAAccessMode = ADC_DMAAccessMode_Disabled;      // 不使能DMA
    ADC_CommonInitStruct.ADC_TwoSamplingDelay = ADC_TwoSamplingDelay_5Cycles; // 5个时钟周期
    ADC_CommonInit(&ADC_CommonInitStruct);

    ADC_InitStruct.ADC_Resolution = ADC_Resolution_12b;                      // 12位
    ADC_InitStruct.ADC_ScanConvMode = DISABLE;                               // 不扫描
    ADC_InitStruct.ADC_ContinuousConvMode = ENABLE;                          // 连续转换(配合AWD)
    ADC_InitStruct.ADC_ExternalTrigConvEdge = ADC_ExternalTrigConvEdge_None; // 无外部触发
    ADC_InitStruct.ADC_ExternalTrigConv = ADC_ExternalTrigConv_T1_CC1;       // 随意(未用)
    ADC_InitStruct.ADC_DataAlign = ADC_DataAlign_Right;                      // 右对齐
    ADC_InitStruct.ADC_NbrOfConversion = 1;                                  // 1次转换
    ADC_Init(ADC1, &ADC_InitStruct);

    ADC_RegularChannelConfig(ADC1, ADC_Channel_0, 1, ADC_SampleTime_56Cycles); /* 通道0 */

    /* 模拟看门狗: 单通道(IN0); 先装"免疫窗口"[0,4095]避免启动瞬间误触发 */
    ADC_AnalogWatchdogThresholdsConfig(ADC1, 4095, 0);
    ADC_AnalogWatchdogSingleChannelConfig(ADC1, ADC_Channel_0);
    ADC_AnalogWatchdogCmd(ADC1, ADC_AnalogWatchdog_SingleRegEnable);
    ADC_ITConfig(ADC1, ADC_IT_AWD, ENABLE);

    NVIC_InitStruct.NVIC_IRQChannel = ADC_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 5; /* =configMAX_SYSCALL: 允许FromISR */
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStruct);

    ADC_Cmd(ADC1, ENABLE);
}

uint16_t Light_Sensor_Read(void)
{
    while (ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC) == RESET)
        ;                                /* 连续转换模式: 等一次转换完成 */
    return ADC_GetConversionValue(ADC1); /* 读取并清EOC */
}

/**
 * @brief ADC 看门狗中断: 阈值越界 → 锁存状态 + 反向装订 + 通知任务
 */
void ADC_IRQHandler(void)
{
    if (ADC_GetITStatus(ADC1, ADC_IT_AWD) != RESET)
    {
        ADC_ClearITPendingBit(ADC1, ADC_IT_AWD);

        ao_dark = !ao_dark;             /* 越界事件对应状态翻转 */
        Light_Sensor_AWD_Arm(!ao_dark); /* 反向装订, 防止连续越界中断风暴 */
        Light_Sensor_Notify();
    }
}

void Light_Sensor_Init(void)
{
    Light_Sensor_AO_GPIO_Init();
    Light_Sensor_ADC_Init();

    ADC_SoftwareStartConv(ADC1); /* 启动连续转换 */

    /* 采样一次确定初始状态, 并按"反向"装订AWD(避免立即触发) */
    uint16_t v = Light_Sensor_Read();
#if LIGHT_SENSOR_AO_DARK_HIGH
    ao_dark = (v > LIGHT_SENSOR_AO_DARK_TH);
#else
    ao_dark = (v < LIGHT_SENSOR_AO_DARK_TH);
#endif
    Light_Sensor_AWD_Arm(!ao_dark);
}

#else /* ==================== DO: EXTI1 中断 ==================== */

static void Light_Sensor_DO_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_StructInit(&GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Pin = LIGHT_SENSOR_DO_GPIO_PIN;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_IN;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;

    GPIO_Init(LIGHT_SENSOR_DO_GPIO_PORT, &GPIO_InitStruct);
}

static void Light_Sensor_EXTI_Init(void)
{
    EXTI_InitTypeDef EXTI_InitStruct;
    EXTI_StructInit(&EXTI_InitStruct);

    SYSCFG_EXTILineConfig(EXTI_PortSourceGPIOA, EXTI_PinSource1);

    EXTI_InitStruct.EXTI_Line = LIGHT_SENSOR_DO_EXTI_LINE;      // 1号中断线
    EXTI_InitStruct.EXTI_Mode = EXTI_Mode_Interrupt;            // 中断模式
    EXTI_InitStruct.EXTI_Trigger = EXTI_Trigger_Rising_Falling; // 上升沿或下降沿触发
    EXTI_InitStruct.EXTI_LineCmd = ENABLE;                      // 使能中断线

    EXTI_Init(&EXTI_InitStruct);
}

static void Light_Sensor_NVIC_Init(void)
{
    NVIC_InitTypeDef NVIC_InitStruct;

    NVIC_InitStruct.NVIC_IRQChannel = LIGHT_SENSOR_DO_EXTI_IRQn; // 1号中断线
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 5;       // =configMAX_SYSCALL: 允许FromISR
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 0;              // 0级子优先级
    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;                 // 使能中断线

    NVIC_Init(&NVIC_InitStruct);
}

void LIGHT_SENSOR_DO_EXTI_IRQHandler(void)
{
    if (EXTI_GetITStatus(LIGHT_SENSOR_DO_EXTI_LINE) == SET)
    {
        EXTI_ClearITPendingBit(LIGHT_SENSOR_DO_EXTI_LINE);
        if (GPIO_ReadInputDataBit(LIGHT_SENSOR_DO_GPIO_PORT, LIGHT_SENSOR_DO_GPIO_PIN) == Bit_RESET)
        {
            Light_Sensor_DO_State = LIGHT_SENSOR_DO_STATE_LOW; // 光照强度达到设定值,DO输出低电平
        }
        else
        {
            Light_Sensor_DO_State = LIGHT_SENSOR_DO_STATE_HIGH; // 光照强度低于设定值,DO输出高电平
        }
        Light_Sensor_Notify(); /* 通知任务(边沿事件) */
    }
}

void Light_Sensor_Init(void)
{
    Light_Sensor_DO_GPIO_Init();
    Light_Sensor_EXTI_Init();
    Light_Sensor_NVIC_Init();

    /* 上电先读一次引脚, 初始化状态(否则上电即暗且无跳变时会误判为亮) */
    Light_Sensor_DO_State = (GPIO_ReadInputDataBit(LIGHT_SENSOR_DO_GPIO_PORT, LIGHT_SENSOR_DO_GPIO_PIN) == Bit_RESET)
                                ? LIGHT_SENSOR_DO_STATE_LOW
                                : LIGHT_SENSOR_DO_STATE_HIGH;
}

uint16_t Light_Sensor_Read(void)
{
    return 0;
}

#endif /* AO_DO_SWITCH */

/**
 * @brief 统一判定当前是否处于“暗”
 *        DO: 引脚为高(未达光照阈值) = 暗
 *        AO: 返回AWD锁存状态(无采样, 无轮询)
 * @return true 暗 / false 亮
 */
bool Light_Sensor_IsDark(void)
{
#if AO_DO_SWITCH == 1
    return ao_dark;
#else
    return (Light_Sensor_DO_State == LIGHT_SENSOR_DO_STATE_HIGH);
#endif
}
