#include "OLED.h"
#include "Asset.h"

/* 单字模缓冲: 全部字模里最大 144 字节(48号ASCII = 3 字节/行 * 48 行) */
static uint8_t s_oled_glyph[ASSET_GLYPH_BYTES_MAX];

/* 清屏用的全零页: 一次 I2C 事务写满一整页(128 列) */
static uint8_t s_oled_zeros[OLED_WIDTH];

Soft_I2C_t oled_i2c = {
    .SCL_Port = GPIOB,
    .SDA_Port = GPIOB,
    .SCL_Pin = GPIO_Pin_6,
    .SDA_Pin = GPIO_Pin_7,
};
/* ================ 硬件底层函数 ================ */

/**
 * @brief 写入OLED命令
 *
 * @param cmd 要写入的命令
 * */
static void OLED_WriteCmd(uint8_t cmd)
{
    if (&oled_i2c == NULL)
        return;
    Soft_I2C_Send_Bytes(&oled_i2c, OLED_ADDR, OLED_CMD_BYTE, &cmd, 1);
}

/**
 * @brief 写入OLED数据
 *
 * @param data 要写入的数据
 * */
static void OLED_WriteData(uint8_t data)
{
    if (&oled_i2c == NULL)
        return;
    Soft_I2C_Send_Bytes(&oled_i2c, OLED_ADDR, OLED_DATA_BYTE, &data, 1);
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

/* ================ 应用函数 ================ */

/**
 * @brief 清除OLED显示
 *
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
 *
 * @param ch 要写入的字符
 * */
void OLED_WriteChar(uint8_t x, uint8_t y, char ch, const Font_t *font)
{
    if (font == NULL || font->ascii_id == ASSET_FID_NONE)
        return;
    if (x >= OLED_WIDTH || y >= OLED_HEIGHT)
        return;

    uint8_t index = (uint8_t)ch - ' ';
    if (index >= 95)
        return; /* 只处理可见ASCII */

    uint16_t full_size = font->size;          // 字模原始高度
    uint16_t full_width = full_size / 2;      // 字模原始宽度
    uint16_t full_bpr = (full_width + 7) / 8; // 字模每行占用的字节数

    /* 先把整个字模读出来。注意偏移与布局都按"原始"尺寸算,
     * 与下面的屏幕裁剪无关, 否则裁剪后偏移会算错。 */
    uint32_t need = (uint32_t)full_bpr * full_size;
    if (need == 0U || need > sizeof(s_oled_glyph))
        return;
    if (Asset_ReadFont(font->ascii_id, (uint32_t)index * need, s_oled_glyph, need) != 0)
        return;

    /* 绘制范围裁剪到屏幕内 */
    uint16_t size = full_size;
    uint16_t width = full_width;
    if (x + width > OLED_WIDTH)
        width = OLED_WIDTH - x;
    if (y + size > OLED_HEIGHT)
        size = OLED_HEIGHT - y;

    /* 临时位图（最大48x24），存储每个像素的亮灭（0/1） */
    static uint8_t bitmap[48][24]; // 最大高度48，宽度24（对应size=48）
    for (uint16_t row = 0; row < size; row++)
    {
        for (uint16_t col = 0; col < width; col++)
        {
            uint16_t byte_idx = row * full_bpr + col / 8;
            uint8_t bit = col % 8;
            bitmap[row][col] = (s_oled_glyph[byte_idx] >> bit) & 0x01;
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
 * @brief 夜间显示: HH:MM(Font_48居中) + 日期(Font_16居中), 不显示秒
 * */
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

/**
 * @brief 开启OLED显示
 * */
void OLED_Display_On(void)
{
    OLED_WriteCmd(0xAF); /* 开启显示 */
    OLED_Clear();        /* 清屏, 由调用方随后绘制内容 */
}

/**
 * @brief 关闭OLED显示
 * */
void OLED_Display_Off(void)
{
    OLED_Clear();
    OLED_WriteCmd(0xAE); /* 关闭显示 */
}

/**
 * @brief 初始化OLED
 *
 * @param i2c I2C句柄
 * */
void OLED_Init(void)
{
    Soft_I2C_Init(&oled_i2c);
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

    /* 先清屏, 再开显示: 否则从开显示到清屏写完的这段时间,
     * 屏上亮着的是上一次留下的内容(例如刚跑完的裸机测试画面) */
    OLED_Clear();

    OLED_WriteCmd(0xAF); /* 开启显示 */
}
