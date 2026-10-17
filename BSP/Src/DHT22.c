#include "DHT22.h"

/* ================ DHT22 传感器时序 ================
 * SDA总线空闲时高电平
 * 主机将GPIO设为输出模式并拉低SDA至少 1s，主机拉高SDA并设置GPIO输入模式
 * 传感器拉低 SDA 80us 作为应答，再拉高 SDA 80us 表示准备发送数据
 * 数据位0 ：50us 低电平+ 26~28 us 高电平 ；数据位1：50us 低电平+ 70 us 高电平
 * 数据格式：湿度高字节 + 湿度低字节 + 温度高字节 + 温度低字节 + 校验和
 * ================================ */

/* ================ static 函数 ================ */
static inline void DHT22_DATA_OUT_H(void) { GPIO_SetBits(DHT22_PORT, DHT22_PIN); }
static inline void DHT22_DATA_OUT_L(void) { GPIO_ResetBits(DHT22_PORT, DHT22_PIN); }
static inline uint8_t DHT22_READ_DATA(void) { return GPIO_ReadInputDataBit(DHT22_PORT, DHT22_PIN); }
static void DHT22_GPIO_Output(void);
static void DHT22_GPIO_Input(void);
static uint8_t DHT22_Wait_For_Set(uint32_t max_us);
static uint8_t DHT22_Wait_For_Reset(uint32_t max_us);
static uint8_t DHT22_ReadByte(uint8_t *out);
static uint8_t DHT22_ReadAttempt(DHT22_Data_t *data);
static uint8_t DHT22_CheckAllZero(uint8_t code, const DHT22_Data_t *data);

/* 设置DHT22 GPIO为输出模式(MCU方向) */
static void DHT22_GPIO_Output(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_StructInit(&GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Pin = DHT22_PIN;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(DHT22_PORT, &GPIO_InitStruct);
}

/* 设置DHT22 GPIO为输入模式(MCU方向) */
static void DHT22_GPIO_Input(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_StructInit(&GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Pin = DHT22_PIN;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_IN;
    GPIO_InitStruct.GPIO_Speed = GPIO_High_Speed;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP; // 输入+内部上拉: 总线空闲为高, 抗噪
    GPIO_Init(DHT22_PORT, &GPIO_InitStruct);
}

/**
 * @brief 等待总线变为高电平; 超时(max_us)返回1
 * @param max_us 最大等待时间(微秒)
 * @return 0=成功, 1=超时
 */
static uint8_t DHT22_Wait_For_Set(uint32_t max_us)
{
    uint32_t t = 0;

    while (DHT22_READ_DATA() == RESET)
    {
        if (++t >= max_us)
            return 1;
        delay_us(1);
    }
    return 0;
}

/**
 * @brief 等待总线变为低电平; 超时(max_us)返回1
 * @param max_us 最大等待时间(微秒)
 * @return 0=成功, 1=超时
 */
static uint8_t DHT22_Wait_For_Reset(uint32_t max_us)
{
    uint32_t t = 0;

    while (DHT22_READ_DATA() != RESET)
    {
        if (++t >= max_us)
            return 1;
        delay_us(1);
    }
    return 0;
}

/**
 * @brief 读取一个字节
 * @param out 输出字节指针
 * @return 0=成功, 1=超时
 * @note  用TIM5测量"高电平持续时间"来判别0/1, 而不是在固定时刻采样:
 *        固定延时30us采样时, 位"0"(26~28us)的裕量只有2~5us,
 *        任何中断插入把采样点推后都有误判风险
 */
static uint8_t DHT22_ReadByte(uint8_t *out)
{
    uint8_t data = 0;

    for (uint8_t i = 0; i < 8; i++)
    {
        /* 等位起始(50us低电平结束) */
        if (DHT22_Wait_For_Set(300) != 0)
            return DHT22_ERR_TIMEOUT;

        uint64_t t_rise = TIM5_Get_us(); // 记录该位上升沿时刻

        /* 等该位高电平结束 */
        if (DHT22_Wait_For_Reset(300) != 0)
            return DHT22_ERR_TIMEOUT;

        /* 高电平宽度 > 40us 判为1, 否则判为0。
         * 用有符号比较: 采样值一旦回退, 无符号相减会下溢成巨大值而被误判为1 */
        if ((int64_t)(TIM5_Get_us() - t_rise) > (int64_t)DHT22_BIT_THRESHOLD_US)
            data |= (uint8_t)(1U << (7 - i));
    }

    *out = data;
    return DHT22_OK;
}

/**
 * @brief 读取一次DHT22传感器数据
 * @param data 输出数据结构体指针
 * @return 0=成功, 1=超时
 */
static uint8_t DHT22_ReadAttempt(DHT22_Data_t *data)
{
    uint8_t buf[5];
    uint8_t code;

    DHT22_GPIO_Output();
    DHT22_DATA_OUT_L();
    vTaskDelay(pdMS_TO_TICKS(2)); // 起始信号: 拉低≥1ms

    DHT22_DATA_OUT_H();
    DHT22_GPIO_Input();
    delay_us(30); // 等待总线稳定, 防止防抖动

    /* 等80us应答(拉低) */
    if (DHT22_Wait_For_Reset(200) != 0)
        return DHT22_ERR_NO_ACK;
    /* 应答结束(拉高) */
    if (DHT22_Wait_For_Set(200) != 0)
        return DHT22_ERR_NO_HIGH;
    /* 数据起始(拉低，数据为均是以 50us 低电平开始，以高电平持续时间区分 0 和 1 ) */
    if (DHT22_Wait_For_Reset(200) != 0)
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
    uint16_t temp_raw = (uint16_t)((uint16_t)(buf[2] << 8) | buf[3]); // 温度原始数据
    /* 温度为负数时 */
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

/**
 * @brief 把"全 0 垃圾数据"识别成失败
 * @param code 单次读取的返回码
 * @param data 单次读取的数据
 * @return 判定后的返回码
 * @note  DHT22 的 5 个字节全为 0 时, 校验和 (0+0+0+0) == 0 同样成立, 会骗过校验和这一关。
 *        但室内不可能同时是 0.0°C 且 0.0%RH(量程下限为 -40°C / 0%), 这种数据实际是
 *        "传感器没真正回数据" —— 供电不足、DATA 线接触不良, 或者位时序被打断把位"1"
 *        全判成了位"0"。当失败处理, 让下面的重试与界面的 "--" 生效
 */
static uint8_t DHT22_CheckAllZero(uint8_t code, const DHT22_Data_t *data)
{
    if (code == DHT22_OK && data->temperature == 0.0f && data->humidity == 0.0f)
        return DHT22_ERR_ALL_ZERO;

    return code;
}

/* ================ public 函数 ================ */
bool DHT22_Init(void)
{
    DHT22_GPIO_Output();
    DHT22_DATA_OUT_H();

    return true;
}

/**
 * @brief 读取一次DHT22传感器数据
 * @param data 输出数据结构体指针
 * @return 0=成功, 1=超时
 */
uint8_t DHT22_ReadData(DHT22_Data_t *data)
{
    uint8_t code = DHT22_CheckAllZero(DHT22_ReadAttempt(data), data);

    if (code == DHT22_OK)
        return DHT22_OK;

    vTaskDelay(pdMS_TO_TICKS(50)); // 间隔后重试一次, 容忍瞬时干扰
    return DHT22_CheckAllZero(DHT22_ReadAttempt(data), data);
}

/**
 * @brief 获取DHT22传感器错误码字符串
 * @param code 错误码
 * @return 错误码字符串指针
 * @note 错误码字符串为ASCII码, 不包含换行符
 */
const char *DHT22_ErrString(uint8_t code)
{
    switch (code)
    {
    case DHT22_OK:
        return "OK";
    case DHT22_ERR_NO_ACK:
        return "no-ack";
    case DHT22_ERR_NO_HIGH:
        return "no-high";
    case DHT22_ERR_NO_LOW:
        return "no-start-low";
    case DHT22_ERR_TIMEOUT:
        return "bit-timeout";
    case DHT22_ERR_CHECKSUM:
        return "checksum";
    case DHT22_ERR_ALL_ZERO:
        return "all-zero";
    default:
        return "unknown";
    }
}
