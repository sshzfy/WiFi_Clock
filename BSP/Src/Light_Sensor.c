#include "Light_Sensor.h"

static void Light_Sensor_GPIO_Init(void)
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
    // ADC_InitStruct.ADC_ExternalTrigConv = ;
    ADC_InitStruct.ADC_DataAlign = ADC_DataAlign_Right; // 右对齐
    ADC_InitStruct.ADC_NbrOfConversion = 1;             // 1次转换

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
    Light_Sensor_GPIO_Init();
    Light_Sensor_ADC_Init();
}
