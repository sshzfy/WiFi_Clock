#include "I2C.h"

/* ================ Soft I2C时序 ================
 * 数据变化：SCL 为低电平时，数据变化
 * 数据采样：SCL 为高电平时，数据采样，SDA 保持
 * start：SCL 为高电平时，SDA 由高电平向低电平跳变
 * stop：SCL 为高电平时，SDA 由低电平向高电平跳变
 * ack：第九个 SCL 周期，SDA 由高电平向低电平跳变
 * nack：第九个 SCL 周期，SDA 保持高电平
 * 主机发送数据由从机应答 ack 确认
 * 主机接受最后一个字节后发送 nack，通知从机数据接收完成
 * ================================ */

void Soft_I2C_Init(Soft_I2C_t *Soft_I2C)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_StructInit(&GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_OD;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;

    GPIO_InitStruct.GPIO_Pin = Soft_I2C->SCL_Pin;
    GPIO_Init(Soft_I2C->SCL_Port, &GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Pin = Soft_I2C->SDA_Pin;
    GPIO_Init(Soft_I2C->SDA_Port, &GPIO_InitStruct);

    Soft_I2C_SDA_HIGH(Soft_I2C);
    Soft_I2C_SCL_HIGH(Soft_I2C);
    delay_us(5);
}

static void Soft_I2C_Start(Soft_I2C_t *Soft_I2C)
{
    Soft_I2C_SDA_HIGH(Soft_I2C);
    Soft_I2C_SCL_HIGH(Soft_I2C);
    delay_us(5);
    Soft_I2C_SDA_LOW(Soft_I2C);
    delay_us(5);
    Soft_I2C_SCL_LOW(Soft_I2C);
    delay_us(5);
}

static void Soft_I2C_Stop(Soft_I2C_t *Soft_I2C)
{
    Soft_I2C_SDA_LOW(Soft_I2C);
    delay_us(5);
    Soft_I2C_SCL_HIGH(Soft_I2C);
    delay_us(5);
    Soft_I2C_SDA_HIGH(Soft_I2C);
    delay_us(5);
}

static uint8_t Soft_I2C_Wait_Ack(Soft_I2C_t *Soft_I2C)
{
    uint8_t ack = 0;

    Soft_I2C_SDA_HIGH(Soft_I2C);
    delay_us(5);
    Soft_I2C_SCL_HIGH(Soft_I2C);
    delay_us(5);
    ack = GPIO_ReadInputDataBit(Soft_I2C->SDA_Port, Soft_I2C->SDA_Pin);
    delay_us(5);
    Soft_I2C_SCL_LOW(Soft_I2C);
    delay_us(5);

    return ack;
}

static void Soft_I2C_Send_ACK(Soft_I2C_t *Soft_I2C)
{
    Soft_I2C_SDA_LOW(Soft_I2C);
    delay_us(5);
    Soft_I2C_SCL_HIGH(Soft_I2C);
    delay_us(5);
    Soft_I2C_SCL_LOW(Soft_I2C);
    delay_us(5);
    Soft_I2C_SDA_HIGH(Soft_I2C); // 释放SDA
}

static void Soft_I2C_Send_NACK(Soft_I2C_t *Soft_I2C)
{
    Soft_I2C_SDA_HIGH(Soft_I2C);
    delay_us(5);
    Soft_I2C_SCL_HIGH(Soft_I2C);
    delay_us(5);
    Soft_I2C_SCL_LOW(Soft_I2C);
    delay_us(5);
}

/**
 * @brief 发送一个字节
 *
 * @param Soft_I2C I2C结构体指针
 * @param data 要发送的字节
 * */
void Soft_I2C_Send_Byte(Soft_I2C_t *Soft_I2C, uint8_t data)
{
    for (uint8_t i = 0; i < 8; i++)
    {
        if (data & 0x80)
            Soft_I2C_SDA_HIGH(Soft_I2C);
        else
            Soft_I2C_SDA_LOW(Soft_I2C);

        data <<= 1;

        Soft_I2C_SCL_HIGH(Soft_I2C);
        delay_us(5);
        Soft_I2C_SCL_LOW(Soft_I2C);
        delay_us(5);
    }
}

/**
 * @brief 接收一个字节
 *
 * @param Soft_I2C I2C结构体指针
 * @param ack_flag 是否发送 ack 确认,0 表示发送 ack,1 表示发送 nack
 * @return uint8_t 接收的字节
 * */
uint8_t Soft_I2c_Receive_Byte(Soft_I2C_t *Soft_I2C, uint8_t ack_flag)
{
    uint8_t data = 0;

    Soft_I2C_SDA_HIGH(Soft_I2C);
    for (uint8_t i = 0; i < 8; i++)
    {
        data <<= 1;

        Soft_I2C_SCL_HIGH(Soft_I2C);
        delay_us(5);
        if (Soft_I2C_Read_SDA(Soft_I2C))
            data |= 0x01;
        Soft_I2C_SCL_LOW(Soft_I2C);
        delay_us(5);
    }
    if (ack_flag)
        Soft_I2C_Send_NACK(Soft_I2C);
    else
        Soft_I2C_Send_ACK(Soft_I2C);

    return data;
}

/**
 * @brief 发送多个字节
 *
 * @param Soft_I2C I2C结构体指针
 * @param dev_addr 从机地址
 * @param reg_addr 寄存器地址
 * @param data 要发送的数据指针
 * @param len 数据长度
 * @return uint8_t 发送结果,0 表示发送成功,1 表示发送失败
 * */
uint8_t Soft_I2C_Send_Bytes(Soft_I2C_t *Soft_I2C, uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, uint8_t len)
{
    Soft_I2C_Start(Soft_I2C);

    /* 发送设备地址 */
    Soft_I2C_Send_Byte(Soft_I2C, dev_addr << 1);
    if (Soft_I2C_Wait_Ack(Soft_I2C))
    {
        Soft_I2C_Stop(Soft_I2C);
        return 1;
    }
    /* 发送寄存器地址 */
    Soft_I2C_Send_Byte(Soft_I2C, reg_addr);
    if (Soft_I2C_Wait_Ack(Soft_I2C))
    {
        Soft_I2C_Stop(Soft_I2C);
        return 1;
    }
    while (len--)
    {
        Soft_I2C_Send_Byte(Soft_I2C, *data++);
        if (Soft_I2C_Wait_Ack(Soft_I2C))
        {
            Soft_I2C_Stop(Soft_I2C);
            return 1;
        }
    }
    Soft_I2C_Stop(Soft_I2C);

    return 0;
}

/**
 * @brief 接收多个字节
 *
 * @param Soft_I2C I2C结构体指针
 * @param dev_addr 从机地址
 * @param reg_addr 寄存器地址
 * @param data 接收的数据指针
 * @param len 数据长度
 * @return uint8_t 接收结果,0 表示接收成功,1 表示接收失败
 * */
uint8_t Soft_I2C_Receive_Bytes(Soft_I2C_t *Soft_I2C, uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, uint8_t len)
{
    Soft_I2C_Start(Soft_I2C);

    /* 发送设备地址,等待从机应答 */
    Soft_I2C_Send_Byte(Soft_I2C, dev_addr << 1);
    if (Soft_I2C_Wait_Ack(Soft_I2C))
    {
        Soft_I2C_Stop(Soft_I2C);
        return 1;
    }
    /* 发送寄存器地址,等待从机应答 */
    Soft_I2C_Send_Byte(Soft_I2C, reg_addr);
    if (Soft_I2C_Wait_Ack(Soft_I2C))
    {
        Soft_I2C_Stop(Soft_I2C);
        return 1;
    }

    /* 重新发送开始信号,从机地址,接收数据 */
    Soft_I2C_Start(Soft_I2C);
    Soft_I2C_Send_Byte(Soft_I2C, (dev_addr << 1) | 0x01);
    if (Soft_I2C_Wait_Ack(Soft_I2C))
    {
        Soft_I2C_Stop(Soft_I2C);
        return 1;
    }

    for (uint8_t i = 0; i < len; i++)
        *data++ = Soft_I2c_Receive_Byte(Soft_I2C, i == (len - 1) ? 1 : 0);

    Soft_I2C_Stop(Soft_I2C);

    return 0;
}
