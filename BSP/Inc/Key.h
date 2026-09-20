#ifndef __KEY_H__
#define __KEY_H__

#include "main.h"
#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "task.h"

/* 按键: PA0, 硬件消抖, 空闲下拉(低), 按下为高 */
#define KEY_GPIO_PORT           GPIOA
#define KEY_GPIO_PIN            GPIO_Pin_0
#define KEY_EXTI_LINE           EXTI_Line0
#define KEY_EXTI_PORT_SOURCE    EXTI_PortSourceGPIOA  // SYSCFG 端口源
#define KEY_EXTI_PIN_SOURCE     EXTI_PinSource0
#define KEY_EXTI_IRQn           EXTI0_IRQn            // NVIC 通道
#define KEY_EXTI_IRQHandler     EXTI0_IRQHandler

/* 手势: 由 RTOS 层的按键任务识别后产生(本层只上报按下/松开边沿) */
typedef enum
{
    KEY_GESTURE_NONE = 0,
    KEY_GESTURE_CLICK_1, // 单击
    KEY_GESTURE_CLICK_2, // 双击
    KEY_GESTURE_CLICK_3, // 三击
    KEY_GESTURE_LONG,    // 长按(按住达到阈值即触发, 不等松开)
} Key_Gesture_t;

/* 中断回调(由 RTOS 层注册, ISR 内调用): 按下/松开边沿 */
typedef void (*Key_IRQ_Callbacks_t)(void);

void Key_Init(void);
void Key_RegisterCallback(Key_IRQ_Callbacks_t callback);
bool Key_IsPressed(void);

#endif /* __KEY_H__ */
