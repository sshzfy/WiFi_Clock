#include "DHT22.h"
#include "FreeRTOS.h"
#include "task.h"

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
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP; /* 输入+内部上拉: 总线空闲为高, 抗噪 */
    GPIO_Init(DHT22_Port, &GPIO_InitStruct);
}

bool DHT22_Init(void)
{
    DHT22_GPIO_Output();
    DHT22_DATA_OUT_H;

    return true;
}

/* 等待总线变为高电平; 超时(max_us)返回1 */
static uint8_t DHT22_Wait_For_Set(uint32_t max_us)
{
    uint32_t t = 0;

    while (DHT22_READ_DATA == RESET)
    {
        if (++t >= max_us)
            return 1;
        delay_us(1);
    }
    return 0;
}

/* 等待总线变为低电平; 超时(max_us)返回1 */
static uint8_t DHT22_Wait_For_Reset(uint32_t max_us)
{
    uint32_t t = 0;

    while (DHT22_READ_DATA == SET)
    {
        if (++t >= max_us)
            return 1;
        delay_us(1);
    }
    return 0;
}

static uint8_t DHT22_ReadByte(uint8_t *out)
{
    uint8_t data = 0;

    for (uint8_t i = 0; i < 8; i++)
    {
        if (DHT22_Wait_For_Set(300) != 0)   /* 等位起始(低电平结束) */
            return DHT22_ERR_TIMEOUT;
        delay_us(30);                        /* 采样窗口 */
        if (DHT22_READ_DATA == SET)
            data |= (uint8_t)(1U << (7 - i));
        if (DHT22_Wait_For_Reset(300) != 0)  /* 等位结束(回到低) */
            return DHT22_ERR_TIMEOUT;
    }

    *out = data;
    return DHT22_OK;
}

static uint8_t DHT22_ReadAttempt(DHT22_Data_t *data)
{
    uint8_t buf[5];
    uint8_t code;

    DHT22_GPIO_Output();
    DHT22_DATA_OUT_L;
    vTaskDelay(pdMS_TO_TICKS(2)); /* 起始信号: 拉低≥1ms */

    DHT22_DATA_OUT_H;
    DHT22_GPIO_Input();
    delay_us(30);

    if (DHT22_Wait_For_Reset(200) != 0) /* 等80us应答(拉低) */
        return DHT22_ERR_NO_ACK;
    if (DHT22_Wait_For_Set(200) != 0)   /* 应答结束(拉高) */
        return DHT22_ERR_NO_HIGH;
    if (DHT22_Wait_For_Reset(200) != 0) /* 数据起始(拉低) */
        return DHT22_ERR_NO_LOW;

    for (uint8_t i = 0; i < 5; i++)
    {
        code = DHT22_ReadByte(&buf[i]);
        if (code != DHT22_OK)
            return code;
    }

    /* 校验和 */
    if ((uint8_t)(buf[0] + buf[1] + buf[2] + buf[3]) != buf[4])
        return DHT22_ERR_CHECKSUM;

    /* 解析数据 */
    uint16_t humidity_raw = (uint16_t)((uint16_t)(buf[0] << 8) | buf[1]); // 湿度原始数据
    data->humidity = (float)(humidity_raw / 10.0f);
    uint16_t temp_raw = (uint16_t)((uint16_t)(buf[2] << 8) | buf[3]);     // 温度原始数据
    if (temp_raw & 0x8000U)
    {
        temp_raw &= 0x7FFFU;
        data->temperature = -(float)(temp_raw / 10.0f);
    }
    else
    {
        data->temperature = (float)(temp_raw / 10.0f);
    }

    return DHT22_OK;
}

uint8_t DHT22_ReadData(DHT22_Data_t *data)
{
    uint8_t code = DHT22_ReadAttempt(data);

    if (code == DHT22_OK)
        return DHT22_OK;

    vTaskDelay(pdMS_TO_TICKS(50)); /* 间隔后重试一次, 容忍瞬时干扰 */
    return DHT22_ReadAttempt(data);
}

const char *DHT22_ErrString(uint8_t code)
{
    switch (code)
    {
        case DHT22_OK:           return "OK";
        case DHT22_ERR_NO_ACK:   return "no-ack";
        case DHT22_ERR_NO_HIGH:  return "no-high";
        case DHT22_ERR_NO_LOW:   return "no-start-low";
        case DHT22_ERR_TIMEOUT:  return "bit-timeout";
        case DHT22_ERR_CHECKSUM: return "checksum";
        default:                 return "unknown";
    }
}
