#ifndef __LIGHT_SENSOR_H__
#define __LIGHT_SENSOR_H__

#include "main.h"
#include "stm32f4xx.h"
#include "stm32f4xx_adc.h"

#define LIGHT_SENSOR_GPIO_PORT GPIOA
#define LIGHT_SENSOR_GPIO_PIN GPIO_Pin_0

void Light_Sensor_Init(void);
uint16_t Light_Sensor_Read(void);

#endif /* __LIGHT_SENSOR_H__ */
