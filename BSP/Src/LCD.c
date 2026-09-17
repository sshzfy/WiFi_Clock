#include "LCD.h"

#define GRAM_DMA_MAX_HALFWORD 65535U // DMA NDTR(16bit)单次最大半字数
#define SCRATCH_H_PX 48              // 最大渲染行高(对齐Font_48)

/* 图片流式发送的乒乓缓冲: 每块 WIDTH*16 像素 = 7680 字节, 两块共 15360 字节,
 * 在 ST7789_Init() 里从 FreeRTOS heap 分配。
 * 必须落在 SRAM1(heap_4 的 ucHeap 位于 .bss 段) —— DMA 访问不了 CCM。 */
#define PIC_BLK_LINES 16
#define PIC_BLK_PX    (WIDTH * PIC_BLK_LINES)

static void ST7789_Rest(void);
static void ST7789_Set_Backlight(bool state);
static void ST7789_Display_Init(void);
static void ST7789_GPIO_Init(void);
static void ST7789_SPI_Init(void);
static void ST7789_DMA_Init(void);
static bool Is_in_Screen(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
static bool Is_GB2312(char ch);
static void ST7789_SPI_SetDataSize(uint8_t size);
static void ST7789_Wait_TXE(void);
static void ST7789_Wait_BSY(void);
static void ST7789_Send8(const uint8_t *data, uint16_t len);
static void ST7789_Write_Reg(uint8_t reg, const uint8_t data[], uint16_t len);
static void ST7789_SetWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
static void ST7789_DMA_Pump(const uint8_t *src, uint32_t halfwords, bool inc);
static void ST7789_Write_Gram(const uint8_t data[], uint32_t len, bool increase);
static bool Color_IsClose(uint16_t c1, uint16_t c2, uint8_t threshold);
static void ST7789_DMA_Start(const uint8_t *src, uint32_t halfwords);
static bool ST7789_DMA_WaitDone(void);
static bool ST7789_PicBuf_Init(void);
static void ST7789_PixTransparent(uint8_t *buf, uint32_t px, uint16_t target_back);
static void ST7789_DrawImage_Stream(uint16_t x, uint16_t y, const Image_t *img,
                                    bool transparency, uint16_t target_back);

/* 渲染缓冲: 一满行(240px)*最高48px*2字节, 字符串整行与图标共用 */
static uint8_t s_scratch[WIDTH * SCRATCH_H_PX * 2];

/* 图片乒乓缓冲(heap 分配)。任一块为 NULL 表示分配失败, 退化为单缓冲串行 */
static uint8_t *s_picBuf[2] = {NULL, NULL};

/* 单字模缓冲: 全部字模里最大 144 字节(48号ASCII = 3 字节/行 * 48 行) */
static uint8_t s_glyph[ASSET_GLYPH_BYTES_MAX];

/* ============ SPI 数据宽度(8位命令/16位像素) ============ */
static uint8_t s_spi_datasize = 0; // 0=未设置 8=8位 16=16位

static void ST7789_Wait_TXE(void)
{
    while (SPI_GetFlagStatus(SPI3, SPI_FLAG_TXE) == RESET)
        ;
}

static void ST7789_Wait_BSY(void)
{
    while (SPI_GetFlagStatus(SPI3, SPI_FLAG_BSY) != RESET)
        ;
}

/**
 * @brief 设置SPI数据宽度(8位命令/16位像素),只在需要切换时切换; 且先等SPI空闲再在SPE=0下修改, 保证可靠
 *
 * @param size 8=8位 16=16位
 */
static void ST7789_SPI_SetDataSize(uint8_t size)
{
    if (s_spi_datasize == size)
        return;

    ST7789_Wait_BSY();
    SPI_Cmd(SPI3, DISABLE);
    SPI_DataSizeConfig(SPI3, (size == 16) ? SPI_DataSize_16b : SPI_DataSize_8b);
    SPI_Cmd(SPI3, ENABLE);

    s_spi_datasize = size;
}

/**
 * @brief 8位模式连续发送若干字节(CS/DC状态已就绪), 结束时等待SPI空闲
 *
 * @param data 要发送的数据指针
 * @param len 要发送的数据长度
 */
static void ST7789_Send8(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
    {
        SPI_SendData(SPI3, data[i]);
        ST7789_Wait_TXE();
    }
    ST7789_Wait_BSY();
}

/**
 * @brief 写入8位命令寄存器
 *
 * @param reg 命令寄存器地址
 * @param data 要写入的数据指针
 * @param len 要写入的数据长度
 */
static void ST7789_Write_Reg(uint8_t reg, const uint8_t data[], uint16_t len)
{
    ST7789_SPI_SetDataSize(8); // 设置为8位模式

    GPIO_ResetBits(ST7789_CS_PORT, ST7789_CS_PIN); // 拉低CS,片选
    GPIO_ResetBits(ST7789_DC_PORT, ST7789_DC_PIN); // 拉低DC,发送命令
    ST7789_Send8(&reg, 1);                         // 发送命令

    GPIO_SetBits(ST7789_DC_PORT, ST7789_DC_PIN); // 拉高DC,发送数据
    ST7789_Send8(data, len);                     // 发送数据

    GPIO_SetBits(ST7789_CS_PORT, ST7789_CS_PIN); // 释放CS,结束一次命令传输
}

/**
 * @brief 一次CS低电平内连续写入 0x2A/0x2B/0x2C, 省去多次CS与BSY往返
 *
 * @param x1 窗口左上角X坐标
 * @param y1 窗口左上角Y坐标
 * @param x2 窗口右下角X坐标
 * @param y2 窗口右下角Y坐标
 */
static void ST7789_SetWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t xdata[4]; // 窗口X坐标高8位/低8位
    uint8_t ydata[4]; // 窗口Y坐标高8位/低8位
    uint8_t cmd;

    xdata[0] = (uint8_t)(x1 >> 8); // 窗口左上角X坐标高8位
    xdata[1] = (uint8_t)x1;        // 窗口左上角X坐标低8位
    xdata[2] = (uint8_t)(x2 >> 8); // 窗口右下角X坐标高8位
    xdata[3] = (uint8_t)x2;        // 窗口右下角X坐标低8位
    ydata[0] = (uint8_t)(y1 >> 8); // 窗口左上角Y坐标高8位
    ydata[1] = (uint8_t)y1;        // 窗口左上角Y坐标低8位
    ydata[2] = (uint8_t)(y2 >> 8); // 窗口右下角Y坐标高8位
    ydata[3] = (uint8_t)y2;        // 窗口右下角Y坐标低8位

    ST7789_SPI_SetDataSize(8);
    GPIO_ResetBits(ST7789_CS_PORT, ST7789_CS_PIN);

    GPIO_ResetBits(ST7789_DC_PORT, ST7789_DC_PIN);
    cmd = 0x2A; // 列地址设置
    ST7789_Send8(&cmd, 1);
    GPIO_SetBits(ST7789_DC_PORT, ST7789_DC_PIN);
    ST7789_Send8(xdata, 4);

    GPIO_ResetBits(ST7789_DC_PORT, ST7789_DC_PIN);
    cmd = 0x2B; // 行地址设置
    ST7789_Send8(&cmd, 1);
    GPIO_SetBits(ST7789_DC_PORT, ST7789_DC_PIN);
    ST7789_Send8(ydata, 4);

    GPIO_ResetBits(ST7789_DC_PORT, ST7789_DC_PIN);
    cmd = 0x2C; // 写内存命令
    ST7789_Send8(&cmd, 1);
    GPIO_SetBits(ST7789_DC_PORT, ST7789_DC_PIN);

    GPIO_SetBits(ST7789_CS_PORT, ST7789_CS_PIN);
}

/* ============ DMA(SPI3_TX: DMA1_Stream5, Channel0) ============ */

/**
 * @brief 初始化DMA1_Stream5, Channel0, 用于SPI3_TX
 */
static void ST7789_DMA_Init(void)
{
    DMA_InitTypeDef DMA_InitStructure;

    DMA_DeInit(DMA1_Stream5);
    DMA_StructInit(&DMA_InitStructure);

    DMA_InitStructure.DMA_Channel = DMA_Channel_0;                              // SPI3_TX
    DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&(SPI3->DR);           // SPI3数据寄存器地址
    DMA_InitStructure.DMA_Memory0BaseAddr = 0;                                  // 内存基地址
    DMA_InitStructure.DMA_DIR = DMA_DIR_MemoryToPeripheral;                     // 内存→外设
    DMA_InitStructure.DMA_BufferSize = 0;                                       // 缓冲区大小
    DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;            // 外设地址不增加
    DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;                     // 内存地址增加
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord; // 外设数据宽度为半字
    DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;         // 内存数据宽度为半字
    DMA_InitStructure.DMA_Mode = DMA_Mode_Normal;                               // 正常模式
    DMA_InitStructure.DMA_Priority = DMA_Priority_Medium;                       // 中优先级
    DMA_InitStructure.DMA_FIFOMode = DMA_FIFOMode_Disable;                      // 不使用FIFO

    DMA_Init(DMA1_Stream5, &DMA_InitStructure);
}

/**
 * @brief 半字模式内存→外设 DMA 泵; 内部自动按NDTR上限分块, 出错(TE)则中止
 * @param src 要发送的数据指针
 * @param halfwords 要发送的数据长度(半字)
 * @param inc 是否自动增加内存指针
 */
static void ST7789_DMA_Pump(const uint8_t *src, uint32_t halfwords, bool inc)
{
    while (halfwords > 0)
    {
        uint32_t chunk = (halfwords > GRAM_DMA_MAX_HALFWORD) ? GRAM_DMA_MAX_HALFWORD : halfwords; // 分块大小,不超过最大传输半字数65535

        DMA_Cmd(DMA1_Stream5, DISABLE);
        while (DMA_GetCmdStatus(DMA1_Stream5) != DISABLE)
            ;

        DMA1_Stream5->NDTR = (uint16_t)chunk; // 设置传输半字数
        DMA1_Stream5->M0AR = (uint32_t)src;   // 设置内存基地址
        if (inc)
            DMA1_Stream5->CR |= DMA_SxCR_MINC; // 内存地址增加
        else
            DMA1_Stream5->CR &= (uint32_t)~DMA_SxCR_MINC; // 内存地址不增加

        DMA_ClearFlag(DMA1_Stream5, DMA_FLAG_FEIF5 | DMA_FLAG_TCIF5 | DMA_FLAG_TEIF5); // 清除DMA标志位
        DMA_Cmd(DMA1_Stream5, ENABLE);

        while (DMA_GetFlagStatus(DMA1_Stream5, DMA_FLAG_TCIF5) == RESET)
        {
            if (DMA_GetFlagStatus(DMA1_Stream5, DMA_FLAG_TEIF5) != RESET)
            {
                DMA_Cmd(DMA1_Stream5, DISABLE);
                printf("[ERR]ST7789_DMA_Pump: DMA传输错误\r\n");
                return; // 传输错误
            }
        }
        DMA_ClearFlag(DMA1_Stream5, DMA_FLAG_TCIF5); // 清除传输完成标志位

        halfwords -= chunk; // 剩余半字数
        if (inc)
            src += (uint32_t)chunk * 2; // 内存指针增加
    }
}

/**
 * @brief 非阻塞启动一次 SPI3_TX DMA 发送(内存地址自增), 供图片双缓冲使用
 * @param src 要发送的数据指针(需 2 字节对齐)
 * @param halfwords 要发送的半字数(不超过 65535)
 * @note 启动后必须用 ST7789_DMA_WaitDone() 收尾, 否则下一次 Start 会打断本次传输
 */
static void ST7789_DMA_Start(const uint8_t *src, uint32_t halfwords)
{
    DMA_Cmd(DMA1_Stream5, DISABLE);
    while (DMA_GetCmdStatus(DMA1_Stream5) != DISABLE)
        ;

    DMA1_Stream5->NDTR = (uint16_t)halfwords; // 传输半字数
    DMA1_Stream5->M0AR = (uint32_t)src;       // 内存基地址
    DMA1_Stream5->CR |= DMA_SxCR_MINC;        // 内存地址增加

    DMA_ClearFlag(DMA1_Stream5, DMA_FLAG_FEIF5 | DMA_FLAG_TCIF5 | DMA_FLAG_TEIF5);
    DMA_Cmd(DMA1_Stream5, ENABLE);
}

/**
 * @brief 等待当前 DMA 发送结束
 * @return true 正常完成; false 传输出错(已中止)
 */
static bool ST7789_DMA_WaitDone(void)
{
    while (DMA_GetFlagStatus(DMA1_Stream5, DMA_FLAG_TCIF5) == RESET)
    {
        if (DMA_GetFlagStatus(DMA1_Stream5, DMA_FLAG_TEIF5) != RESET)
        {
            DMA_Cmd(DMA1_Stream5, DISABLE);
            printf("[ERR]DMA transfer error\r\n");
            return false;
        }
    }

    DMA_ClearFlag(DMA1_Stream5, DMA_FLAG_TCIF5);
    return true;
}

/**
 * @brief 分配图片乒乓缓冲(只在首次调用时真正分配, 之后常驻)
 * @return true 双缓冲可用; false 分配失败, 调用方退化为单缓冲
 */
static bool ST7789_PicBuf_Init(void)
{
    if (s_picBuf[0] != NULL && s_picBuf[1] != NULL)
        return true;

    s_picBuf[0] = (uint8_t *)pvPortMalloc((uint32_t)PIC_BLK_PX * 2U);
    s_picBuf[1] = (uint8_t *)pvPortMalloc((uint32_t)PIC_BLK_PX * 2U);

    if (s_picBuf[0] == NULL || s_picBuf[1] == NULL)
    {
        if (s_picBuf[0] != NULL)
        {
            vPortFree(s_picBuf[0]);
            s_picBuf[0] = NULL;
        }
        if (s_picBuf[1] != NULL)
        {
            vPortFree(s_picBuf[1]);
            s_picBuf[1] = NULL;
        }
        printf("[LCD ] ping-pong buffer alloc failed, fallback to single buffer\r\n");
        return false;
    }

    printf("[LCD ] ping-pong buffer ready: %u x 2 = %u bytes\r\n",
           (unsigned)((uint32_t)PIC_BLK_PX * 2U), (unsigned)((uint32_t)PIC_BLK_PX * 4U));
    return true;
}

/**
 * @brief 把缓冲里接近白色的像素替换成目标背景色(原地修改)
 * @param buf 像素缓冲(RGB565, 低字节在前)
 * @param px  像素个数
 * @param target_back 替换成的目标背景色
 */
static void ST7789_PixTransparent(uint8_t *buf, uint32_t px, uint16_t target_back)
{
    for (uint32_t i = 0; i < px; i++)
    {
        uint16_t pixel = (uint16_t)((uint16_t)buf[i * 2U] | ((uint16_t)buf[i * 2U + 1U] << 8));

        if (Color_IsClose(pixel, COLOR_WHITE, 90))
        {
            pixel = target_back;
            buf[i * 2U] = (uint8_t)(pixel & 0xFFU);
            buf[i * 2U + 1U] = (uint8_t)(pixel >> 8);
        }
    }
}

/**
 * @brief 从 W25Q64 流式读取图片并写入 LCD(可选透明色替换)
 *
 * @param x,y         绘制起始坐标
 * @param img         图片描述(宽高 + littlefs 路径)
 * @param transparency 是否把接近白色的像素替换为 target_back
 * @param target_back 透明替换的目标色
 *
 * @note 双缓冲的核心: 一块数据正由 DMA 发送时, CPU 同时从 SPI1 读下一块。
 *       SPI1 与 SPI3 同为 21MHz, 因此全屏图的读取时间可以被完全隐藏。
 *       heap 分配失败时自动退化为单缓冲串行, 功能不受影响。
 */
static void ST7789_DrawImage_Stream(uint16_t x, uint16_t y, const Image_t *img,
                                    bool transparency, uint16_t target_back)
{
    if (img == NULL)
        return;

    uint16_t w = img->width;
    uint16_t h = img->height;

    if (x > WIDTH - 1 || y > HEIGHT - 1 || (uint32_t)x + w > WIDTH || (uint32_t)y + h > HEIGHT)
        return;

    /* 资源缺失时填背景色, 保证画面结构不塌也不花屏 */
    if (!Asset_ImageOk(img))
    {
        ST7789_Fill_Color(x, y, x + w - 1, y + h - 1, transparency ? target_back : COLOR_BLACK);
        return;
    }

    lfs_file_t f;
    if (Asset_ImageOpen(img, &f) != 0)
        return;

    uint32_t total_px = (uint32_t)w * (uint32_t)h;
    bool dbl = (s_picBuf[0] != NULL && s_picBuf[1] != NULL);
    uint32_t blk = dbl ? (uint32_t)PIC_BLK_PX : (uint32_t)(sizeof(s_scratch) / 2);
    uint8_t *buf[2];

    if (dbl) // 双缓冲: 发送与读取并行
    {
        buf[0] = s_picBuf[0];
        buf[1] = s_picBuf[1];
    }
    else // 回退: 复用字符串渲染缓冲, 串行收发
    {
        buf[0] = s_scratch;
        buf[1] = s_scratch;
    }

    ST7789_SetWindow(x, y, x + w - 1, y + h - 1);
    ST7789_SPI_SetDataSize(16);
    GPIO_ResetBits(ST7789_CS_PORT, ST7789_CS_PIN);
    GPIO_SetBits(ST7789_DC_PORT, ST7789_DC_PIN);

    uint32_t remain = total_px;
    int cur = 0;

    /* 预读第 0 块: 此时 DMA 尚未开始, 这一段无法并行 */
    uint32_t n0 = (remain > blk) ? blk : remain;
    if (Asset_ImageRead(&f, buf[0], n0 * 2U) != 0)
    {
        remain = 0;
    }
    else if (transparency)
    {
        ST7789_PixTransparent(buf[0], n0, target_back);
    }

    while (remain > 0)
    {
        uint32_t n = (remain > blk) ? blk : remain;

        ST7789_DMA_Start(buf[cur], n); // 非阻塞启动发送
        remain -= n;

        int nxt = cur ^ 1;
        if (remain > 0)
        {
            /* 与上面的 DMA 并行: CPU 从 SPI1 把下一块读进来 */
            uint32_t n2 = (remain > blk) ? blk : remain;

            if (Asset_ImageRead(&f, buf[nxt], n2 * 2U) != 0)
            {
                remain = 0;
            }
            else if (transparency)
            {
                ST7789_PixTransparent(buf[nxt], n2, target_back);
            }
        }

        if (!ST7789_DMA_WaitDone())
            break;

        cur = nxt;
    }

    ST7789_Wait_BSY();
    GPIO_SetBits(ST7789_CS_PORT, ST7789_CS_PIN);
    Asset_ImageClose(&f);
}

/**
 * @brief 写入GRAM内存
 * @param data 要写入的数据指针
 * @param len 要写入的数据长度(字节)
 * @param increase 是否自动增加内存指针,填充纯色时为false,非纯色时为true
 */
static void ST7789_Write_Gram(const uint8_t data[], uint32_t len, bool increase)
{
    ST7789_SPI_SetDataSize(16); // 设置SPI3数据宽度为半字

    GPIO_ResetBits(ST7789_CS_PORT, ST7789_CS_PIN);
    GPIO_SetBits(ST7789_DC_PORT, ST7789_DC_PIN);

    ST7789_DMA_Pump(data, len >> 1, increase);

    ST7789_Wait_BSY();
    GPIO_SetBits(ST7789_CS_PORT, ST7789_CS_PIN);
}

/* ============ 底层功能 ============ */

static void ST7789_Rest(void)
{
    GPIO_ResetBits(ST7789_RESET_PORT, ST7789_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(20));
    GPIO_SetBits(ST7789_RESET_PORT, ST7789_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void ST7789_Set_Backlight(bool state)
{
    GPIO_WriteBit(ST7789_BACKLIGHT_PORT, ST7789_BACKLIGHT_PIN, state ? Bit_SET : Bit_RESET);
}

/**
 * @brief 屏幕电源开关
 * @param on true=显示开(0x29)+背光; false=背光灭+显示睡眠(0x28, 显存保留)
 */
void ST7789_Display_Power(bool on)
{
    if (on)
    {
        ST7789_Write_Reg(0x29, NULL, 0); /* 显示开 */
        ST7789_Set_Backlight(true);
    }
    else
    {
        ST7789_Set_Backlight(false);
        ST7789_Write_Reg(0x28, NULL, 0); /* 显示关(显存保留, 唤醒无需重初始化) */
    }
}

static void ST7789_Display_Init(void)
{
    ST7789_Rest();
    vTaskDelay(pdMS_TO_TICKS(20));
    ST7789_Write_Reg(0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    ST7789_Write_Reg(0x36, (uint8_t[]){0x00}, 1);
    ST7789_Write_Reg(0x3A, (uint8_t[]){0x55}, 1);
    ST7789_Write_Reg(0xB2, (uint8_t[]){0x0C, 0x0C, 0x00, 0x33, 0x33}, 5);
    ST7789_Write_Reg(0xB7, (uint8_t[]){0x46}, 1);
    ST7789_Write_Reg(0xBB, (uint8_t[]){0x1B}, 1);
    ST7789_Write_Reg(0xC0, (uint8_t[]){0x2C}, 1);
    ST7789_Write_Reg(0xC2, (uint8_t[]){0x01}, 1);
    ST7789_Write_Reg(0xC3, (uint8_t[]){0x0F}, 1);
    ST7789_Write_Reg(0xC4, (uint8_t[]){0x20}, 1);
    ST7789_Write_Reg(0xC6, (uint8_t[]){0x0F}, 1);
    ST7789_Write_Reg(0xD0, (uint8_t[]){0xA4, 0xA1}, 2);
    ST7789_Write_Reg(0xD6, (uint8_t[]){0xA1}, 1);
    ST7789_Write_Reg(0xE0, (uint8_t[]){0xF0, 0x00, 0x06, 0x04, 0x05, 0x05, 0x31, 0x44, 0x48, 0x36, 0x12, 0x12, 0x2B, 0x34}, 14);
    ST7789_Write_Reg(0xE1, (uint8_t[]){0xF0, 0x0B, 0x0F, 0x0F, 0x0D, 0x26, 0x31, 0x43, 0x47, 0x38, 0x14, 0x14, 0x2C, 0x32}, 14);
    // ST7789_Write_Reg(0x21, NULL, 0);

    /* 打开显示前先把GRAM刷黑: ST7789复位不清GRAM, 若不清, 打开显示与背光
     * 后会先亮出复位前残留在GRAM里的旧画面(例如上次加载完的主页), 一直挂到
     * 调用方画完第一帧为止 —— 表现为复位后约1秒的残留。 */
    ST7789_Fill_Color(0, 0, WIDTH - 1, HEIGHT - 1, 0x0000);

    ST7789_Write_Reg(0x29, NULL, 0); // 打开显示
    vTaskDelay(pdMS_TO_TICKS(10));
    ST7789_Set_Backlight(true); // 开启背光
}

static void ST7789_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    GPIO_StructInit(&GPIO_InitStructure);

    GPIO_SetBits(GPIOE, ST7789_CS_PIN | ST7789_RESET_PIN | ST7789_DC_PIN);
    GPIO_ResetBits(ST7789_BACKLIGHT_PORT, ST7789_BACKLIGHT_PIN);
    GPIO_InitStructure.GPIO_Pin = ST7789_CS_PIN | ST7789_RESET_PIN | ST7789_DC_PIN | ST7789_BACKLIGHT_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(GPIOE, &GPIO_InitStructure);

    GPIO_PinAFConfig(ST7789_SCLK_PORT, GPIO_PinSource10, GPIO_AF_SPI3);
    GPIO_PinAFConfig(ST7789_MISO_PORT, GPIO_PinSource11, GPIO_AF_SPI3);
    GPIO_PinAFConfig(ST7789_MOSI_PORT, GPIO_PinSource12, GPIO_AF_SPI3);

    GPIO_InitStructure.GPIO_Pin = ST7789_SCLK_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_Speed = GPIO_High_Speed;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(ST7789_SCLK_PORT, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = ST7789_MOSI_PIN | ST7789_MISO_PIN;
    GPIO_Init(GPIOC, &GPIO_InitStructure);
}

static void ST7789_SPI_Init(void)
{
    SPI_InitTypeDef SPI_InitStructure;
    SPI_StructInit(&SPI_InitStructure);

    SPI_InitStructure.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    SPI_InitStructure.SPI_Mode = SPI_Mode_Master;
    SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;
    SPI_InitStructure.SPI_CPHA = SPI_CPHA_1Edge;
    SPI_InitStructure.SPI_CPOL = SPI_CPOL_Low;
    SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_2; /* SPI3=42MHz → 21MHz */
    SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;
    SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;

    SPI_Init(SPI3, &SPI_InitStructure);
    SPI_DMACmd(SPI3, SPI_I2S_DMAReq_Tx, ENABLE);
    SPI_Cmd(SPI3, ENABLE);

    s_spi_datasize = 8;
}

static bool Is_in_Screen(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    if (x1 >= WIDTH || x2 >= WIDTH || y1 >= HEIGHT || y2 >= HEIGHT)
        return false;
    else if (x1 > x2 || y1 > y2)
        return false;
    return true;
}

/**
 * @brief 判断字符是否为GB2312中文
 * @param ch 字符
 * @return true 中文
 * @return false 非中文
 */
static bool Is_GB2312(char ch)
{
    return (ch >= 0xA1 && ch <= 0xF7);
}

void ST7789_Init(void)
{
    ST7789_GPIO_Init();
    ST7789_SPI_Init();
    ST7789_DMA_Init();
    ST7789_Display_Init();
    (void)ST7789_PicBuf_Init(); // 分配图片乒乓缓冲(失败则内部退化为单缓冲)
}

/**
 * @brief 填充颜色
 *
 * @param x1 坐标1
 * @param y1 坐标1
 * @param x2 坐标2
 * @param y2 坐标2
 * @param color 颜色
 */
void ST7789_Fill_Color(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    if (!Is_in_Screen(x1, y1, x2, y2))
        return;

    uint32_t size = (x2 - x1 + 1) * (y2 - y1 + 1);

    ST7789_SetWindow(x1, y1, x2, y2); // 设置填充区域

    ST7789_Write_Gram((uint8_t *)&color, size * 2, false);
}

/**
 * @brief 整行文字渲染: 把同一行的连续字符合成一次窗口+DMA突发
 * @param x,y        起始坐标
 * @param str        文本(GB2312)
 * @param color_font 前景色
 * @param color_back 背景色
 * @param font       字体
 */
void ST7789_Write_String(uint16_t x, uint16_t y, char *str, uint16_t color_font, uint16_t color_back, const Font_t *font)
{
    if (font == NULL || str == NULL || font->size == 0 || font->size > SCRATCH_H_PX)
        return;

    uint16_t size = font->size; // 字体大小（像素）
    const char *p = str;

    while (*p)
    {
        if (y + size > HEIGHT)
            return;

        /* 收集当前行(run): 只记录每个字符的资源位置与宽度, 这一步不碰 Flash */
        typedef struct
        {
            uint8_t fid;      // 资源文件 ID; ASSET_FID_NONE = 未收录, 按背景填充
            uint32_t off;     // 字模在该文件中的字节偏移
            uint16_t w;       // 该字符的像素宽度（中文=size，英文=size/2）
            uint16_t offset;  // 该字符在当前行内的起始列偏移（相对于行首 x）
        } GRef_t;
        GRef_t g[40];      // 当前行字符引用数组
        uint16_t n = 0;    // 当前行已收集的字符个数
        uint16_t xpos = x; // 当前行的“虚拟光标”位置（像素坐标）

        while (*p && n < 40)
        {
            bool is_cn = Is_GB2312(*p);            // 是否为中文
            uint16_t gw = is_cn ? size : size / 2; // 当前字符的像素宽度（中文=size，英文=size/2）
            uint8_t fid = ASSET_FID_NONE;
            uint32_t off = 0;

            if (is_cn)
            {
                /* 汉字用 GB2312 区位码直接定位: 偏移 O(1) 算出,
                 * 不再像过去那样按名字线性遍历整张字库表。 */
                if (font->cn_id != ASSET_FID_NONE)
                {
                    uint8_t qu = (uint8_t)p[0]; // 区码
                    uint8_t we = (uint8_t)p[1]; // 位码

                    if (qu >= 0xB0U && qu <= 0xD7U && we >= 0xA1U && we <= 0xFEU)
                    {
                        uint32_t idx = (uint32_t)(qu - 0xB0U) * 94U + (uint32_t)(we - 0xA1U);
                        if (idx < ASSET_GLYPH_COUNT)
                        {
                            fid = font->cn_id;
                            off = idx * font->cn_bytes;
                        }
                    }
                }
            }
            else if ((uint8_t)*p >= 0x20 && (uint8_t)*p <= 0x7E && font->ascii_id != ASSET_FID_NONE) // 是否为ASCII字符,ASCII 可打印字符（0x20 ~ 0x7E）
            {
                uint16_t aw = size / 2;                           // ASCII 字符宽度
                uint16_t ob = (aw + 7) / 8;                       // ASCII 每行点阵占用的字节数(向上取整)
                fid = font->ascii_id;                             // ASCII 表所在的资源文件
                off = (uint32_t)((uint8_t)*p - 0x20) * ob * size; // 计算ASCII字符模型偏移
            }

            if (xpos + gw > WIDTH) // 放不下当前行, 该字符换到下一行
                break;

            g[n].fid = fid;         // 记录资源文件(ASSET_FID_NONE 表示未收录)
            g[n].off = off;         // 记录字模偏移
            g[n].w = gw;            // 记录当前字符的像素宽度（中文=size，英文=size/2）
            g[n].offset = xpos - x; // 记录当前字符在当前行内的起始列偏移（相对于行首 x）
            n++;                    // 当前行已收集的字符个数++
            xpos += gw;             // 更新当前字符的“虚拟光标”位置（像素坐标）
            p += is_cn ? 2 : 1;     // 移动到下一个字符

            if ((xpos - x) + gw > WIDTH - 1) // 与逐字符绘制一致的换行判定
                break;
        }

        uint16_t run_px = xpos - x; // 当前行已收集的字符像素宽度（像素）
        if (run_px > 0)
        {
            /* 逐字符读字模再铺进缓冲。
             * 外层是字符、内层是行: 每个字模只从 W25Q64 读一次, 先落在
             * s_glyph, 再按行散写进 s_scratch 的对应列偏移。 */
            uint8_t *buf = s_scratch; // 指向全局缓冲区的起点

            for (uint16_t k = 0; k < n; k++)
            {
                uint16_t gw2 = g[k].w;       // 当前字符的像素宽度（中文=size，英文=size/2）
                uint16_t ob = (gw2 + 7) / 8; // 当前字符每行点阵占用的字节数(向上取整)
                const uint8_t *mb = NULL;    // NULL = 未收录, 按背景色填充

                if (g[k].fid != ASSET_FID_NONE)
                {
                    uint32_t need = (uint32_t)ob * size; // 该字模字节数, 最大 144(48号ASCII的3*48)

                    if (need <= sizeof(s_glyph) &&
                        Asset_ReadFont(g[k].fid, g[k].off, s_glyph, need) == 0)
                    {
                        mb = s_glyph;
                    }
                }

                for (uint16_t row = 0; row < size; row++) // 从上到下扫描像素行
                {
                    uint8_t *dst = buf + ((uint32_t)row * run_px + g[k].offset) * 2; // 当前字符第row行的起始位置([lo][hi])

                    if (mb != NULL)
                    {
                        const uint8_t *sr = mb + (uint32_t)row * ob; // 第 row 行的点阵数据
                        for (uint16_t c = 0; c < gw2; c++)
                        {
                            uint16_t color = (sr[c >> 3] & (1 << (c & 7))) ? color_font : color_back;
                            *dst++ = (uint8_t)(color & 0xFF); // 写入低字节
                            *dst++ = (uint8_t)(color >> 8);   // 写入高字节
                        }
                    }
                    else // 字库未收录该字符, 按背景色填充
                    {
                        for (uint16_t c = 0; c < gw2; c++)
                        {
                            *dst++ = (uint8_t)(color_back & 0xFF);
                            *dst++ = (uint8_t)(color_back >> 8);
                        }
                    }
                }
            }

            ST7789_SetWindow(x, y, x + run_px - 1, y + size - 1);
            ST7789_Write_Gram(s_scratch, (uint32_t)run_px * size * 2, true);
        }

        if (*p == '\0')
            return;

        x = 0;
        y += size;
        if (y > HEIGHT - 1)
            return;
    }
}

/**
 * @brief 绘制图片(像素数据来自 W25Q64)
 * @param x,y   绘制起始坐标
 * @param image 图片结构体指针(只含宽高与 littlefs 路径)
 */
void ST7789_Draw_Picture(uint16_t x, uint16_t y, const Image_t *image)
{
    ST7789_DrawImage_Stream(x, y, image, false, COLOR_BLACK);
}

/**
 * @brief 判断两个 RGB565 颜色是否在阈值内接近
 * @param c1, c2  RGB565 颜色值
 * @param threshold 允许的最大差值（0~255，建议 30~50）
 * @return true 表示相近
 */
static bool Color_IsClose(uint16_t c1, uint16_t c2, uint8_t threshold)
{
    // 分离 RGB 分量（RGB565：R[15:11], G[10:5], B[4:0]）
    uint8_t r1 = (c1 >> 11) & 0x1F;
    uint8_t g1 = (c1 >> 5) & 0x3F;
    uint8_t b1 = c1 & 0x1F;
    uint8_t r2 = (c2 >> 11) & 0x1F;
    uint8_t g2 = (c2 >> 5) & 0x3F;
    uint8_t b2 = c2 & 0x1F;

    // 计算分量绝对差值（归一化到 0~255 更直观）
    int dr = abs(r1 - r2) * 8; // 5bit -> 255
    int dg = abs(g1 - g2) * 4; // 6bit -> 252
    int db = abs(b1 - b2) * 8;

    // 简单阈值：各分量差值均小于 threshold
    return (dr < threshold && dg < threshold && db < threshold);
}

/**
 * @brief 绘制图片，自动检测并替换背景色(透明效果)
 *        改色在缓冲内原地完成, 之后再走 DMA 发送
 * @param x,y        绘制起始坐标
 * @param image       图片结构体指针
 * @param target_back 目标背景色（例如 COLOR_WHITE）
 */
void ST7789_Draw_Picture_AutoTransparent(uint16_t x, uint16_t y, const Image_t *image, uint16_t target_back)
{
    ST7789_DrawImage_Stream(x, y, image, true, target_back);
}
