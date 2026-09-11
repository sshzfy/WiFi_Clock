#include "Light_Sensor.h"

volatile Light_Sensor_DO_State_t Light_Sensor_DO_State = LIGHT_SENSOR_DO_STATE_LOW;

#if AO_DO_SWITCH == 1

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

static void Light_Sensor_ADC_Init(void)
{
    ADC_CommonInitTypeDef ADC_CommonInitStruct;
    ADC_InitTypeDef ADC_InitStruct;
    ADC_StructInit(&ADC_InitStruct);

    ADC_CommonInitStruct.ADC_Mode = ADC_Mode_Independent;                     // 独立模式
    ADC_CommonInitStruct.ADC_Prescaler = ADC_Prescaler_Div4;                  // 4分频,84M/4=21M,最大21M
    ADC_CommonInitStruct.ADC_DMAAccessMode = ADC_DMAAccessMode_Disabled;      // 不使能DMA模式
    ADC_CommonInitStruct.ADC_TwoSamplingDelay = ADC_TwoSamplingDelay_5Cycles; // 5个时钟周期

    ADC_CommonInit(&ADC_CommonInitStruct);

    ADC_InitStruct.ADC_Resolution = ADC_Resolution_12b;                      // 12位分辨率,最大4095
    ADC_InitStruct.ADC_ScanConvMode = DISABLE;                               // 不使能扫描转换
    ADC_InitStruct.ADC_ContinuousConvMode = DISABLE;                         // 不使能连续转换
    ADC_InitStruct.ADC_ExternalTrigConvEdge = ADC_ExternalTrigConvEdge_None; // 无外部触发转换
    ADC_InitStruct.ADC_ExternalTrigConv = ADC_ExternalTrigConv_T1_CC1;       // 外部触发转换,定时器1,通道1,此处无外部触发,随意填写
    ADC_InitStruct.ADC_DataAlign = ADC_DataAlign_Right;                      // 右对齐
    ADC_InitStruct.ADC_NbrOfConversion = 1;                                  // 1次转换

    ADC_Init(ADC1, &ADC_InitStruct);

    ADC_RegularChannelConfig(ADC1, ADC_Channel_0, 1, ADC_SampleTime_56Cycles); /* 通道0,56个时钟周期 */
    ADC_Cmd(ADC1, ENABLE);                                                     /* 使能ADC1 */
}

uint16_t Light_Sensor_Read(void)
{
    ADC_SoftwareStartConv(ADC1); /* 开始转换 */
    while (ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC) == RESET)
        ;                                /* 等待转换完成 */
    return ADC_GetConversionValue(ADC1); /* 获取转换值 */
}

void Light_Sensor_Init(void)
{
    Light_Sensor_AO_GPIO_Init();
    Light_Sensor_ADC_Init();
}

#else /* 使用数字量输出 */

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
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 5;       // 5级优先级
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

#endif /* AO_DO_SWITCH = 0 ,使用模拟量输出 */

/**
 * @brief 统一判定当前是否处于“暗”
 *        DO: 引脚为高(未达光照阈值) = 暗
 *        AO: 多次采样取平均 + 双阈值滞回
 * @return true 暗 / false 亮
 */
bool Light_Sensor_IsDark(void)
{
#if AO_DO_SWITCH == 1
    static bool dark = false;
    static bool inited = false;
    uint32_t sum = 0;

    for (uint8_t i = 0; i < 8; i++)
        sum += Light_Sensor_Read();
    uint16_t value = (uint16_t)(sum / 8);

#if LIGHT_SENSOR_AO_DARK_HIGH /* 越暗ADC值越大 */
    if (!inited || value > LIGHT_SENSOR_AO_DARK_TH)
        dark = true;
    else if (value < LIGHT_SENSOR_AO_LIGHT_TH)
        dark = false;
#else
    if (!inited || value < LIGHT_SENSOR_AO_DARK_TH)
        dark = true;
    else if (value > LIGHT_SENSOR_AO_LIGHT_TH)
        dark = false;
#endif
    inited = true;
    return dark;
#else
    return (Light_Sensor_DO_State == LIGHT_SENSOR_DO_STATE_HIGH);
#endif
}
