#ifndef __LIGHT_SENSOR_H__
#define __LIGHT_SENSOR_H__

#include "main.h"
#include "stm32f4xx.h"
#include "stm32f4xx_adc.h"
#include "stm32f4xx_exti.h"

/* AO/DO 选择: 1 = 模拟量 AO(PA0/ADC1_IN0, 软件阈值判断)
 *             0 = 数字量 DO(PA1/EXTI1, 板载电位器调阈值) */
#define AO_DO_SWITCH 0

/* AO 输出引脚 */
#define LIGHT_SENSOR_GPIO_PORT              GPIOA
#define LIGHT_SENSOR_GPIO_PIN               GPIO_Pin_0

/* AO 阈值(仅在 AO_DO_SWITCH==1 时使用), 需用裸机 case2 读原始值标定
 * LIGHT_SENSOR_AO_DARK_HIGH: 1=越暗ADC值越大; 0=越暗ADC值越小(极性取决于分压接法) */
#define LIGHT_SENSOR_AO_DARK_HIGH           1     /* 1=越暗ADC值越大; 0=越暗ADC值越小 */
#define LIGHT_SENSOR_AO_DARK_TH             3000  /* 进入“暗”的阈值(极端侧) */
#define LIGHT_SENSOR_AO_LIGHT_TH            2000  /* 回到“亮”的阈值(滞回, 防抖动) */

/* DO 配置 */
#define LIGHT_SENSOR_DO_GPIO_PORT           GPIOA
#define LIGHT_SENSOR_DO_GPIO_PIN            GPIO_Pin_1
#define LIGHT_SENSOR_DO_EXTI_LINE           EXTI_Line1
#define LIGHT_SENSOR_DO_EXTI_IRQn           EXTI1_IRQn
#define LIGHT_SENSOR_DO_EXTI_IRQHandler     EXTI1_IRQHandler

/* ================DO 引脚输出状态 ================
 * 光照强度达到设定值,DO输出低电平,光照强度达不到设定值,DO输出高电平
 * 阈值通过电位移调节,顺时针调节检测亮度增强
 * ================================ */

typedef enum
{
    LIGHT_SENSOR_DO_STATE_LOW = 0,
    LIGHT_SENSOR_DO_STATE_HIGH = 1,
} Light_Sensor_DO_State_t;

extern volatile Light_Sensor_DO_State_t Light_Sensor_DO_State;

/* AO/DO 公用函数 */
void Light_Sensor_Init(void);
uint16_t Light_Sensor_Read(void);

/* 统一判定: 当前是否处于“暗”(需稳定去抖由调用方负责)
 * DO模式: DO为高(未达光照阈值)=暗; AO模式: 采样+滞回 */
bool Light_Sensor_IsDark(void);

#endif /* __LIGHT_SENSOR_H__ */
