#include "Board.h"

void Board_Peripheral_Init(void)
{
    /* 使能GPIOA时钟,用于调试输出
     * 使能GPIOB时钟,备用引脚
     * 使能GPIOC时钟,LCD,LED测试
     * 使能GPIOE时钟,LCD,DHT22 */
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOE, ENABLE);
    /* 使能USART1时钟,传输AT命令
     * 使能USART2时钟,用于调试输出
     * 使能USART3时钟,用于ATGM336H定位模块 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);
    /* 使能SPI3时钟,用于与LCD通信
     * 使能SPI1时钟,用于与W25Q64通信 */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI3, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE);
    /* 使能DMA1时钟,用于SPI3/USART2 DMA传输 */
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA1, ENABLE);
    /* 使能TIM5时钟,用于替代RTC时间基准,延迟函数 */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM5, ENABLE);
    /* 使能ADC1时钟,用于光敏电阻 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);
    /* 使能CCM RAM: LVGL的24KB内存池就放在 0x10000000 */
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_CCMDATARAMEN, ENABLE);
    /* 使能SYSCFG时钟,用于外部中断配置 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);
}

void Board_Init(void)
{
    /* 测试LED,正常烧录时应亮起 */
    Test();
    /* 初始化TIM5,用于RTC时间基准 */
    TIM5_Init();
    /* 初始化USART2: 先开调试口, 后面的初始化日志才看得到 */
    Usart2_Debug_Init();
    /* 使能DWT周期计数器, 用于测量渲染耗时 */
    Prof_Init();
    /* 初始化触摸屏: SPI3 由 LCD_Init 配好, 校准参数从 /data/touch_cal.bin 读 */
    (void)Touch_Init();
    // /* 抢在 LCD 初始化之前把触摸 CS 拉高: LCD_Init() 会往 SPI3 上打大量 ST7789 初始化
    //  * 时钟, 此时触摸 CS 若还是复位后的浮空输入, XPT2046 会把这些命令当成自己的收进去,
    //  * 状态机错位后只有掉电才能恢复(见 Touch_Park 的说明) */
    // Touch_Park();
    /* 初始化LCD, 并分配图片乒乓缓冲 */
    LCD_Init();
    /* 初始化资源层: W25Q64 + littlefs, 字库/图片只读 + 运行数据读写 */
    Asset_Init();
    
    OLED_Init();
    /* 白天由LCD显示, OLED保持关闭 */
    OLED_Display_Off();
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
