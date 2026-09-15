#ifndef __FONT_H__
#define __FONT_H__

#include "main.h"

typedef struct
{
    const char *name;
    const uint8_t *model;
} Chinese_Font_t;

typedef struct
{
    uint16_t size; // 字体大小,高度
    const uint8_t *ascii_model;
    const Chinese_Font_t *chinese;
} Font_t;

/* 供littlefs使用 */
extern const uint8_t Font_12_Table[];
extern const uint8_t Font_16_Table[];
extern const uint8_t Font_16B_Table[];
extern const Chinese_Font_t Chinese_Font_16B_Table[];
extern const uint8_t Font_22_Table[];
extern const uint8_t Font_22B_Table[];
extern const Chinese_Font_t Chinese_Font_22B_Table[];
extern const uint8_t Font_32_Table[];
extern const uint8_t Font_32B_Table[];
extern const uint8_t Font_48_Table[];
extern const uint8_t Font_48B_Table[];

/* GB2312 一级字库 16x16 点阵(3755 字), 独立文件 Chinese_Font16.c
 * 由 Documents/中文字库/Chinese_16.txt 生成, 点阵布局与 Font_16.c 一致。
 * 注意: 数据约 120KB, 加上名字串与指针表共约 165KB。
 * 只有被引用时链接器才会把它链入固件, 当前没有任何 Font_t 挂载它。 */
extern const Chinese_Font_t Chinese_Font16_Table[];

/* GB2312 一级字库 22x22 点阵(3755 字), 独立文件 Chinese_Font22.c
 * 由 Documents/中文字库/Chinese_22.txt 生成, 点阵布局与 Font_22.c 一致:
 * 每字 66 字节 = 22 行 x 3 字节, 横向取模, 每字节低位 bit0 对应左侧像素。
 * 注意: 点阵数据约 242KB, 加上名字串与指针表共约 290KB。
 * 只有被引用时链接器才会把它链入固件, 当前没有任何 Font_t 挂载它。 */
extern const Chinese_Font_t Chinese_Font22_Table[];

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
