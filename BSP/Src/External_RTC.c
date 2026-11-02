#include "External_RTC.h"

/* ================ DS1302 协议规范 ==================
 * - 命令字节；bit7 必须为 1，bit6 0/1（时钟/日历），
 *   bit5 ~ 1 寄存器地址，bit0 0/1（写/读）
 * - 写数据：数据在 SCLK 上升沿有效输入。
 *   读数据：数据在 SCLK 下降沿输出。
 *          命令最后一位写完后第一个数据位出现在第一个 SCLK 下降沿。
 *   数据字节和命令字节均 LSB 先传。
 * - 上电时，RST/CE 必须为 0
 * - CE/RST 拉高开始传送，拉低终止
 *   当 RST/CE 拉高时，SCLK 必须为拉低。
 * ================================*/

#define DS1302_DELAY_US 10U  // 10us 延时
#define DS1302_READ_RETRY 3U // 3次重试, 每次间隔10us

static inline void DS1302_CLK_HIGH(void) { GPIO_SetBits(DS1302_PORT, DS1302_CLK_PIN); }
static inline void DS1302_CLK_LOW(void) { GPIO_ResetBits(DS1302_PORT, DS1302_CLK_PIN); }
static inline void DS1302_RST_HIGH(void) { GPIO_SetBits(DS1302_PORT, DS1302_RST_PIN); }
static inline void DS1302_RST_LOW(void) { GPIO_ResetBits(DS1302_PORT, DS1302_RST_PIN); }
static void DS1302_GPIO_Init(void);
static void DS1302_GPIO_OUT(void);
static void DS1302_GPIO_IN(void);
static inline void DS1302_IO_Write(uint8_t bit);
static inline uint8_t DS1302_IO_Read(void);
static void DS1302_WriteByte(uint8_t data);
static uint8_t DS1302_ReadByte(void);
static void DS1302_WriteReg(uint8_t cmd, uint8_t data);
// static uint8_t DS1302_ReadReg(uint8_t cmd);
static uint8_t Bcd2Dec(uint8_t value);
static uint8_t Dec2Bcd(uint8_t value);
static void DS1302_BurstRead(uint8_t raw[8]);
static bool DS1302_ReadOnce(DS1302_Time_t *time);

/** @brief 初始化DS1302 GPIO引脚 */
static void DS1302_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;

    GPIO_StructInit(&GPIO_InitStruct);
    /* RST / SCLK 推挽输出 */
    GPIO_InitStruct.GPIO_Pin = DS1302_RST_PIN | DS1302_CLK_PIN;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(DS1302_PORT, &GPIO_InitStruct);

    /* IO 默认输出, 后面读时设为输入 */
    GPIO_InitStruct.GPIO_Pin = DS1302_IO_PIN;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_OD;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(DS1302_PORT, &GPIO_InitStruct);

    GPIO_ResetBits(DS1302_PORT, DS1302_RST_PIN); // 空闲时 RST 为低电平
    GPIO_ResetBits(DS1302_PORT, DS1302_CLK_PIN); // 空闲时 SCLK 为低电平
    GPIO_SetBits(DS1302_PORT, DS1302_IO_PIN);
}

/** @brief 设置DS1302 IO引脚为输出模式 */
static void DS1302_GPIO_OUT(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;

    GPIO_StructInit(&GPIO_InitStruct);
    GPIO_InitStruct.GPIO_Pin = DS1302_IO_PIN;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_OD;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(DS1302_PORT, &GPIO_InitStruct);
}

/** @brief 设置DS1302 IO引脚为输入模式 */
static void DS1302_GPIO_IN(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;

    GPIO_StructInit(&GPIO_InitStruct);
    GPIO_InitStruct.GPIO_Pin = DS1302_IO_PIN;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_IN;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_OD;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(DS1302_PORT, &GPIO_InitStruct);
}

/** @brief 写入DS1302 IO引脚
 *  @param bit 要写入的高低电平位
 */
static inline void DS1302_IO_Write(uint8_t bit)
{
    if (bit)
        GPIO_SetBits(DS1302_PORT, DS1302_IO_PIN);
    else
        GPIO_ResetBits(DS1302_PORT, DS1302_IO_PIN);
}

/** @brief 从DS1302 IO引脚读取数据
 *  @return 读取到的高低电平位
 */
static inline uint8_t DS1302_IO_Read(void)
{
    return GPIO_ReadInputDataBit(DS1302_PORT, DS1302_IO_PIN);
}

/**
 * @brief 写入一个字节到DS1302, 低位先写
 * @param data 要写入的字节
 */
static void DS1302_WriteByte(uint8_t data)
{
    DS1302_GPIO_OUT();

    for (uint8_t i = 0; i < 8; i++)
    {
        DS1302_IO_Write(data & 0x01);
        delay_us(DS1302_DELAY_US);
        DS1302_CLK_HIGH();
        delay_us(DS1302_DELAY_US);
        DS1302_CLK_LOW();
        delay_us(DS1302_DELAY_US);

        data >>= 1;
    }
}

/**
 * @brief 从DS1302读取一个字节, 低位先读
 * @return 读取到的字节
 */
static uint8_t DS1302_ReadByte(void)
{
    uint8_t data = 0;

    DS1302_GPIO_IN();

    for (uint8_t i = 0; i < 8; i++)
    {
        data >>= 1;

        /* 先读入引脚数据，再拉高 SCLK */
        if (DS1302_IO_Read())
            data |= 0x80U;
        DS1302_CLK_HIGH();
        delay_us(DS1302_DELAY_US);
        DS1302_CLK_LOW();
        delay_us(DS1302_DELAY_US);
    }

    return data;
}

/**
 * @brief 写入DS1302寄存器
 * @param cmd 寄存器地址
 * @param data 要写入的数据
 */
static void DS1302_WriteReg(uint8_t cmd, uint8_t data)
{
    DS1302_RST_LOW();
    DS1302_CLK_LOW();
    delay_us(DS1302_DELAY_US);

    DS1302_RST_HIGH();
    delay_us(DS1302_DELAY_US);

    DS1302_WriteByte(cmd);
    DS1302_WriteByte(data);

    DS1302_RST_LOW();
    delay_us(DS1302_DELAY_US);
}

// static uint8_t DS1302_ReadReg(uint8_t cmd)
// {
//     uint8_t data = 0;

//     DS1302_RST_LOW();
//     DS1302_CLK_LOW();
//     delay_us(DS1302_DELAY_US);

//     DS1302_RST_HIGH();
//     delay_us(DS1302_DELAY_US);

//     DS1302_WriteByte(cmd);
//     data = DS1302_ReadByte();

//     DS1302_RST_LOW();
//     delay_us(DS1302_DELAY_US);

//     return data;
// }

/* ================ BCD 转换 ==================*/
static uint8_t Bcd2Dec(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10) + (value & 0x0F));
}

static uint8_t Dec2Bcd(uint8_t value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

/* ================ 初始化 ==================*/
void DS1302_Init(void)
{
    DS1302_GPIO_Init();
    DS1302_WriteReg(DS1302_REG_WP, 0x80); // 开启写保护
}

/**
 * @brief 突发读取 8 字节寄存器
 * @param raw 至少 8 字节的输出缓冲, 顺序: 秒 分 时 日 月 周 年 WP
 */
static void DS1302_BurstRead(uint8_t raw[8])
{
    if (raw == NULL)
        return;

    DS1302_RST_LOW();
    DS1302_CLK_LOW();
    delay_us(DS1302_DELAY_US);

    DS1302_RST_HIGH();
    delay_us(DS1302_DELAY_US);

    /* 时钟突发读取 */
    DS1302_WriteByte(0xBF);

    for (uint8_t i = 0; i < 8; i++)
        raw[i] = DS1302_ReadByte();

    DS1302_RST_LOW();
    delay_us(DS1302_DELAY_US);
}

/**
 * @brief 读一次DS1302时间(突发读 + 合法性判断)
 * @param time 存储读取到的时间结构体指针
 * @return true 成功,false 失败
 */
static bool DS1302_ReadOnce(DS1302_Time_t *time)
{
    uint8_t buf[8];

    if (time == NULL)
        return false;

    DS1302_BurstRead(buf);

    /* 秒寄存器 bit7 = CH(Clock Halt)。CH=1 表示振荡器已停, 读取到的时间无效。 */
    if (buf[0] & 0x80)
        return false;

    time->sec = Bcd2Dec(buf[0] & 0x7F);
    time->min = Bcd2Dec(buf[1] & 0x7F);
    /* 小时寄存器: bit7=1 为12小时制(bit5=AM/PM, bit4~0=1~12);
     *            bit7=0 为24小时制(bit5~0=00~23)。
     * SetTime 写 24 小时制, 但芯片可能因历史配置处于 12 小时制, 这里统一转成 24 小时。 */
    if (buf[2] & 0x80)
    {
        uint8_t h = Bcd2Dec(buf[2] & 0x1F);
        bool is_pm = (buf[2] & 0x20) != 0;
        if (h == 12)
            h = 0;
        else if (is_pm)
            h = (uint8_t)(h + 12);
        time->hour = h;
    }
    else
    {
        time->hour = Bcd2Dec(buf[2] & 0x3F); // 24小时制
    }
    time->day = Bcd2Dec(buf[3] & 0x3F);
    time->month = Bcd2Dec(buf[4] & 0x1F);
    time->week = Bcd2Dec(buf[5] & 0x07);
    time->year = (uint16_t)(2000 + Bcd2Dec(buf[6]));

    /* 合法性判断 */
    if (time->year < 2000 || time->year > 2099)
        return false;
    if (time->month < 1 || time->month > 12)
        return false;
    if (time->day < 1 || time->day > 31)
        return false;
    if (time->hour > 23)
        return false;
    if (time->min > 59)
        return false;
    if (time->sec > 59)
        return false;
    if (time->week < 1 || time->week > 7)
        return false;

    return true;
}

/**
 * @brief 从DS1302读取时间(连读两次比对)
 * @param time 存储读取到的时间结构体指针
 * @return true 成功，false 失败
 */
bool DS1302_ReadTime(DS1302_Time_t *time)
{
    DS1302_Time_t a, b;

    if (time == NULL)
        return false;

    for (uint8_t retry = 0; retry < DS1302_READ_RETRY; retry++)
    {
        if (!DS1302_ReadOnce(&a))
            continue;
        if (!DS1302_ReadOnce(&b))
            continue;

        /* 不比较秒: 两次读之间可能跨秒 */
        if ((a.min != b.min) || (a.hour != b.hour) || (a.day != b.day) ||
            (a.month != b.month) || (a.week != b.week) || (a.year != b.year))
            continue;

        *time = a;
        return true;
    }

    return false;
}

/**
 * @brief 设置DS1302时间
 * @param time 要设置的时间结构体指针
 * @return true 成功,false 失败
 */
bool DS1302_SetTime(const DS1302_Time_t *time)
{
    if (time == NULL)
        return false;

    /* 输入范围校验: 越界值经 Dec2Bcd 会生成非法 BCD, DS1302 行为未定义 */
    if (time->year < 2000 || time->year > 2099)
        return false;
    if (time->month < 1 || time->month > 12)
        return false;
    if (time->day < 1 || time->day > 31)
        return false;
    if (time->hour > 23)
        return false;
    if (time->min > 59)
        return false;
    if (time->sec > 59)
        return false;
    if (time->week < 1 || time->week > 7)
        return false;

    /* 解除写保护 */
    DS1302_WriteReg(0x8E, 0x00);

    DS1302_RST_LOW();
    DS1302_CLK_LOW();
    delay_us(DS1302_DELAY_US);

    DS1302_RST_HIGH();
    delay_us(DS1302_DELAY_US);

    /* 时钟突发写入 */
    DS1302_WriteByte(0xBE);

    DS1302_WriteByte(Dec2Bcd(time->sec) & 0x7F); // CH=0，启动
    DS1302_WriteByte(Dec2Bcd(time->min) & 0x7F);
    DS1302_WriteByte(Dec2Bcd(time->hour) & 0x3F); // 24小时制
    DS1302_WriteByte(Dec2Bcd(time->day) & 0x3F);
    DS1302_WriteByte(Dec2Bcd(time->month) & 0x1F);
    DS1302_WriteByte(Dec2Bcd(time->week) & 0x07);
    DS1302_WriteByte(Dec2Bcd((uint8_t)(time->year % 100)));
    DS1302_WriteByte(0x00); // 第8字节是写保护寄存器，突发写时通常写 0

    DS1302_RST_LOW();
    delay_us(DS1302_DELAY_US);

    /* 恢复写保护 */
    DS1302_WriteReg(0x8E, 0x80);

    return true;
}
