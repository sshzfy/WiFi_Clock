#include "DHT22.h"
#include "delay.h"

static void DHT22_GPIO_Output(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_StructInit(&GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Pin = DHT22_Pin;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStruct.GPIO_Speed = GPIO_High_Speed;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(DHT22_Port, &GPIO_InitStruct);
}

static void DHT22_GPIO_Input(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_StructInit(&GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Pin = DHT22_Pin;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_IN;
    GPIO_InitStruct.GPIO_Speed = GPIO_High_Speed;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(DHT22_Port, &GPIO_InitStruct);
}

bool DHT22_Init(void)
{
    DHT22_GPIO_Output();
    DHT22_DATA_OUT_H;

    return true;
}

static uint8_t DHT22_ReadByte(void)
{
    uint8_t data = 0;

    for (uint8_t i = 0; i < 8; i++)
    {
        while (DHT22_READ_DATA == RESET)
            ;
        delay_us(30);
        if (DHT22_READ_DATA == SET)
            data |= (1 << (7 - i));
        while (DHT22_READ_DATA == SET)
            ;
    }
    return data;
}

/**
 * @brief 读取DHT22传感器数据
 *
 * @param data 指向存储数据的结构体指针
 * @return uint8_t 0:成功 1:失败
 */
uint8_t DHT22_ReadData(DHT22_Data_t *data)
{
    uint8_t buf[5];
    uint32_t timeout = 0;

    DHT22_GPIO_Output();
    DHT22_DATA_OUT_L;
    delay_ms(2);

    DHT22_DATA_OUT_H;
    DHT22_GPIO_Input();
    delay_us(30);

    /* 等待总线拉低80us的应答信号 */
    while (DHT22_READ_DATA == SET)
    {
        if (timeout++ > 100)
            return 1;
        delay_us(1);
    }

    /* 等待总线拉高80us通知外设准备接收数据 */
    timeout = 0;
    while (DHT22_READ_DATA == RESET)
    {
        if (timeout++ > 100)
            return 1;
        delay_us(1);
    }

    /* 等待总线拉低,准备接收数据 */
    timeout = 0;
    while (DHT22_READ_DATA == SET)
    {
        if (timeout++ > 100)
            return 1;
        delay_us(1);
    }

    /* 接收40位数据(5 byte) */
    for (uint8_t i = 0; i < 5; i++)
    {
        buf[i] = DHT22_ReadByte();
    }

    /* 校验数据 */
    uint8_t checksum = buf[0] + buf[1] + buf[2] + buf[3];

    if (checksum != buf[4])
        return 1;

    /* 解析数据 */
    uint16_t humidity_raw = (buf[0] << 8) | buf[1]; // 湿度原始数据
    data->humidity = (float)(humidity_raw / 10.0f);
    uint16_t temp_raw = (buf[2] << 8) | buf[3]; // 温度原始数据
    if (temp_raw & 0x8000)
    {
        temp_raw &= 0x7FFF;
        data->temperature = -(float)(temp_raw / 10.0f);
    }
    else
    {
        data->temperature = (float)(temp_raw / 10.0f);
    }

    return 0;
}
