#include "ST7789.h"

static void ST7789_Write_Reg(uint8_t reg, const uint8_t data[], uint16_t len);
static void ST7789_Rest(void);
static void ST7789_Set_Backlight(bool state);
static void ST7789_Display_Init(void);
static void ST7789_GPIO_Init(void);
static void ST7789_SPI_Init(void);
static bool Is_in_Screen(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
static bool Is_GB2312(char ch);
static void ST7789_SetWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
static void ST7789_Draw_Bitmap(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t color_font, uint16_t color_back, const uint8_t *model);
static void ST7789_Write_Ascii(uint16_t x, uint16_t y, char ch, uint16_t color_font, uint16_t color_back, const Font_t *font);
static void ST7789_Write_Chinese(uint16_t x, uint16_t y, char *ch, uint16_t color_font, uint16_t color_back, const Font_t *font);

static void ST7789_Write_Reg(uint8_t reg, const uint8_t data[], uint16_t len)
{
    GPIO_ResetBits(ST7789_CS_Port, ST7789_CS_Pin);
    GPIO_ResetBits(ST7789_DC_Port, ST7789_DC_Pin);
    SPI_SendData(SPI2, reg);

    while (SPI_GetFlagStatus(SPI2, SPI_FLAG_TXE) == RESET)
        ;
    while (SPI_GetFlagStatus(SPI2, SPI_FLAG_BSY) != RESET)
        ;

    GPIO_SetBits(ST7789_DC_Port, ST7789_DC_Pin);

    for (uint16_t i = 0; i < len; i++)
    {
        SPI_SendData(SPI2, data[i]);
        while (!SPI_GetFlagStatus(SPI2, SPI_FLAG_TXE))
            ;
    }
    while (SPI_GetFlagStatus(SPI2, SPI_FLAG_BSY) != RESET)
        ;
    GPIO_SetBits(ST7789_CS_Port, ST7789_CS_Pin);
}

static void ST7789_Rest(void)
{
    GPIO_ResetBits(ST7789_RESET_Port, ST7789_RESET_Pin);
    delay_us(20);
    GPIO_SetBits(ST7789_RESET_Port, ST7789_RESET_Pin);
    delay_ms(20);
}

static void ST7789_Set_Backlight(bool state)
{
    GPIO_WriteBit(ST7789_BACKLIGHT_Port, ST7789_BACKLIGHT_Pin, state ? Bit_SET : Bit_RESET);
}

static void ST7789_Display_Init(void)
{
    ST7789_Rest();
    ST7789_Write_Reg(0x11, NULL, 0);
    delay_ms(5);

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

    // ST7789_Fill_Color(0, 0, WIDTH - 1, HEIGHT - 1, 0x001F); // 填充背景颜色（显示打开前）
    ST7789_Write_Reg(0x29, NULL, 0); // 打开显示
    ST7789_Set_Backlight(true);      // 开启背光
}

static void ST7789_GPIO_Init(void)
{
    GPIO_PinAFConfig(ST7789_SCLK_Port, GPIO_PinSource13, GPIO_AF_SPI2);
    GPIO_PinAFConfig(ST7789_MOSI_Port, GPIO_PinSource3, GPIO_AF_SPI2);
    GPIO_PinAFConfig(ST7789_MISO_Port, GPIO_PinSource2, GPIO_AF_SPI2);

    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_StructInit(&GPIO_InitStruct);

    GPIO_SetBits(GPIOE, ST7789_CS_Pin | ST7789_RESET_Pin | ST7789_DC_Pin);
    GPIO_ResetBits(ST7789_BACKLIGHT_Port, ST7789_BACKLIGHT_Pin);
    GPIO_InitStruct.GPIO_Pin = ST7789_CS_Pin | ST7789_RESET_Pin | ST7789_DC_Pin | ST7789_BACKLIGHT_Pin;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStruct.GPIO_Speed = GPIO_High_Speed;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(GPIOE, &GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Pin = ST7789_SCLK_Pin;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStruct.GPIO_Speed = GPIO_High_Speed;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(ST7789_SCLK_Port, &GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Pin = ST7789_MOSI_Pin | ST7789_MISO_Pin;
    GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_PinAFConfig(ST7789_SCLK_Port, GPIO_PinSource13, GPIO_AF_SPI2);
    GPIO_PinAFConfig(ST7789_MOSI_Port, GPIO_PinSource3, GPIO_AF_SPI2);
    GPIO_PinAFConfig(ST7789_MISO_Port, GPIO_PinSource2, GPIO_AF_SPI2);
}

static void ST7789_SPI_Init(void)
{
    SPI_InitTypeDef SPI_InitStruct;
    SPI_StructInit(&SPI_InitStruct);

    SPI_InitStruct.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    SPI_InitStruct.SPI_Mode = SPI_Mode_Master;
    SPI_InitStruct.SPI_DataSize = SPI_DataSize_8b;
    SPI_InitStruct.SPI_CPHA = SPI_CPHA_1Edge;
    SPI_InitStruct.SPI_CPOL = SPI_CPOL_Low;
    SPI_InitStruct.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_4;
    SPI_InitStruct.SPI_FirstBit = SPI_FirstBit_MSB;
    SPI_InitStruct.SPI_NSS = SPI_NSS_Soft;

    SPI_Init(SPI2, &SPI_InitStruct);
    SPI_Cmd(SPI2, ENABLE);
}

static bool Is_in_Screen(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    if (x1 >= WIDTH || x2 >= WIDTH || y1 >= HEIGHT || y2 >= HEIGHT)
        return false;
    else if (x1 > x2 || y1 > y2)
        return false;
    return true;
}

static bool Is_GB2312(char ch)
{
    return (ch >= 0xA1 && ch <= 0xF7);
}

static void ST7789_SetWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    ST7789_Write_Reg(0x2A, (uint8_t[]){(x1 >> 8) & 0xFF, (x1 & 0xFF), (x2 >> 8) & 0xFF, (x2 & 0xFF)}, 4);
    ST7789_Write_Reg(0x2B, (uint8_t[]){(y1 >> 8) & 0xFF, (y1 & 0xFF), (y2 >> 8) & 0xFF, (y2 & 0xFF)}, 4);
    ST7789_Write_Reg(0x2C, NULL, 0);
}

static void ST7789_Draw_Bitmap(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t color_font, uint16_t color_back, const uint8_t *model)
{
    if (!Is_in_Screen(x, y, x + width - 1, y + height - 1))
        return;

    ST7789_SetWindow(x, y, x + width - 1, y + height - 1);

    uint8_t color_font_data[2] = {(color_font >> 8) & 0xFF, (color_font & 0xFF)};
    uint8_t color_back_data[2] = {(color_back >> 8) & 0xFF, (color_back & 0xFF)};
    uint16_t onerow_bytes = (width + 7) / 8;

    GPIO_ResetBits(ST7789_CS_Port, ST7789_CS_Pin);
    GPIO_SetBits(ST7789_DC_Port, ST7789_DC_Pin);

    for (uint16_t row = 0; row < height; row++)
    {
        const uint8_t *row_data = model + row * onerow_bytes;
        for (uint16_t col = 0; col < width; col++)
        {
            if (row_data[col / 8] & (1 << (col % 8)))
            {
                SPI_SendData(SPI2, color_font_data[0]);
                while (SPI_GetFlagStatus(SPI2, SPI_FLAG_TXE) == RESET)
                    ;
                SPI_SendData(SPI2, color_font_data[1]);
                while (SPI_GetFlagStatus(SPI2, SPI_FLAG_TXE) == RESET)
                    ;
            }
            else
            {
                SPI_SendData(SPI2, color_back_data[0]);
                while (SPI_GetFlagStatus(SPI2, SPI_FLAG_TXE) == RESET)
                    ;
                SPI_SendData(SPI2, color_back_data[1]);
                while (SPI_GetFlagStatus(SPI2, SPI_FLAG_TXE) == RESET)
                    ;
            }
        }
        while (SPI_GetFlagStatus(SPI2, SPI_FLAG_BSY) != RESET)
            ;
    }
}

static void ST7789_Write_Ascii(uint16_t x, uint16_t y, char ch, uint16_t color_font, uint16_t color_back, const Font_t *font)
{
    if (font == NULL || ch < 0x20 || ch > 0x7E)
        return;

    uint16_t width = font->size / 2;
    uint16_t height = font->size;
    uint16_t onerow_bytes = (width + 7) / 8;
    const uint8_t *model = font->ascii_model + (ch - ' ') * onerow_bytes * height;

    ST7789_Draw_Bitmap(x, y, width, height, color_font, color_back, model);
}

static void ST7789_Write_Chinese(uint16_t x, uint16_t y, char *ch, uint16_t color_font, uint16_t color_back, const Font_t *font)
{
    if (font == NULL || ch == NULL || *ch == '\0')
        return;

    uint16_t width = font->size;
    uint16_t height = font->size;
    const Chinese_Font_t *chinese_font = font->chinese;
    if (chinese_font == NULL)
        return;

    while (chinese_font->name != NULL)
    {
        if (strcmp(chinese_font->name, ch) == 0)
            break;
        chinese_font++;
    }

    if (chinese_font->name == NULL)
        return;

    ST7789_Draw_Bitmap(x, y, width, height, color_font, color_back, chinese_font->model);
}

// static int UTF8_char_length(const char *str)
// {
//     if (((*str & 0x80) == 0))
//         return 1; // 1字节
//     else if (((*str & 0xE0) == 0xC0))
//         return 2; // 2字节
//     else if (((*str & 0xF0) == 0xE0))
//         return 3; // 3字节
//     else if (((*str & 0xF8) == 0xF0))
//         return 4; // 4字节
//     else
//         return -1; // 无效的UTF-8字符
// }

void ST7789_Init(void)
{

    ST7789_GPIO_Init();

    ST7789_SPI_Init();

    ST7789_Display_Init();
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
    uint8_t color_data[2] = {(color >> 8) & 0xFF, (color & 0xFF)};

    ST7789_SetWindow(x1, y1, x2, y2); // 设置填充区域

    GPIO_ResetBits(ST7789_CS_Port, ST7789_CS_Pin);
    GPIO_SetBits(ST7789_DC_Port, ST7789_DC_Pin);

    for (uint32_t i = 0; i < size; i++)
    {
        SPI_SendData(SPI2, color_data[0]);
        while (SPI_GetFlagStatus(SPI2, SPI_FLAG_TXE) == RESET)
            ;
        SPI_SendData(SPI2, color_data[1]);
        while (SPI_GetFlagStatus(SPI2, SPI_FLAG_TXE) == RESET)
            ;
    }
    while (SPI_GetFlagStatus(SPI2, SPI_FLAG_BSY) != RESET)
        ;

    GPIO_SetBits(ST7789_CS_Port, ST7789_CS_Pin);
}

// void ST7789_Write_Ascii(uint16_t x, uint16_t y, char *str, uint16_t color_font, uint16_t color_back, const Font_t *font)
// {
//     while (*str)
//     {
//         ST7789_Write_Single_Ascii(x, y, *str, color_font, color_back, font);
//         x += font->size / 2;
//         // 超过屏幕宽度，换行
//         if (x + font->size / 2 > 239)
//         {
//             x = 0;
//             y += font->size;
//         }
//         str++;
//     }
// }

void ST7789_Write_String(uint16_t x, uint16_t y, char *str, uint16_t color_font, uint16_t color_back, const Font_t *font)
{
    while (*str)
    {
        int length = Is_GB2312(*str) ? 2 : 1;

        if (length <= 0) // 无效的UTF-8字符
        {
            str++;
            continue;
        }
        else if (length == 1) // 1字节字符
        {
            ST7789_Write_Ascii(x, y, *str, color_font, color_back, font);
            x += font->size / 2;
            // 超过屏幕宽度，换行
            if (x + font->size / 2 > 239)
            {
                x = 0;
                y += font->size;
                if (y > HEIGHT - 1)
                    return;
            }
            str += length;
        }
        else // 2或2字节字符以上
        {
            char ch[5];

            strncpy(ch, str, length);
            ch[length] = '\0';
            ST7789_Write_Chinese(x, y, ch, color_font, color_back, font);

            x += font->size;
            // 超过屏幕宽度，换行
            if (x + font->size > WIDTH - 1)
            {
                x = 0;
                y += font->size;
                if (y > HEIGHT - 1)
                    return;
            }
            str += length;
        }
    }
}

void ST7789_Draw_Picture(uint16_t x, uint16_t y, const Image_t *image)
{
    if (image == NULL)
        return;

    uint16_t width = image->width;
    uint16_t height = image->height;

    if (x > WIDTH - 1 || y > HEIGHT - 1 || x + width > WIDTH || y + height > HEIGHT)
        return;

    const uint8_t *data = image->data;
    uint32_t pixel_count = (uint32_t)width * height;

    ST7789_SetWindow(x, y, x + width - 1, y + height - 1);
    GPIO_ResetBits(ST7789_CS_Port, ST7789_CS_Pin);
    GPIO_SetBits(ST7789_DC_Port, ST7789_DC_Pin);

    for (uint32_t i = 0; i < pixel_count * 2; i += 2)
    {
        SPI_SendData(SPI2, data[i + 1]);
        while (SPI_GetFlagStatus(SPI2, SPI_FLAG_TXE) == RESET)
            ;
        SPI_SendData(SPI2, data[i]);
        while (SPI_GetFlagStatus(SPI2, SPI_FLAG_TXE) == RESET)
            ;
    }

    while (SPI_GetFlagStatus(SPI2, SPI_FLAG_BSY) != RESET)
        ;
    GPIO_SetBits(ST7789_CS_Port, ST7789_CS_Pin);
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

    // 简单阈值：各分量差值均小于 threshold，或平方和阈值
    return (dr < threshold && dg < threshold && db < threshold);
}

/**
 * @brief 绘制图片，自动检测并替换背景色
 * @param x,y        绘制起始坐标
 * @param image       图片结构体指针
 * @param target_back 目标背景色（例如 COLOR_WHITE）
 */
void ST7789_Draw_Picture_AutoTransparent(uint16_t x, uint16_t y, const Image_t *image, uint16_t target_back)
{
    if (image == NULL)
        return;
    uint16_t width = image->width;
    uint16_t height = image->height;
    const uint8_t *data = image->data;

    // 固定背景色为白色（可根据实际调整）
    uint16_t back_color = COLOR_WHITE;
    // 颜色接近阈值，建议 30~50（根据图片质量微调）
    uint8_t threshold = 90;

    ST7789_SetWindow(x, y, x + width - 1, y + height - 1);
    GPIO_ResetBits(ST7789_CS_Port, ST7789_CS_Pin);
    GPIO_SetBits(ST7789_DC_Port, ST7789_DC_Pin);

    uint32_t pixel_count = (uint32_t)width * height;
    for (uint32_t i = 0; i < pixel_count; i++)
    {
        uint16_t pixel = (data[i * 2 + 1] << 8) | data[i * 2];
        // 若像素与背景色相近，则替换
        if (Color_IsClose(pixel, back_color, threshold))
        {
            pixel = target_back;
        }
        // 发送像素（高字节在前）
        SPI_SendData(SPI2, (pixel >> 8) & 0xFF);
        while (SPI_GetFlagStatus(SPI2, SPI_FLAG_TXE) == RESET)
            ;
        SPI_SendData(SPI2, pixel & 0xFF);
        while (SPI_GetFlagStatus(SPI2, SPI_FLAG_TXE) == RESET)
            ;
    }
    while (SPI_GetFlagStatus(SPI2, SPI_FLAG_BSY) != RESET)
        ;
    GPIO_SetBits(ST7789_CS_Port, ST7789_CS_Pin);
}
