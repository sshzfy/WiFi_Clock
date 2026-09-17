#ifndef __FONT_H__
#define __FONT_H__

#include "main.h"
#include "BuildConfig.h"

/* ============================================================
 * 字体资源布局
 * ------------------------------------------------------------
 * 所有字模(ASCII 表与汉字库)都已迁移到 W25Q64 的 littlefs 文件,
 * MCU Flash 里只保留本文件中的"文件 ID + 尺寸"描述。
 * 定位方式都是 O(1), 不再按名字线性查找:
 *
 *   ASCII : off = (ch - 0x20) * ob * size,  ob = ((size/2)+7)/8
 *   汉字  : off = ((区码-0xB0)*94 + (位码-0xA1)) * cn_bytes
 *
 * 点阵格式: 横向取模, 行优先, 每行 ob=(gw+7)/8 字节,
 *           每字节低位 bit0 对应最左侧像素。
 * ============================================================ */

/* 资源文件 ID, 与 Asset.c 里的路径表一一对应 */
typedef enum
{
    ASSET_FID_CN16 = 0, /* /font/cn16.bin   16x16 汉字库(3755 字) */
    ASSET_FID_CN22,     /* /font/cn22.bin   22x22 汉字库(3755 字) */
    ASSET_FID_AS12,     /* /font/as12.bin   12 号 ASCII */
    ASSET_FID_AS16,     /* /font/as16.bin   16 号 ASCII */
    ASSET_FID_AS16B,    /* /font/as16b.bin  16 号 ASCII(B) */
    ASSET_FID_AS22,     /* /font/as22.bin   22 号 ASCII */
    ASSET_FID_AS22B,    /* /font/as22b.bin  22 号 ASCII(B) */
    ASSET_FID_AS32,     /* /font/as32.bin   32 号 ASCII */
    ASSET_FID_AS32B,    /* /font/as32b.bin  32 号 ASCII(B) */
    ASSET_FID_AS48,     /* /font/as48.bin   48 号 ASCII */
    ASSET_FID_AS48B,    /* /font/as48b.bin  48 号 ASCII(B) */
    ASSET_FID_COUNT     /* 字库资源 ID 数量 */
} Asset_FontId_t;

#define ASSET_FID_NONE 0xFFU /* 该字体没有这一类字模 */

typedef struct
{
    uint16_t size;    // 字体大小(像素高)
    uint8_t ascii_id; // ASCII 表资源 ID; ASSET_FID_NONE = 无
    uint8_t cn_id;    // 汉字库资源 ID; ASSET_FID_NONE = 纯ASCII字体
    uint8_t cn_bytes; // 汉字每字字节数(16号=32, 22号=66); 0 = 无汉字
    uint8_t reserved; // 对齐填充
} Font_t;

/* 各资源文件的字节数, 供烧录与开机自检使用 */
#define ASSET_GLYPH_COUNT 3755U /* GB2312 一级字库字数 */

#define ASSET_CN16_SIZE  (ASSET_GLYPH_COUNT * 32U) /* 120160 */
#define ASSET_CN22_SIZE  (ASSET_GLYPH_COUNT * 66U) /* 247830 */
#define ASSET_AS12_SIZE  (95U * 12U * 1U)          /* 12 号 ASCII */
#define ASSET_AS16_SIZE  (95U * 16U * 1U)          /* 16 号 ASCII */
#define ASSET_AS16B_SIZE (95U * 16U * 1U)          /* 16 号 ASCII(加粗) */
#define ASSET_AS22_SIZE  (95U * 22U * 2U)          /* 22 号 ASCII */
#define ASSET_AS22B_SIZE (95U * 22U * 2U)          /* 22 号 ASCII(加粗) */
#define ASSET_AS32_SIZE  (95U * 32U * 2U)          /* 32 号 ASCII */
#define ASSET_AS32B_SIZE (95U * 32U * 2U)          /* 32 号 ASCII(加粗) */
#define ASSET_AS48_SIZE  (95U * 48U * 3U)          /* 48 号 ASCII */
#define ASSET_AS48B_SIZE (95U * 48U * 3U)          /* 48 号 ASCII(加粗) */

/* ASCII 单字符点阵字节数 = ob * size */
#define ASSET_ASCII_GLYPH_BYTES(sz) (((((sz) / 2U) + 7U) / 8U) * (sz))

/* 全部字模里的最大单字节数: 48 号 ASCII = 3 字节/行 * 48 行 = 144 */
#define ASSET_GLYPH_BYTES_MAX (ASSET_ASCII_GLYPH_BYTES(48U))

/* ------------------------------------------------------------
 * 资源数据本体声明
 * 只在烧录固件(RESOURCE_DATA_IN_ROM=1)里编译, 正式固件不引用,
 * 因此这些表不会占用 MCU Flash。数据仍完整备份在源码中。
 * ------------------------------------------------------------ */
#if (RESOURCE_DATA_IN_ROM == 1)
extern const unsigned char Font_12_Table[ASSET_AS12_SIZE];
extern const unsigned char Font_16_Table[ASSET_AS16_SIZE];
extern const unsigned char Font_16B_Table[ASSET_AS16B_SIZE];
extern const unsigned char Font_22_Table[ASSET_AS22_SIZE];
extern const unsigned char Font_22B_Table[ASSET_AS22B_SIZE];
extern const unsigned char Font_32_Table[ASSET_AS32_SIZE];
extern const unsigned char Font_32B_Table[ASSET_AS32B_SIZE];
extern const unsigned char Font_48_Table[ASSET_AS48_SIZE];
extern const unsigned char Font_48B_Table[ASSET_AS48B_SIZE];

extern const unsigned char Chinese_Font16_Data[ASSET_CN16_SIZE];
extern const unsigned char Chinese_Font22_Data[ASSET_CN22_SIZE];
#endif /* RESOURCE_DATA_IN_ROM */

/* 字体结构 */
extern const Font_t Font_12;
extern const Font_t Font_16;
extern const Font_t Font_16B;
extern const Font_t Font_22;
extern const Font_t Font_22B;
extern const Font_t Font_32;
extern const Font_t Font_32B;
extern const Font_t Font_48;
extern const Font_t Font_48B;

#endif /* __FONT_H__ */
