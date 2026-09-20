#include "W25Q64.h"

/* ================ W25Q64 相关信息 ================
 * 此项目中用来存储字库和图片信息
 * 大小 64 Mbit，8MB（8M 字节 = 8388608 字节）
 * SPI 接口 最大传输速率 75 MHz，支持 SPI 模式 0 和模式 3
 *   模式0: CPOL=0, CPHA=0   (空闲低电平, 第一个边沿采样)
 *   模式3: CPOL=1, CPHA=1   (空闲高电平, 第二个边沿采样)
 * 每页 256 字节，32768 页
 * 每扇区 4KB(4096 字节)，共 2048 个扇区
 * 每块 64KB，共 128 个块
 * 擦除方式：4KB 扇区擦除、32KB 块擦除、64KB 块擦除、整片擦除
 *   —— Flash 的特性：只能按位从 1 变 0，写之前必须先擦除(擦除即全置 1)
 * CLK 上升沿获取数据(主机采样 MISO)，下降沿输出数据(从机在下降沿更新 MISO)，数据均为 MSB 先发送
 * ================================ */

/* 忙等待超时累计次数, 由 W25Q64_GetBusyTimeoutCount() 读取, 便于测试定位硬件问题 */
static volatile uint32_t s_busy_timeout_cnt = 0;

static inline void W25Q64_CS_Low(void)
{
    GPIO_ResetBits(W25Q64_GPIO_PORT, W25Q64_CS_PIN);
}

static inline void W25Q64_CS_High(void)
{
    GPIO_SetBits(W25Q64_GPIO_PORT, W25Q64_CS_PIN);
}

/**
 * @brief 读写字节函数，用于与W25Q64 Flash进行数据交换
 * @param data 要发送的字节（读操作时统一填 0xFF 以产生时钟并避免驱动 MISO 冲突
 * @return uint8_t 接收的字节
 * */
static uint8_t W25Q64_RWByte(uint8_t data)
{
    while (SPI_GetFlagStatus(SPI1, SPI_FLAG_TXE) == RESET) /* 等待发送寄存器为空 */
        ;
    SPI_SendData(SPI1, data);                               /* 发送数据 */
    while (SPI_GetFlagStatus(SPI1, SPI_FLAG_RXNE) == RESET) /* 等待接收完成 */
        ;

    return SPI_ReceiveData(SPI1); /* 返回接收寄存器数据 */
}

static void W25Q64_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    GPIO_StructInit(&GPIO_InitStructure);

    /* 初始化CS引脚，推挽输出，内部上拉 */
    GPIO_InitStructure.GPIO_Pin = W25Q64_CS_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(W25Q64_GPIO_PORT, &GPIO_InitStructure);

    /* 初始化 SPI1 的 CLK MOSI MISO 引脚，复用推挽 */
    GPIO_PinAFConfig(W25Q64_GPIO_PORT, GPIO_PinSource5, GPIO_AF_SPI1);
    GPIO_PinAFConfig(W25Q64_GPIO_PORT, GPIO_PinSource6, GPIO_AF_SPI1);
    GPIO_PinAFConfig(W25Q64_GPIO_PORT, GPIO_PinSource7, GPIO_AF_SPI1);

    GPIO_InitStructure.GPIO_Pin = W25Q64_CLK_PIN | W25Q64_MOSI_PIN | W25Q64_MISO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(W25Q64_GPIO_PORT, &GPIO_InitStructure);
}

static void W25Q64_SPI_Init(void)
{
    SPI_InitTypeDef SPI_InitStructure;
    SPI_StructInit(&SPI_InitStructure);

    SPI_InitStructure.SPI_Direction = SPI_Direction_2Lines_FullDuplex; // 2线全双工模式
    SPI_InitStructure.SPI_Mode = SPI_Mode_Master;                      // 主模式
    SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;                  // 8位数据
    SPI_InitStructure.SPI_CPHA = SPI_CPHA_1Edge;                       // 1; 1边沿采样
    SPI_InitStructure.SPI_CPOL = SPI_CPOL_Low;                         // 0; 低电平有效
    SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_4; // SPI1=84MHz → 21MHz
    SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;                 // 最高位先发送
    SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;                          // 软件控制NSS引脚
    SPI_InitStructure.SPI_CRCPolynomial = 7;                           // CRC多项式7

    SPI_Init(SPI1, &SPI_InitStructure);
    SPI_Cmd(SPI1, ENABLE);
}

void W25Q64_Init(void)
{
    W25Q64_GPIO_Init();
    W25Q64_SPI_Init();
    W25Q64_CS_High(); /* 上电先释放片选, 保证首个命令的起始沿被芯片识别 */
    W25Q64_CS_Low();
    W25Q64_RWByte(W25Q64_CMD_RELEASE_PD); /* 释放保护模式，恢复正常工作 */
    W25Q64_CS_High();

    /* 芯片从掉电模式唤醒需要 tRES1(典型 3us)，这里做保守延时 */
    for (volatile uint32_t i = 0; i < 100; i++)
        ;
}

static void W25Q64_WriteEnable(void)
{
    W25Q64_CS_Low();
    W25Q64_RWByte(W25Q64_CMD_WRITE_ENABLE);
    W25Q64_CS_High();
}

/**
 * @brief 轮询状态寄存器直到 BUSY 清零
 * @return true 芯片已空闲, false 超时(芯片无响应)
 * @note  每次读取状态都必须重新拉低再拉高片选: 若退出时 CS 保持低电平,
 *        后续命令会被芯片当作上一条命令的延续而全部失效(写/擦除静默失败)。
 * */
static bool W25Q64_WaitBusy(void)
{
    uint8_t status;
    uint32_t timeout = W25Q64_BUSY_TIMEOUT;

    do
    {
        W25Q64_CS_Low();
        W25Q64_RWByte(W25Q64_CMD_READ_SR1); /* 读状态寄存器1 */
        status = W25Q64_RWByte(0xFF);       // 发送0xFF，读一字节状态
        W25Q64_CS_High();                   /* 必须释放片选 */
    } while ((status & W25Q64_SR1_BUSY) && (--timeout != 0U)); /* 等待 BUSY 清零或超时 */

    /* 超时或芯片无响应, 计数 + 1，返回 false */
    if (status & W25Q64_SR1_BUSY)
    {
        s_busy_timeout_cnt++;
        return false;
    }

    return true;
}

static uint8_t W25Q64_ReadStatus1(void)
{
    uint8_t status;
    W25Q64_CS_Low();
    W25Q64_RWByte(W25Q64_CMD_READ_SR1);
    status = W25Q64_RWByte(0xFF);
    W25Q64_CS_High();

    return status;
}

/* ================ 外部调用函数 ================ */

uint32_t W25Q64_GetBusyTimeoutCount(void)
{
    return s_busy_timeout_cnt;
}

uint16_t W25Q64_ReadId(void)
{
    uint16_t id;

    W25Q64_CS_Low();
    W25Q64_RWByte(W25Q64_CMD_READ_ID); /* 0x90: 缺少此命令字节会读到无效数据 */
    W25Q64_RWByte(0x00);               /* 地址高字节 = 00，伪字节占位 */
    W25Q64_RWByte(0x00);               /* 地址中字节 = 00，伪字节占位 */
    W25Q64_RWByte(0x00);               /* 地址低字节 = 00，伪字节占位 */

    id = W25Q64_RWByte(0xFF) << 8; // 读厂商 ID（0xEF）
    id |= W25Q64_RWByte(0xFF);     // 读器件 ID（0x16）
    W25Q64_CS_High();              /* 拉高片选 */

    return id;
}

uint32_t W25Q64_ReadJedecId(void)
{
    uint32_t id;

    W25Q64_CS_Low();
    W25Q64_RWByte(W25Q64_CMD_JEDEC_ID); /* 发送 9Fh */
    id = W25Q64_RWByte(0xFF) << 16;     // 厂商 ID
    id |= W25Q64_RWByte(0xFF) << 8;     // 存储类型
    id |= W25Q64_RWByte(0xFF);          // 容量
    W25Q64_CS_High();

    return id;
}

uint8_t W25Q64_IsBusy(void)
{
    return (W25Q64_ReadStatus1() & W25Q64_SR1_BUSY) ? 1 : 0;
}

/**
 * @brief 从W25Q64 Flash读取数据
 * @param addr 起始地址（字节对齐）
 * @param buf 要读取的缓冲区指针
 * @param len 要读取的字节数
 * @note  读取时必须先发送地址字节，再发送数据字节。
 * */
void W25Q64_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    W25Q64_CS_Low();
    W25Q64_RWByte(W25Q64_CMD_READ_DATA); /* 读取数据 */
    W25Q64_RWByte((addr >> 16) & 0xFF);  /* 地址高字节 */
    W25Q64_RWByte((addr >> 8) & 0xFF);   /* 地址中字节 */
    W25Q64_RWByte(addr & 0xFF);          /* 地址低字节 */

    while (len--)
    {
        *buf++ = W25Q64_RWByte(0xFF);
    }
    W25Q64_CS_High();
}

/**
 * @brief 页编程函数，用于将数据写入W25Q64 Flash的指定页
 * @param addr 起始地址（字节对齐，可跨页，函数内部自动分页）
 * @param buf 要写入的缓冲区指针
 * @param len 要写入的字节数
 * @return true 全部写入完成, false 出现忙等待超时
 * */
bool W25Q64_PageProgram(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    bool ok = true;

    while (len > 0)
    {
        /* 本页剩余字节数: 必须用 (PAGE_SIZE - 1) 取页内偏移。
         * 若写成 (addr & W25Q64_PAGE_SIZE), 当 addr 的 bit8=1 时结果为 0,
         * write_len 变为 0 而 len 永不减少 -> 死循环。 */
        uint32_t page_remain = W25Q64_PAGE_SIZE - (addr & (W25Q64_PAGE_SIZE - 1U));
        uint32_t write_len = (len < page_remain) ? len : page_remain;

        W25Q64_WriteEnable();
        W25Q64_CS_Low();
        W25Q64_RWByte(W25Q64_CMD_PAGE_PROGRAM);
        W25Q64_RWByte((addr >> 16) & 0xFF);
        W25Q64_RWByte((addr >> 8) & 0xFF);
        W25Q64_RWByte(addr & 0xFF);

        /* 逐字节写入 */
        for (uint32_t i = 0; i < write_len; i++)
            W25Q64_RWByte(buf[i]);

        /* 拉高片选，芯片内部开始编程 */
        W25Q64_CS_High();

        /* 等待编程完成 */
        if (!W25Q64_WaitBusy())
            ok = false;

        /* 推进地址/缓冲区/剩余长度，准备写下一页 */
        addr += write_len;
        buf += write_len;
        len -= write_len;
    }

    return ok;
}

/**
 * @brief 扇区擦除函数，用于将W25Q64 Flash的指定扇区全部擦除为 0xFF
 * @param addr 扇区起始地址（字节对齐）
 * @return true 完成, false 超时
 * @note  扇区擦除时必须先发送地址字节，再发送命令字节。
 * */
bool W25Q64_SectorErase(uint32_t addr)
{
    addr &= ~(W25Q64_SECTOR_SIZE - 1); // 地址向下对齐到 4KB 边界

    W25Q64_WriteEnable();
    W25Q64_CS_Low();
    W25Q64_RWByte(W25Q64_CMD_SECTOR_ERASE);
    W25Q64_RWByte((addr >> 16) & 0xFF);
    W25Q64_RWByte((addr >> 8) & 0xFF);
    W25Q64_RWByte(addr & 0xFF);
    W25Q64_CS_High();

    return W25Q64_WaitBusy();
}

/**
 * @brief 32KB 块擦除函数，用于将W25Q64 Flash的指定 32KB 块全部擦除为 0xFF
 * @param addr 32KB 块起始地址（字节对齐）
 * @return true 完成, false 超时
 * @note  32KB 块擦除时必须先发送地址字节，再发送命令字节。
 * */
bool W25Q64_Block32Erase(uint32_t addr)
{
    addr &= ~(W25Q64_BLOCK32_SIZE - 1); // 地址向下对齐到 32KB 边界

    W25Q64_WriteEnable();
    W25Q64_CS_Low();
    W25Q64_RWByte(W25Q64_CMD_BLOCK32_ERASE);
    W25Q64_RWByte((addr >> 16) & 0xFF);
    W25Q64_RWByte((addr >> 8) & 0xFF);
    W25Q64_RWByte(addr & 0xFF);
    W25Q64_CS_High();

    return W25Q64_WaitBusy();
}

/**
 * @brief 整片擦除
 * @return true 完成, false 超时
 * @note  8MB 芯片典型耗时 20~100s, 会长时间阻塞, 测试时慎用
 * */
bool W25Q64_ChipErase(void)
{
    W25Q64_WriteEnable();
    W25Q64_CS_Low();
    W25Q64_RWByte(W25Q64_CMD_CHIP_ERASE);
    W25Q64_CS_High();

    return W25Q64_WaitBusy();
}
