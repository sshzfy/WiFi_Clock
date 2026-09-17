#include "Key.h"

/* ================= Key 用于通知任务 =================
 * 按键已有硬件消抖
 * 按下与松开均为边沿触发, 手势(短按/长按/连击)由 RTOS 层按键任务识别
 * ================================== */

static Key_IRQ_Callbacks_t s_irq_cb = NULL;

void Key_RegisterCallback(Key_IRQ_Callbacks_t callback)
{
    s_irq_cb = callback;
}

static inline void Key_Notify(void)
{
    if (s_irq_cb != NULL)
        s_irq_cb();
}

static void Key_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_StructInit(&GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Pin = KEY_GPIO_PIN;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_IN;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_DOWN; // 下拉: 空闲为低, 按下为高

    GPIO_Init(KEY_GPIO_PORT, &GPIO_InitStruct);
}

static void Key_NVIC_Init(void)
{
    NVIC_InitTypeDef NVIC_InitStruct;

    NVIC_InitStruct.NVIC_IRQChannel = KEY_EXTI_IRQn;       // NVIC 通道
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 5; // =configMAX_SYSCALL: 允许FromISR
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;

    NVIC_Init(&NVIC_InitStruct);
}

static void Key_EXTI_Init(void)
{
    EXTI_InitTypeDef EXTI_InitStruct;
    EXTI_StructInit(&EXTI_InitStruct);

    /* 端口源必须用 EXTI_PortSourceGPIOx(NVIC 通道另行配置) */
    SYSCFG_EXTILineConfig(KEY_EXTI_PORT_SOURCE, KEY_EXTI_PIN_SOURCE);

    EXTI_InitStruct.EXTI_Line = KEY_EXTI_LINE;
    EXTI_InitStruct.EXTI_Mode = EXTI_Mode_Interrupt;            // 中断模式
    EXTI_InitStruct.EXTI_Trigger = EXTI_Trigger_Rising_Falling; // 按下/松开都通知
    EXTI_InitStruct.EXTI_LineCmd = ENABLE;

    EXTI_Init(&EXTI_InitStruct);
}

void Key_Init(void)
{
    Key_GPIO_Init();
    Key_EXTI_Init();
    Key_NVIC_Init();
}

/** @brief 读取按键当前电平
 *  @return true=按下, false=松开
 */
bool Key_IsPressed(void)
{
    return (GPIO_ReadInputDataBit(KEY_GPIO_PORT, KEY_GPIO_PIN) == Bit_SET);
}

void KEY_EXTI_IRQHandler(void)
{
    if (EXTI_GetITStatus(KEY_EXTI_LINE) == SET)
    {
        EXTI_ClearITPendingBit(KEY_EXTI_LINE);

        Key_Notify(); /* ISR 内只通知任务, 手势识别在任务上下文完成 */
    }
}
