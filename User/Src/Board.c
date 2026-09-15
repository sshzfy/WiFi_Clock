#include "Board.h"
#include "Asset.h"
#include "Profiling.h"

void Board_Peripheral_Init(void)
{
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);  // 使能GPIOA时钟,用于调试输出
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);  // 使能GPIOB时钟,备用引脚
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);  // 使能GPIOC时钟,LCD,LED测试
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOE, ENABLE);  // 使能GPIOE时钟,LCD,DHT22
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE); // 使能USART1时钟,传输AT命令
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE); // 使能USART2时钟,用于调试输出
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI3, ENABLE);   // 使能SPI3时钟,用于与ST7789通信
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE);   // 使能SPI1时钟,用于与W25Q64通信
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA1, ENABLE);   // 使能DMA1时钟,用于SPI3/USART2 DMA传输
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM5, ENABLE);   // 使能TIM5时钟,用于替代RTC时间基准,延迟函数
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);   // 使能ADC1时钟,用于光敏电阻
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE); // 使能SYSCFG时钟,用于外部中断配置
}

void Board_Init(void)
{
    Test();              // 测试LED,正常烧录时应亮起
    TIM5_Init();         // 初始化TIM5,用于RTC时间基准
    Usart2_Debug_Init(); // 初始化USART2: 先开调试口, 后面的初始化日志才看得到
    Prof_Init();         // 使能DWT周期计数器, 用于测量渲染耗时
    ST7789_Init();       // 初始化ST7789, 并分配图片乒乓缓冲
    Asset_Init();        // 初始化资源层: W25Q64 + littlefs(字库与图片)
}

static void Test_LED_GPIO_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

#if (USE_FREERTOS == 1)
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_InitStructure.GPIO_Speed = GPIO_High_Speed;
    GPIO_Init(GPIOC, &GPIO_InitStructure);
#else
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
    GPIO_Init(GPIOB, &GPIO_InitStructure);
#endif
}

void Test(void)
{
    Test_LED_GPIO_init();
    GPIO_ResetBits(GPIOC, GPIO_Pin_5);
    GPIO_ResetBits(GPIOB, GPIO_Pin_2);
}
