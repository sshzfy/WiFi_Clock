#include "Key.h"

/* ================= Key 用于通知任务 =================
 * 按键已有硬件消抖
 * 按下与松开均为边沿触发, 手势(短按/长按/连击)由 RTOS 层按键任务识别
 * ================================== */

static Key_IRQ_Callbacks_t s_irq_cb = NULL; // 任务回调函数指针

static inline void Key_Notify(void)
{
    if (s_irq_cb != NULL)
        s_irq_cb();
}

void Key_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructre;
    EXTI_InitTypeDef EXTI_InitStructre;
    NVIC_InitTypeDef NVIC_InitStructre;

    GPIO_StructInit(&GPIO_InitStructre);
    GPIO_InitStructre.GPIO_Pin = KEY_GPIO_PIN;
    GPIO_InitStructre.GPIO_Mode = GPIO_Mode_IN;
    GPIO_InitStructre.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructre.GPIO_PuPd = GPIO_PuPd_DOWN; // 下拉: 空闲为低, 按下为高
    GPIO_Init(KEY_GPIO_PORT, &GPIO_InitStructre);

    EXTI_StructInit(&EXTI_InitStructre);
    SYSCFG_EXTILineConfig(KEY_EXTI_PORT_SOURCE, KEY_EXTI_PIN_SOURCE);
    EXTI_InitStructre.EXTI_Line = KEY_EXTI_LINE;
    EXTI_InitStructre.EXTI_Mode = EXTI_Mode_Interrupt;            // 中断模式
    EXTI_InitStructre.EXTI_Trigger = EXTI_Trigger_Rising_Falling; // 按下/松开都通知
    EXTI_InitStructre.EXTI_LineCmd = ENABLE;
    EXTI_Init(&EXTI_InitStructre);

    NVIC_InitStructre.NVIC_IRQChannel = KEY_EXTI_IRQn;       // NVIC 通道
    NVIC_InitStructre.NVIC_IRQChannelPreemptionPriority = 5; // =configMAX_SYSCALL: 允许FromISR
    NVIC_InitStructre.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructre.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructre);
}

void Key_RegisterCallback(Key_IRQ_Callbacks_t callback)
{
    s_irq_cb = callback;
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
        /* ISR 内只通知任务, 手势识别在任务上下文完成 */
        Key_Notify();
    }
}
