#include "OLED.h"

/* ================ 屏幕信息 ================
 * - 分辨率: 128x64
 * - 显存 GDDRAM： 128 列 x 64 行 = 1024 字节
 *   显存分 8 页，每页 128 字节
 * - 每个数据字节对应同一列的 8 个行像素（每页 8 行）
 *   D0：页内最上面一行，D7：页内最下面一行
 * ================================ */

static uint8_t s_oled_glyph[ASSET_GLYPH_BYTES_MAX]; // 单字模缓冲: 全部字模里最大 144 字节(48号ASCII = 3 字节/行 * 48 行)
static uint8_t s_oled_zeros[OLED_WIDTH] = {0};      // 清屏用的全零页: 一次 I2C 事务写满一整页(128 列)

Soft_I2C_t oled_i2c =
    {
        .SCL_Port = OLED_GPIO_PORT,
        .SDA_Port = OLED_GPIO_PORT,
        .SCL_Pin = OLED_GPIO_SCL_PIN,
        .SDA_Pin = OLED_GPIO_SDA_PIN,
};

static void OLED_WriteCmd(uint8_t cmd);
static void OLED_WriteData(uint8_t data);
static void OLED_SetCursor(uint8_t page, uint8_t col);

/**
 * @brief 写入OLED命令字节
 * @param cmd 要写入的命令字节
 * */
static void OLED_WriteCmd(uint8_t cmd)
{
    if (&oled_i2c == NULL)
        return;
    Soft_I2C_Send_Bytes(&oled_i2c, OLED_ADDR, OLED_CMD_BYTE, &cmd, 1);
}

/**
 * @brief 写入OLED数据字节
 * @param data 要写入的数据字节
 * */
static void OLED_WriteData(uint8_t data)
{
    if (&oled_i2c == NULL)
        return;
    Soft_I2C_Send_Bytes(&oled_i2c, OLED_ADDR, OLED_DATA_BYTE, &data, 1);
}

/**
 * @brief 设置OLED光标位置
 * @param page 要设置的光标位置的页
 * @param col 要设置的光标位置的列
 * */
static void OLED_SetCursor(uint8_t page, uint8_t col)
{
    OLED_WriteCmd(0xB0 | page);                // 设置页地址
    OLED_WriteCmd(0x10 | ((col & 0xF0) >> 4)); // 设置列高4位
    OLED_WriteCmd(0x00 | (col & 0x0F));        // 设置列低4位
}

/**
 * @brief 清除OLED显示
 * @note 按页整块写: 每页一次 I2C 事务写 128 字节, 共 8 次。
 *       原先逐列发 1024 次单字节事务, 上电清屏要花约 240ms。
 * */
void OLED_Clear(void)
{
    for (uint8_t page = 0; page < 8; page++)
    {
        OLED_SetCursor(page, 0);
        Soft_I2C_Send_Bytes(&oled_i2c, OLED_ADDR, OLED_DATA_BYTE, s_oled_zeros, OLED_WIDTH);
    }
}

/**
 * @brief 写入OLED字符
 * @param x 要写入的字符的 x 坐标，y 要写入的字符的 y 坐标
 * @param ch 要写入的字符
 * @param font 字体指针
 * */
void OLED_WriteChar(uint8_t x, uint8_t y, char ch, const Font_t *font)
{
    if (font == NULL || font->ascii_id == ASSET_FID_NONE)
        return;
    if (x >= OLED_WIDTH || y >= OLED_HEIGHT)
        return;

    /* 只处理可见ASCII （0x20 ~ 0x7E） */
    uint8_t index = (uint8_t)ch - ' ';
    if (index >= 95)
        return;

    uint16_t raw_size = font->size;         // 字模原始高度
    uint16_t raw_width = raw_size / 2;      // 字模原始宽度
    uint16_t raw_bpr = (raw_width + 7) / 8; // 字模每行占用的字节数

    /* 读出整个字模 */
    uint32_t need = (uint32_t)raw_bpr * raw_size; // 一个字符的字节数
    if (need == 0U || need > sizeof(s_oled_glyph))
        return;
    if (Asset_ReadFont(font->ascii_id, (uint32_t)index * need, s_oled_glyph, need) != 0)
        return;

    /* 绘制范围裁剪到屏幕内 */
    uint16_t size = raw_size;   // 实际绘制的字符高度（行）
    uint16_t width = raw_width; // 实际绘制的字符宽度（列）
    if (x + width > OLED_WIDTH)
        width = OLED_WIDTH - x;
    if (y + size > OLED_HEIGHT)
        size = OLED_HEIGHT - y;

    /* 临时位图（最大48x24），存储每个像素的亮灭（0/1） */
    static uint8_t bitmap[48][24];
    for (uint16_t row = 0; row < size; row++)
    {
        for (uint16_t col = 0; col < width; col++)
        {
            uint16_t byte_idx = row * raw_bpr + col / 8;               // row 行 col 列像素所在字节
            uint8_t bit = col % 8;                                     // 像素所在位
            bitmap[row][col] = (s_oled_glyph[byte_idx] >> bit) & 0x01; // 提取该位的值（0/1）
        }
    }

    /* 按页写入（每页8行） */
    uint8_t start_page = y / 8;
    uint8_t end_page = (y + size - 1) / 8;
    for (uint8_t page = start_page; page <= end_page; page++)
    {
        uint8_t page_y = page * 8;                                                 // 当前页第一行对应的屏幕行号
        uint8_t row_start = (page_y > y) ? 0 : (y - page_y);                       // 当前页起始行号
        uint8_t row_end = (page_y + 7) < (y + size) ? 7 : (y + size - 1 - page_y); // 当前页结束行号

        /* 逐列构造该页的字节数据 */
        OLED_SetCursor(page, x);
        for (uint16_t col = 0; col < width; col++)
        {
            uint8_t byte = 0;
            for (uint8_t row = row_start; row <= row_end; row++)
            {
                if (bitmap[page_y + row - y][col])
                    byte |= (1 << row);
            }
            OLED_WriteData(byte);
        }
    }
}

/**
 * @brief 写入OLED字符串
 * @param x 要写入的字符串的起始列，y 要写入的字符串的起始行
 * @param str 要写入的字符串
 * @param font 要写入的字符串的字体
 */
void OLED_Write_String(uint8_t x, uint8_t y, const char *str, const Font_t *font)
{
    if (font == NULL)
        return;

    uint16_t char_width = font->size / 2;
    while (*str)
    {
        OLED_WriteChar(x, y, *str, font);
        x += char_width;
        if (x >= OLED_WIDTH)
            break;
        str++;
    }
}

void OLED_ShowClock(uint8_t hh, uint8_t mm, uint16_t year, uint8_t mon, uint8_t day)
{
    char buf[16];

    /* HH:MM: Font_48 字宽24, 5字符=120px, 居中 */
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)hh, (unsigned)mm);
    OLED_Write_String((OLED_WIDTH - 5 * (Font_48.size / 2)) / 2, 0, buf, &Font_48);

    /* 日期: Font_16 字宽8, 10字符=80px, 居中 */
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u", (unsigned)year, (unsigned)mon, (unsigned)day);
    OLED_Write_String((OLED_WIDTH - 10 * (Font_16.size / 2)) / 2, 48, buf, &Font_16);
}

/** @brief 开启显示 * */
void OLED_Display_On(void)
{
    OLED_WriteCmd(0xAF); // 开启显示
    OLED_Clear();        // 清屏
}

/** @brief 关闭显示 * */
void OLED_Display_Off(void)
{
    OLED_Clear();        // 清屏
    OLED_WriteCmd(0xAE); // 关闭显示
}

void OLED_Init(void)
{
    Soft_I2C_Init(&oled_i2c);
    delay_ms(100);

    OLED_WriteCmd(0xAE); // 关闭显示

    /* ================ 标准化初始序列 ================ */

    /* 0xD5: 设置显示时钟分频比 / 振荡器频率
     * 参数 0x80: 高4位=振荡器频率(FOSC)，低4位=分频比
     *            0x80 -> 分频比=1，DCLK = FOSC (约370kHz) */
    OLED_WriteCmd(0xD5);
    OLED_WriteCmd(0x80);
    /* 0xA8: 设置多路复用比 (Multiplex Ratio)
     * 参数 0x3F: 复用比 = 64，对应 64 行 COM */
    OLED_WriteCmd(0xA8);
    OLED_WriteCmd(0x3F);
    /* 0xD3: 设置显示偏移 (Display Offset)
     * 参数 0x00: GDDRAM 第0行与 COM0 无偏移 */
    OLED_WriteCmd(0xD3);
    OLED_WriteCmd(0x00);
    /* 0x40: 设置显示起始行 (Set Display Start Line)
     * 0x40 为基址，表示起始行 = 0 */
    OLED_WriteCmd(0x40);
    /* 0xA1: 设置段重映射 (Segment Re-map)
     * 0xA1: SEG0 -> 列127，水平镜像
     * (0xA0 为正常方向) */
    OLED_WriteCmd(0xA1);
    /* 0xC8: 设置 COM 输出扫描方向
     * 0xC8: 反向扫描，COM[N-1] -> COM0，垂直翻转
     * (0xC0 为正向扫描) */
    OLED_WriteCmd(0xC8);
    /* 0xDA: 设置 COM 引脚硬件配置
     * 参数 0x12: 启用交替 COM 配置，禁用左右 COM 重映射
     *            128x64 屏典型值 */
    OLED_WriteCmd(0xDA);
    OLED_WriteCmd(0x12);
    /* 0x81: 设置对比度 (Contrast)
     * 参数 0xCF: 0~255，值越大越亮 (0xCF = 207) */
    OLED_WriteCmd(0x81);
    OLED_WriteCmd(0xCF);
    /* 0xD9: 设置预充电周期 (Pre-charge Period)
     * 参数 0xF1: 高4位=放电周期，低4位=充电周期
     *            内部电荷泵供电时的常用值 */
    OLED_WriteCmd(0xD9);
    OLED_WriteCmd(0xF1);
    /* 0xDB: 设置 VCOMH 取消选择电平
     * 参数 0x30: VCOMH ≈ 0.83 × Vcc */
    OLED_WriteCmd(0xDB);
    OLED_WriteCmd(0x30);
    /* 0xA4: 显示跟随 GDDRAM 内容 (正常模式)
     * 0xA4: 按显存内容显示
     * (0xA5: 忽略显存，全屏强制点亮) */
    OLED_WriteCmd(0xA4);
    /* 0xA6: 设置正常显示 (非反色)
     * 0xA6: 亮像素 = RAM 中的 1
     * (0xA7: 反色显示) */
    OLED_WriteCmd(0xA6);
    /* 0x8D: 设置电荷泵 (Charge Pump)
     * 参数 0x14: 使能内部 DC-DC 电荷泵
     *            OLED 发光所需高压由此产生 */
    OLED_WriteCmd(0x8D);
    OLED_WriteCmd(0x14);

    /* 先清屏, 再开显示: 清除上一次留下的内容 */
    OLED_Clear();
    OLED_WriteCmd(0xAF); // 开启显示
}
