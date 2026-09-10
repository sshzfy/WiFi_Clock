#ifndef __I2C_H__
#define __I2C_H__

#include "main.h"
#include "stm32f4xx.h"
#include "Timer.h"

typedef struct
{
    GPIO_TypeDef *SCL_Port;
    GPIO_TypeDef *SDA_Port;
    uint16_t SCL_Pin;
    uint16_t SDA_Pin;
} Soft_I2C_t;

#define Soft_I2C_SDA_HIGH(Soft_I2C) GPIO_SetBits((Soft_I2C)->SDA_Port, (Soft_I2C)->SDA_Pin)
#define Soft_I2C_SDA_LOW(Soft_I2C) GPIO_ResetBits((Soft_I2C)->SDA_Port, (Soft_I2C)->SDA_Pin)
#define Soft_I2C_SCL_HIGH(Soft_I2C) GPIO_SetBits((Soft_I2C)->SCL_Port, (Soft_I2C)->SCL_Pin)
#define Soft_I2C_SCL_LOW(Soft_I2C) GPIO_ResetBits((Soft_I2C)->SCL_Port, (Soft_I2C)->SCL_Pin)
#define Soft_I2C_Read_SDA(Soft_I2C) GPIO_ReadInputDataBit((Soft_I2C)->SDA_Port, (Soft_I2C)->SDA_Pin)

void Soft_I2C_Init(Soft_I2C_t *Soft_I2C);
void Soft_I2C_Send_Byte(Soft_I2C_t *Soft_I2C, uint8_t data);
uint8_t Soft_I2c_Receive_Byte(Soft_I2C_t *Soft_I2C, uint8_t ack_flag);
uint8_t Soft_I2C_Send_Bytes(Soft_I2C_t *Soft_I2C, uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, uint8_t len);
uint8_t Soft_I2C_Receive_Bytes(Soft_I2C_t *Soft_I2C, uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, uint8_t len);

#endif /* __I2C_H__ */
