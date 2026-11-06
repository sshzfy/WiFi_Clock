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
     * 使能USART3时钟,用于ATGM336H定位模块(未使用)  */
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
    /* 使能CCM RAM: LVGL的24KB内存池 */
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_CCMDATARAMEN, ENABLE);
    /* 使能SYSCFG时钟,用于外部中断配置 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);
}

void Board_Init(void)
{
    /* 初始化TIM5,用于RTC时间基准 */
    TIM5_Init();
    /* 初始化USART2: 先开调试口, 后面的初始化日志才看得到 */
    Usart2_Debug_Init();
    /* 使能DWT周期计数器, 用于测量渲染耗时 */
    Prof_Init();
    /* 在 LCD_Init 之前把触摸 CS(PE0) 拉高: 否则 ST7789 初始化时钟可能被 XPT2046 误收, 状态机错位后只有断电才能恢复 */
    Touch_Park();
    /* 初始化LCD, 并分配图片乒乓缓冲 */
    LCD_Init();
    /* 初始化触摸屏: SPI3 由 LCD_Init 配好, 校准参数从 /data/touch_cal.bin 读 */
    (void)Touch_Init();
    /* 初始化资源层: W25Q64 + littlefs, 字库/图片只读 + 运行数据读写 */
    Asset_Init();
    OLED_Init();
    OLED_Display_Off();
}
