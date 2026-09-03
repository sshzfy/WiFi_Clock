#ifndef __DHT22_H__
#define __DHT22_H__

#include "main.h"

#define DHT22_Pin GPIO_Pin_6
#define DHT22_Port GPIOE

#define DHT22_DATA_OUT_H GPIO_SetBits(DHT22_Port, DHT22_Pin)
#define DHT22_DATA_OUT_L GPIO_ResetBits(DHT22_Port, DHT22_Pin)
#define DHT22_READ_DATA GPIO_ReadInputDataBit(DHT22_Port, DHT22_Pin)

typedef struct
{
    float temperature;
    float humidity;
    bool valid;
} DHT22_Data_t;

bool DHT22_Init(void);
uint8_t DHT22_ReadData(DHT22_Data_t *data);

#endif /* __DHT22_H__ */
