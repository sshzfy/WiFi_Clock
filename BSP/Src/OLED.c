#include "OLED.h"

static Soft_I2C_t *OLED_I2C = NULL;

/* ========= 硬件底层函数 =========*/

/**
 * @brief 写入OLED命令
 *
 * @param cmd 要写入的命令
 * */
static void OLED_WriteCmd(uint8_t cmd)
{
    if (OLED_I2C == NULL)
        return;
    Soft_I2C_Send_Bytes(OLED_I2C, OLED_ADDR, OLED_CMD_BYTE, &cmd, 1);
}

/**
 * @brief 写入OLED数据
 *
 * @param data 要写入的数据
 * */
static void OLED_WriteData(uint8_t data)
{
    if (OLED_I2C == NULL)
        return;
    Soft_I2C_Send_Bytes(OLED_I2C, OLED_ADDR, OLED_DATA_BYTE, &data, 1);
}

/**
 * @brief 设置OLED光标位置
 *
 * @param y 要设置的光标位置的行
 * @param x 要设置的光标位置的列
 * */
static void OLED_SetCursor(uint8_t page, uint8_t col)
{
    OLED_WriteCmd(0xB0 | page);                /* 设置页地址 */
    OLED_WriteCmd(0x10 | ((col & 0xF0) >> 4)); /* 列高4位 */
    OLED_WriteCmd(0x00 | (col & 0x0F));        /* 列低4位 */
}

/* ========= 应用函数 =========*/

/**
 * @brief 清除OLED显示
 * */
void OLED_Clear(void)
{
    for (uint8_t page = 0; page < 8; page++)
    {
        OLED_SetCursor(page, 0);
        for (uint8_t i = 0; i < OLED_WIDTH; i++)
        {
            OLED_WriteData(0x00);
        }
    }
}

/**
 * @brief 写入OLED字符
 *
 * @param ch 要写入的字符
 * */
void OLED_WriteChar(uint8_t x, uint8_t y, char ch, const Font_t *font)
{
    if (font == NULL || font->ascii_model == NULL)
        return;
    if (x >= OLED_WIDTH || y >= OLED_HEIGHT)
        return;

    uint16_t size = font->size; // 字体高度（像素）
    uint16_t width = size / 2;  // ASCII字符宽度 = 高度/2
    /* 裁剪到屏幕边界 */
    if (x + width > OLED_WIDTH)
        width = OLED_WIDTH - x;
    if (y + size > OLED_HEIGHT)
        size = OLED_HEIGHT - y;

    uint8_t index = (uint8_t)ch - ' ';
    if (index >= 95)
        return; /* 只处理可见ASCII */

    /* 计算每行占用的字节数（向上取整） */
    uint16_t bytes_per_row = (width + 7) / 8;
    const uint8_t *model = font->ascii_model + index * bytes_per_row * size;

    /* 临时位图（最大48x24），存储每个像素的亮灭（0/1） */
    static uint8_t bitmap[48][24]; // 最大高度48，宽度24（对应size=48）
    for (uint16_t row = 0; row < size; row++)
    {
        for (uint16_t col = 0; col < width; col++)
        {
            uint16_t byte_idx = row * bytes_per_row + col / 8;
            uint8_t bit = col % 8;
            bitmap[row][col] = (model[byte_idx] >> bit) & 0x01;
        }
    }

    /* 按页写入（每页8行） */
    uint8_t start_page = y / 8;
    uint8_t end_page = (y + size - 1) / 8;
    for (uint8_t page = start_page; page <= end_page; page++)
    {
        uint8_t page_y = page * 8; // 页的起始行
        uint8_t row_start = (page_y > y) ? 0 : (y - page_y);
        uint8_t row_end = (page_y + 7) < (y + size) ? 7 : (y + size - 1 - page_y);

        OLED_SetCursor(page, x);
        /* 逐列构造该页的字节数据 */
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
 *
 * @param x 要写入的字符串的起始列
 * @param y 要写入的字符串的起始行
 * @param str 要写入的字符串
 * @param font 要写入的字符串的字体
 * */
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

/**
 * @brief 初始化OLED
 *
 * @param i2c I2C句柄
 * */
void OLED_Init(Soft_I2C_t *i2c)
{
    OLED_I2C = i2c;
    delay_ms(100);

    OLED_WriteCmd(0xAE); /*关闭显示 */

    /* 标准化初始序列 */
    OLED_WriteCmd(0xD5);
    OLED_WriteCmd(0x80); /* 时钟分频 */
    OLED_WriteCmd(0xA8);
    OLED_WriteCmd(0x3F); /* 64行复用 */
    OLED_WriteCmd(0xD3);
    OLED_WriteCmd(0x00); /* 无偏移 */
    OLED_WriteCmd(0x40); /* 起始行 = 0 */
    OLED_WriteCmd(0xA1); /* 段重映射（左右镜像） */
    OLED_WriteCmd(0xC8); /* COM 反向扫描 */
    OLED_WriteCmd(0xDA);
    OLED_WriteCmd(0x12); /* 交替 COM 配置 */
    OLED_WriteCmd(0x81);
    OLED_WriteCmd(0xCF); /* 对比度 */
    OLED_WriteCmd(0xD9);
    OLED_WriteCmd(0xF1); /* 预充电 */
    OLED_WriteCmd(0xDB);
    OLED_WriteCmd(0x30); /* VCOMH 电平 */
    OLED_WriteCmd(0xA4); /* 正常显示 (根据 RAM) */
    OLED_WriteCmd(0xA6); /* 非反显 */
    OLED_WriteCmd(0x8D);
    OLED_WriteCmd(0x14); /* 开启内部 DC-DC (电荷泵) */
    OLED_WriteCmd(0xAF); /* 开启显示 */

    OLED_Clear();
}
