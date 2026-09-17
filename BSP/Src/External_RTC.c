#include "External_RTC.h"

/* ================ DS1302 时序 ==================
 * CE 拉高使能, 命令/数据为 LSB ,SCLK 上升沿采样输入,下降沿输出
 * ================================*/

static inline void DS1302_CLK_HIGH() { GPIO_SetBits(DS1302_PORT, DS1302_CLK_PIN); }
static inline void DS1302_CLK_LOW() { GPIO_ResetBits(DS1302_PORT, DS1302_CLK_PIN); }
static inline void DS1302_RST_HIGH() { GPIO_SetBits(DS1302_PORT, DS1302_RST_PIN); }
static inline void DS1302_RST_LOW() { GPIO_ResetBits(DS1302_PORT, DS1302_RST_PIN); }

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

    GPIO_ResetBits(DS1302_PORT, DS1302_RST_PIN);
    GPIO_ResetBits(DS1302_PORT, DS1302_CLK_PIN);
    GPIO_SetBits(DS1302_PORT, DS1302_IO_PIN);
}

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

static inline void DS1302_IO_Write(uint8_t bit)
{
    if (bit)
        GPIO_SetBits(DS1302_PORT, DS1302_IO_PIN);
    else
        GPIO_ResetBits(DS1302_PORT, DS1302_IO_PIN);
}

static inline uint8_t DS1302_IO_Read(void)
{
    return GPIO_ReadInputDataBit(DS1302_PORT, DS1302_IO_PIN);
}

/**
 * @brief 写入一个字节到DS1302, 低位先写
 *
 * @param data 要写入的字节
 */
static void DS1302_WriteByte(uint8_t data)
{
    DS1302_GPIO_OUT();

    for (uint8_t i = 0; i < 8; i++)
    {
        DS1302_IO_Write(data & 0x01);
        delay_us(10);
        DS1302_CLK_HIGH();
        delay_us(10);
        DS1302_CLK_LOW();
        delay_us(10);

        data >>= 1;
    }
}

/**
 * @brief 从DS1302读取一个字节, 低位先读
 *
 * @return 读取到的字节
 * @note 采用手册标准时序: 数据在 SCLK 下降沿由 DS1302 输出,
 *       主机在 SCLK 上升沿采样。上升沿前已有一个完整半周期用于建立,
 *       对"没有外部上拉、仅靠 MCU 内部约 40k 上拉"的场合更鲁棒。
 */
static uint8_t DS1302_ReadByte(void)
{
    uint8_t data = 0;

    DS1302_GPIO_IN();

    for (uint8_t i = 0; i < 8; i++)
    {
        data >>= 1;

        DS1302_CLK_HIGH(); /* 上升沿采样 */
        delay_us(10);
        if (DS1302_IO_Read())
            data |= 0x80U;

        DS1302_CLK_LOW(); /* 下降沿输出下一位 */
        delay_us(10);
    }

    return data;
}

static void DS1302_WriteReg(uint8_t cmd, uint8_t data)
{
    DS1302_RST_LOW();
    DS1302_CLK_LOW();
    delay_us(10);

    DS1302_RST_HIGH();
    delay_us(10);

    DS1302_WriteByte(cmd);
    DS1302_WriteByte(data);

    DS1302_RST_LOW();
    delay_us(10);
}

static uint8_t DS1302_ReadReg(uint8_t cmd)
{
    uint8_t data = 0;

    DS1302_RST_LOW();
    DS1302_CLK_LOW();
    delay_us(10);

    DS1302_RST_HIGH();
    delay_us(10);

    DS1302_WriteByte(cmd);
    data = DS1302_ReadByte();

    DS1302_RST_LOW();
    delay_us(10);

    return data;
}

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

    /* 只把写保护置于已知的"开启"状态。
     * 注意: 这里不清零秒寄存器(0x80), 否则每次上电都会把时间重置为 00:00:00,
     *       从而无法验证 DS1302 的断电走时。
     *       启动振荡器的职责交给 DS1302_SetTime()(其写入的秒值已带 CH=0)。 */
    DS1302_WriteReg(DS1302_REG_WP, 0x80);
}

/**
 * @brief 突发读取 8 字节寄存器
 *
 * @param raw 至少 8 字节的输出缓冲, 顺序: 秒 分 时 日 月 周 年 WP
 */
static void DS1302_BurstRead(uint8_t raw[8])
{
    if (raw == NULL)
        return;

    DS1302_RST_LOW();
    DS1302_CLK_LOW();
    delay_us(10);

    DS1302_RST_HIGH();
    delay_us(10);

    DS1302_WriteByte(0xBF); /* 时钟突发读取 */

    for (uint8_t i = 0; i < 8; i++)
        raw[i] = DS1302_ReadByte();

    DS1302_RST_LOW();
    delay_us(10);
}

/**
 * @brief 读一次DS1302时间(突发读 + 合法性判断)
 *
 * @param time 存储读取到的时间结构体指针
 * @return true 成功
 * @return false 失败
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
    time->hour = Bcd2Dec(buf[2] & 0x3F); /* 24小时制 */
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
 *
 * @param time 存储读取到的时间结构体指针
 * @return true 成功
 * @return false 失败
 *
 * @note  突发读期间若时钟半周期被压缩, 可能只错某一位而结果仍落在合法范围内
 *        (例如 00:13 被读成 01:13), 单靠范围校验挡不住。这里连读两次比对:
 *        两次间隔约2ms, 除"秒"以外的字段不应变化, 不一致就判本次读取失败,
 *        由调用方下个周期重试。代价是读取耗时翻倍(约2ms → 约4ms)。
 */
bool DS1302_ReadTime(DS1302_Time_t *time)
{
    DS1302_Time_t a, b;

    if (time == NULL)
        return false;

    if (!DS1302_ReadOnce(&a))
        return false;
    if (!DS1302_ReadOnce(&b))
        return false;

    if ((a.min != b.min) || (a.hour != b.hour) || (a.day != b.day) ||
        (a.month != b.month) || (a.week != b.week) || (a.year != b.year))
        return false;

    *time = a;
    return true;
}

/**
 * @brief 设置DS1302时间
 *
 * @param time 要设置的时间结构体指针
 * @return true 成功
 * @return false 失败
 */
bool DS1302_SetTime(const DS1302_Time_t *time)
{
    if (time == NULL)
        return false;

    DS1302_WriteReg(0x8E, 0x00); /* 解除写保护 */

    DS1302_RST_LOW();
    DS1302_CLK_LOW();
    delay_us(10);

    DS1302_RST_HIGH();
    delay_us(10);

    DS1302_WriteByte(0xBE); /* 时钟突发写入 */

    DS1302_WriteByte(Dec2Bcd(time->sec) & 0x7F); /* CH=0，启动 */
    DS1302_WriteByte(Dec2Bcd(time->min) & 0x7F);
    DS1302_WriteByte(Dec2Bcd(time->hour) & 0x3F); /* 24小时模式 */
    DS1302_WriteByte(Dec2Bcd(time->day) & 0x3F);
    DS1302_WriteByte(Dec2Bcd(time->month) & 0x1F);
    DS1302_WriteByte(Dec2Bcd(time->week) & 0x07);
    DS1302_WriteByte(Dec2Bcd((uint8_t)(time->year % 100)));
    DS1302_WriteByte(0x00); /* 第8字节是写保护寄存器，突发写时通常写0 */

    DS1302_RST_LOW();
    delay_us(10);

    DS1302_WriteReg(0x8E, 0x80); /* 恢复写保护 */

    return true;
}
