#include "W25Q64.h"

/* ================ W25Q64 相关信息 ================
 * - 本项目中用来存储字库、图片信息、部分配置信息、OTA 更新文件
 * - 大小 64 Mbit，8MB（8M 字节）
 * - 寻址范围：24 位地址（A23 ~ A0），0x000000 ~ 0x7FFFFF
 * - SPI 接口 最大传输速率 75 MHz，支持 SPI 模式 0 和模式 3
 *      模式0: CPOL=0, CPHA=0   (空闲低电平, 第一个边沿采样)
 *      模式3: CPOL=1, CPHA=1   (空闲高电平, 第二个边沿采样)
 * - 存储结构：
 *       128 个 Block（块），每块 64KB；
 *       2048 个 Sector（扇区），每扇区 4KB；
 *       32768 个 Page（页），每页 256 字节；
 * - 擦除方式：4KB 扇区擦除、32KB 块擦除、64KB 块擦除、整片擦除
 * - CLK 上升沿获取数据(主机采样 MISO)，
 *   下降沿输出数据(从机在下降沿更新 MISO)，数据均为 MSB 先发送
 *
 * - 读数据流程：
 *       拉低 CS -> 发送指令 0x03（快读 0x08） -> 发送 24 位地址 ->
 *       SPI 读写 -> 拉高 CS
 * - 写数据流程：
 *       擦除 -> 拉低 CS -> 发送指令 0x06 -> 拉高 CS -> 拉低 CS ->
 *       发送指令 0x02 -> 发送 24 位地址 -> 数据 -> 拉高 CS->
 *       等待忙标志位为 0 (寄存器读取指令 0x05）
 * - 擦除流程：
 *       拉低 CS -> 发送指令 0x06 -> 拉高 CS -> 拉低 CS ->
 *       发送指令 0x20 -> 发送 24 位扇区地址 -> 拉高 CS ->
 *       等待忙标志位为 0 (寄存器读取指令 0x05)
 * ================================ */

#define W25Q64_BUSY_TIMEOUT 1000000U // 忙等待超时(轮询次数上限)，数值要大覆盖扇区擦除时间

/* 忙等待超时累计次数 */
static volatile uint32_t s_busy_timeout_cnt = 0;

static inline void W25Q64_CS_Low(void) { GPIO_ResetBits(W25Q64_GPIO_PORT, W25Q64_CS_PIN); }
static inline void W25Q64_CS_High(void) { GPIO_SetBits(W25Q64_GPIO_PORT, W25Q64_CS_PIN); }

static uint8_t W25Q64_SpiReadWriteByte(uint8_t data);
static void W25Q64_GPIO_Init(void);
static void W25Q64_SPI_Init(void);
static void W25Q64_WriteEnable(void);
static bool W25Q64_WaitBusy(void);
static uint8_t W25Q64_ReadStatus1(void);

/**
 * @brief 读写字节函数，用于与W25Q64 Flash进行数据交换
 * @param data 要发送的字节，读操作时统一填 0xFF/0x00
 * @return uint8_t 接收的字节
 */
static uint8_t W25Q64_SpiReadWriteByte(uint8_t data)
{
    /* 等待发送寄存器为空
     * 读写数据
     * 等待接收完成 */
    while (SPI_GetFlagStatus(SPI1, SPI_FLAG_TXE) == RESET)
    {
    }
    SPI_SendData(SPI1, data);
    while (SPI_GetFlagStatus(SPI1, SPI_FLAG_RXNE) == RESET)
    {
    }

    /* 返回接收寄存器数据 */
    return SPI_ReceiveData(SPI1);
}

static void W25Q64_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    GPIO_StructInit(&GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin = W25Q64_CS_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP; // 推挽输出
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;   // 内部上拉
    GPIO_Init(W25Q64_GPIO_PORT, &GPIO_InitStructure);

    /* 初始化 SPI1 的 CLK MOSI MISO 引脚，复用推挽 */
    GPIO_PinAFConfig(W25Q64_GPIO_PORT, GPIO_PinSource5, GPIO_AF_SPI1);
    GPIO_PinAFConfig(W25Q64_GPIO_PORT, GPIO_PinSource6, GPIO_AF_SPI1);
    GPIO_PinAFConfig(W25Q64_GPIO_PORT, GPIO_PinSource7, GPIO_AF_SPI1);

    GPIO_InitStructure.GPIO_Pin = W25Q64_CLK_PIN | W25Q64_MOSI_PIN | W25Q64_MISO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF; // 复用推挽输出
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP; // 推挽输出
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;   // 内部上拉
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

/** @brief 写使能 */
static void W25Q64_WriteEnable(void)
{
    W25Q64_CS_Low();
    W25Q64_SpiReadWriteByte(W25Q64_CMD_WRITE_ENABLE);
    W25Q64_CS_High();
}

/**
 * @brief 轮询状态寄存器直到 BUSY 清零
 * @return true 芯片已空闲, false 超时(芯片无响应)
 */
static bool W25Q64_WaitBusy(void)
{
    uint8_t status;
    uint32_t timeout = W25Q64_BUSY_TIMEOUT;

    /* 等待 BUSY 清零或超时 */
    do
    {
        W25Q64_CS_Low();
        W25Q64_SpiReadWriteByte(W25Q64_CMD_READ_SR1); // 读状态寄存器1
        status = W25Q64_SpiReadWriteByte(0xFF);       // 发送0xFF，读一字节状态
        W25Q64_CS_High();
    } while ((status & W25Q64_SR1_BUSY) && (--timeout != 0U));

    /* 超时或芯片无响应, 计数 + 1，返回 false */
    if (status & W25Q64_SR1_BUSY)
    {
        s_busy_timeout_cnt++;
        return false;
    }

    return true;
}

/** @brief 读取状态寄存器1
 *  @return uint8_t 状态寄存器1的字节值
 */
static uint8_t W25Q64_ReadStatus1(void)
{
    uint8_t status;
    W25Q64_CS_Low();
    W25Q64_SpiReadWriteByte(W25Q64_CMD_READ_SR1);
    status = W25Q64_SpiReadWriteByte(0xFF);
    W25Q64_CS_High();

    return status;
}

void W25Q64_Init(void)
{
    W25Q64_GPIO_Init();
    W25Q64_SPI_Init();

    W25Q64_CS_High(); // 上电先释放片选, 保证首个命令的起始沿被芯片识别

    W25Q64_CS_Low();
    W25Q64_SpiReadWriteByte(W25Q64_CMD_RELEASE_PD); // 释放保护模式，恢复正常工作
    W25Q64_CS_High();

    /* 芯片从掉电模式唤醒需要 tRES1(典型 3us)，这里做保守延时 */
    for (volatile uint32_t i = 0; i < 100; i++)
        ;
}

/** @brief 获取忙等待超时累计次数
 *  @return uint32_t 忙等待超时累计次数
 */
uint32_t W25Q64_GetBusyTimeoutCount(void)
{
    return s_busy_timeout_cnt;
}

/** @brief 读取 W25Q64 Flash 的 ID
 *  @return uint16_t 厂商 ID（0xEF） + 器件 ID（0x16）
 * */
uint16_t W25Q64_ReadId(void)
{
    uint16_t id;

    W25Q64_CS_Low();
    W25Q64_SpiReadWriteByte(W25Q64_CMD_READ_ID);
    W25Q64_SpiReadWriteByte(0x00);           // 地址占位高字节
    W25Q64_SpiReadWriteByte(0x00);           // 地址占位中字节
    W25Q64_SpiReadWriteByte(0x00);           // 地址占位低字节
    id = W25Q64_SpiReadWriteByte(0xFF) << 8; // 读厂商 ID（0xEF）
    id |= W25Q64_SpiReadWriteByte(0xFF);     // 读器件 ID（0x16）
    W25Q64_CS_High();

    return id;
}

/** @brief 读取 W25Q64 Flash 的 JEDEC ID
 *  @return uint32_t JEDEC ID（0xEF） + 存储类型（0x16） + 容量（0x00）
 * */
uint32_t W25Q64_ReadJedecId(void)
{
    uint32_t id;

    W25Q64_CS_Low();
    W25Q64_SpiReadWriteByte(W25Q64_CMD_JEDEC_ID);
    id = W25Q64_SpiReadWriteByte(0xFF) << 16; // 厂商 ID
    id |= W25Q64_SpiReadWriteByte(0xFF) << 8; // 存储器 ID
    id |= W25Q64_SpiReadWriteByte(0xFF);      // 兼容性 ID
    W25Q64_CS_High();

    return id;
}

/** @brief 检查 W25Q64 Flash 是否忙
 *  @return true 芯片忙, false 芯片空闲
 * */
uint8_t W25Q64_IsBusy(void)
{
    return (W25Q64_ReadStatus1() & W25Q64_SR1_BUSY) ? 1 : 0;
}

/**
 * @brief 从 W25Q64 Flash 读取数据
 * @param addr 起始地址（字节对齐）
 * @param buf 要读取的缓冲区指针
 * @param len 要读取的字节数
 * */
void W25Q64_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    W25Q64_CS_Low();
    /* 读取数据高中低字节 */
    W25Q64_SpiReadWriteByte(W25Q64_CMD_READ_DATA);
    W25Q64_SpiReadWriteByte((addr >> 16) & 0xFF);
    W25Q64_SpiReadWriteByte((addr >> 8) & 0xFF);
    W25Q64_SpiReadWriteByte(addr & 0xFF);

    while (len--)
        *buf++ = W25Q64_SpiReadWriteByte(0xFF);
    W25Q64_CS_High();
}

/**
 * @brief 页编程函数，用于将数据写入 W25Q64 Flash 的指定 256字节页
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
        uint32_t pagebyte_remain = W25Q64_PAGE_SIZE - (addr & (W25Q64_PAGE_SIZE - 1U)); // 页剩余字节数
        uint32_t writebyte_len = (len < pagebyte_remain) ? len : pagebyte_remain;       // 写入字节数

        W25Q64_WriteEnable(); // 写使能
        W25Q64_CS_Low();
        W25Q64_SpiReadWriteByte(W25Q64_CMD_PAGE_PROGRAM); // 页编程命令
        W25Q64_SpiReadWriteByte((addr >> 16) & 0xFF);
        W25Q64_SpiReadWriteByte((addr >> 8) & 0xFF);
        W25Q64_SpiReadWriteByte(addr & 0xFF);
        for (uint32_t i = 0; i < writebyte_len; i++)
            W25Q64_SpiReadWriteByte(buf[i]);
        W25Q64_CS_High(); // 拉高片选，芯片内部开始编程

        /* 等待编程完成 */
        if (!W25Q64_WaitBusy())
            ok = false;

        /* 推进地址/缓冲区/剩余长度，准备写下一页 */
        addr += writebyte_len;
        buf += writebyte_len;
        len -= writebyte_len;
    }

    return ok;
}

/**
 * @brief 扇区擦除函数，将W25Q64 Flash的指定扇区全部擦除为 0xFF
 * @param addr 扇区起始地址（字节对齐）
 * @return true 完成, false 超时
 * */
bool W25Q64_SectorErase(uint32_t addr)
{
    /* 地址向下对齐到 4KB (0x1000) 边界
     * 每个扇区: 0x0000 0000 ~ 0x0000 0FFF(4KB)
     *           0x0000 1000 ~ 0x0000 1FFF(4KB)
     * 传入地址: 0x00 xxxx,只需判断低四位的最高位即可确定扇区起始地址 */
    addr &= ~(W25Q64_SECTOR_SIZE - 1);

    W25Q64_WriteEnable(); // 写使能
    W25Q64_CS_Low();
    W25Q64_SpiReadWriteByte(W25Q64_CMD_SECTOR_ERASE); // 扇区擦除命令
    W25Q64_SpiReadWriteByte((addr >> 16) & 0xFF);
    W25Q64_SpiReadWriteByte((addr >> 8) & 0xFF);
    W25Q64_SpiReadWriteByte(addr & 0xFF);
    W25Q64_CS_High();

    return W25Q64_WaitBusy(); // 等待擦除完成后返回是否成功
}

/**
 * @brief 32KB 块擦除函数，将W25Q64 Flash的指定 32KB 块全部擦除为 0xFF
 * @param addr 32KB 块起始地址（字节对齐）
 * @return true 完成, false 超时
 * */
bool W25Q64_Block32Erase(uint32_t addr)
{
    /* 地址向下对齐到 32KB (0x8000) 边界
     * 每个块: 0x0000 0000 ~ 0x0000 7FFF(32KB)
     *         0x0000 8000 ~ 0x0000 FFFF(32KB)
     * 传入地址: 0x00 xxxx,只需判断低四位的最高位即可确定块起始地址 */
    addr &= ~(W25Q64_BLOCK32_SIZE - 1);

    W25Q64_WriteEnable(); // 写使能
    W25Q64_CS_Low();
    W25Q64_SpiReadWriteByte(W25Q64_CMD_BLOCK32_ERASE); // 32KB 块擦除命令
    W25Q64_SpiReadWriteByte((addr >> 16) & 0xFF);
    W25Q64_SpiReadWriteByte((addr >> 8) & 0xFF);
    W25Q64_SpiReadWriteByte(addr & 0xFF);
    W25Q64_CS_High();

    return W25Q64_WaitBusy(); // 等待擦除完成后返回是否成功
}

/**
 * @brief 整片擦除
 * @return true 完成, false 超时
 * */
bool W25Q64_ChipErase(void)
{
    W25Q64_WriteEnable();
    W25Q64_CS_Low();
    W25Q64_SpiReadWriteByte(W25Q64_CMD_CHIP_ERASE); // 整片擦除命令
    W25Q64_CS_High();

    return W25Q64_WaitBusy();
}
