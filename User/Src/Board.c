#include "Board.h"

void Board_Peripheral_Init(void)
{
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);  // 使能GPIOA时钟,用于调试输出
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);  // 使能GPIOB时钟,LCD,LED测试
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);  // 使能GPIOC时钟,LCD,LED测试
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOE, ENABLE);  // 使能GPIOE时钟,LCD,DHT22
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE); // 使能USART1时钟,传输AT命令
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE); // 使能USART2时钟,用于调试输出
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);   // 使能SPI2时钟,用于与ST7789通信
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA1, ENABLE);   // 使能DMA1时钟,用于SPI2/USART2 DMA传输
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM5, ENABLE);   // 使能TIM5时钟,用于替代RTC时间基准,延迟函数
}

void Board_Init(void)
{
    TIM5_Init();         // 初始化TIM5,用于RTC时间基准
    ST7789_Init();       // 初始化ST7789
    Test();              // 测试LED,正常烧录时应亮起
    Usart2_Debug_Init(); // 初始化USART2,用于调试输出

}

static void Test_LED_GPIO_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_InitStructure.GPIO_Speed = GPIO_High_Speed;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
    GPIO_Init(GPIOB, &GPIO_InitStructure);
}

void Test(void)
{
    Test_LED_GPIO_init();
    GPIO_ResetBits(GPIOC, GPIO_Pin_5);
    GPIO_ResetBits(GPIOB, GPIO_Pin_2);
}
