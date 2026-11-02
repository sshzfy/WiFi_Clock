#ifndef __LIGHT_SENSOR_H__
#define __LIGHT_SENSOR_H__

#include "main.h"
#include "stm32f4xx.h"
#include "stm32f4xx_adc.h"
#include "stm32f4xx_exti.h"

/* AO/DO 选择: 1 = 模拟量 AO，0 = 数字量 DO */
#define AO_DO_SWITCH 0

/* AO 输出引脚 */
#define LIGHT_SENSOR_GPIO_PORT        GPIOC
#define LIGHT_SENSOR_GPIO_PIN         GPIO_Pin_0

/* AO 阈值(仅在 AO_DO_SWITCH==1 时使用，未标定) */
#define LIGHT_SENSOR_AO_DARK_HIGH        1     // 1=越暗ADC值越大; 0=越暗ADC值越小
#define LIGHT_SENSOR_AO_DARK_TH          3000  // 进入“暗”的阈值(极端侧)
#define LIGHT_SENSOR_AO_LIGHT_TH         2000  // 回到“亮”的阈值(滞回, 防抖动)

/* ================DO 引脚输出状态 ================
 * 光照强度达到设定值,DO输出低电平,光照强度达不到设定值,DO输出高电平
 * 阈值通过电位移调节
 * ================================ */

/* DO 配置 */
#define LIGHT_SENSOR_DO_GPIO_PORT               GPIOC
#define LIGHT_SENSOR_DO_GPIO_PIN                GPIO_Pin_1
#define LIGHT_SENSOR_DO_EXTI_PORT_SOURCE        EXTI_PortSourceGPIOC
#define LIGHT_SENSOR_DO_EXTI_PIN_SOURCE         EXTI_PinSource1
#define LIGHT_SENSOR_DO_EXTI_LINE               EXTI_Line1
#define LIGHT_SENSOR_DO_EXTI_IRQn               EXTI1_IRQn
#define LIGHT_SENSOR_DO_EXTI_IRQHandler         EXTI1_IRQHandler

/* DO 状态 */
typedef enum
{
    LIGHT_SENSOR_DO_STATE_LOW = 0,  // 亮状态
    LIGHT_SENSOR_DO_STATE_HIGH = 1, // 暗状态
} Light_Sensor_DO_State_t;

extern volatile Light_Sensor_DO_State_t Light_Sensor_DO_State;

/* AO/DO 公用函数 */
void Light_Sensor_Init(void);
uint16_t Light_Sensor_Read(void);
bool Light_Sensor_IsDark(void);

typedef void (*Light_Sensor_IRQ_Callback_t)(void);
void Light_Sensor_RegisterCallback(Light_Sensor_IRQ_Callback_t callback);

#endif /* __LIGHT_SENSOR_H__ */
