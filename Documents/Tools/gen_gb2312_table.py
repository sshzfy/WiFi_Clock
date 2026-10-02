# -*- coding: utf-8 -*-
"""生成 UTF-8 -> GB2312 转码表与转换函数 (BSP/Src/Utf8_Gb2312.c)。

背景
----
LCD 中文字模来自 W25Q64 的 cn16.bin / cn22.bin, 索引方式是 GB2312 区位码:
    index = (区码 - 0xB0) * 94 + (位码 - 0xA1)
而且只有**一级汉字 3755 字**(0xB0A1~0xD7F9), 二级汉字根本没有字模。
网络接口(高德 / 心知天气)返回的中文一律是 UTF-8, 直接丢给
ST7789_Write_String() 只会显示空白或错字, 所以必须在显示前转成 GB2312。

本脚本按"Unicode 升序"生成两张一一对应的表:
    Uni_Table[i] -> Gb_Table[i]
转换时对 Unicode 码点做二分查找, 单字约 6 次比较, 不需要额外 RAM。

用法(仓库根目录下任意位置执行均可):
    python Documents/Tools/gen_gb2312_table.py
"""

import os

# 仓库根目录: 本文件位于 <root>/Documents/Tools/
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT_C = os.path.join(ROOT, "BSP", "Src", "Utf8_Gb2312.c")

# GB2312 一级汉字区: 区码 0xB0~0xD7, 位码 0xA1~0xFE
FIRST_LEVEL_QU = (0xB0, 0xD7)
COUNT_EXPECTED = 3755

pairs = []
for qu in range(FIRST_LEVEL_QU[0], FIRST_LEVEL_QU[1] + 1):
    for we in range(0xA1, 0xFF):
        try:
            ch = bytes((qu, we)).decode("gb2312")
        except UnicodeDecodeError:
            continue
        pairs.append((ord(ch), (qu << 8) | we))

# 按 Unicode 升序, 供二分查找
pairs.sort()

if len(pairs) != COUNT_EXPECTED:
    raise SystemExit("一级汉字数量异常: %d (期望 %d)" % (len(pairs), COUNT_EXPECTED))

# 同一 Unicode 只应出现一次, 否则二分查找会取到不确定的结果
unis = [u for u, _ in pairs]
if len(set(unis)) != len(unis):
    raise SystemExit("Unicode 码点存在重复")


def table_text(values, per_line=8):
    """把整数列表排版成 C 数组初始化体。"""
    lines = []
    for i in range(0, len(values), per_line):
        chunk = values[i:i + per_line]
        lines.append("    " + " ".join("0x%04XU," % v for v in chunk))
    return "\n".join(lines)


uni_text = table_text(unis)
gb_text = table_text([gb for _, gb in pairs])

C_TEMPLATE = r'''/* ============================================================
 * UTF-8 -> GB2312 转码 (仅 ASCII + GB2312 一级汉字)
 * ------------------------------------------------------------
 * 本文件由 Documents/Tools/gen_gb2312_table.py 生成, 请勿手工修改数值。
 *
 * 为什么需要它:
 *   LCD 的汉字字模 (W25Q64 里的 cn16.bin / cn22.bin) 按 GB2312 区位码
 *   索引, 而高德 / 心知天气等接口返回的是 UTF-8。直接把 UTF-8 中文丢给
 *   ST7789_Write_String() 会查不到字模, 显示为空白。
 *
 * 表的范围:
 *   只覆盖 GB2312 一级汉字 3755 字 (区码 0xB0~0xD7), 与字库完全一致;
 *   二级汉字本来就没有字模, 转出来也只能显示空白, 这里直接丢弃该字符
 *   (不占宽度, 不会破坏后面的排版)。
 *   表按 Unicode 升序排列, 转换时二分查找, 无额外 RAM 开销。
 * ============================================================ */

#include "Utf8_Gb2312.h"

#define GB_TABLE_COUNT 3755U /* 一级汉字数量, 与 ASSET_GLYPH_COUNT 一致 */

/* Unicode 码点(升序) —— 二分查找的键 */
static const uint16_t Uni_Table[GB_TABLE_COUNT] = {
/*@UNI@*/
};

/* 与 Uni_Table 一一对应的 GB2312 双字节码(高字节在前) */
static const uint16_t Gb_Table[GB_TABLE_COUNT] = {
/*@GB@*/
};

/**
 * @brief 在对照表里二分查找 Unicode 码点对应的 GB2312 码
 * @param uni Unicode 码点(基本多文种平面)
 * @return GB2312 双字节码(高字节<<8 | 低字节); 0 = 一级字库没有该字
 */
static uint16_t Utf8_Find_Gb(uint16_t uni)
{
    /* 用有符号 int 做下标: mid=0 时 mid-1 必须变成 -1 结束循环,
     * 若用 uint16_t 会回绕成 65535, 接着就会越界访问整张表 */
    int lo = 0;
    int hi = (int)GB_TABLE_COUNT - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;

        if (Uni_Table[mid] < uni)
            lo = mid + 1;
        else if (Uni_Table[mid] > uni)
            hi = mid - 1;
        else
            return Gb_Table[mid];
    }

    return 0U;
}

/**
 * @brief 把 UTF-8 字符串转成 GB2312(一级汉字) + ASCII 混合串
 * @param src      UTF-8 源串(NULL 视为空串)
 * @param dst      输出缓冲
 * @param dst_size 输出缓冲字节数(含结尾 '\0')
 * @return 写入 dst 的字节数(不含结尾 '\0')
 * @note  放不下的字符整字丢弃; 查不到字模的汉字整字跳过, 因此返回长度
 *        可能短于源串, 但仍保证 dst 是合法的 GB2312 串。
 */
uint16_t Utf8_To_Gb2312(const char *src, char *dst, uint16_t dst_size)
{
    const uint8_t *p = (const uint8_t *)src;
    uint16_t n = 0U;

    if (dst == NULL || dst_size == 0U)
        return 0U;

    dst[0] = '\0';

    if (src == NULL)
        return 0U;

    while (*p != 0U)
    {
        uint16_t uni;
        uint16_t gb;

        /* ASCII: 原样拷贝(液晶 ASCII 字模宽度 = 汉字的一半) */
        if (*p < 0x80U)
        {
            if ((uint16_t)(n + 1U) >= dst_size)
                break;
            dst[n++] = (char)(*p++);
            continue;
        }

        if ((*p & 0xF0U) == 0xE0U) /* 3 字节序列: 汉字就在这一段 */
        {
            if (p[1] == 0U || p[2] == 0U)
                break; /* 序列被截断, 后面的内容不可信 */
            uni = (uint16_t)(((uint16_t)(p[0] & 0x0FU) << 12) |
                             ((uint16_t)(p[1] & 0x3FU) << 6) |
                             (uint16_t)(p[2] & 0x3FU));
            p += 3;
        }
        else if ((*p & 0xE0U) == 0xC0U) /* 2 字节序列(拉丁扩展等): 丢弃 */
        {
            if (p[1] == 0U)
                break;
            p += 2;
            continue;
        }
        else /* 4 字节序列或非法首字节: 丢弃 */
        {
            p++;
            continue;
        }

        gb = Utf8_Find_Gb(uni);
        if (gb == 0U)
            continue; /* 一级字库没有这个字: 整字跳过 */

        if ((uint16_t)(n + 2U) >= dst_size)
            break;
        dst[n++] = (char)(gb >> 8);
        dst[n++] = (char)(gb & 0xFFU);
    }

    dst[n] = '\0';

    return n;
}

/**
 * @brief 按像素宽度截断 GB2312 串(汉字 = font_size, ASCII = font_size/2)
 * @param gb        GB2312 串(就地截断)
 * @param max_px    可用像素宽度
 * @param font_size 字号(像素高)
 * @return None
 * @note  只会在字符边界上截断, 不会留下半个汉字。
 */
void Gb2312_Truncate_Px(char *gb, uint16_t max_px, uint16_t font_size)
{
    uint16_t n = 0U;
    uint16_t px = 0U;

    if (gb == NULL || font_size == 0U)
        return;

    while (gb[n] != '\0')
    {
        uint8_t ch = (uint8_t)gb[n];
        uint16_t w;

        if (ch >= 0xA1U && ch <= 0xF7U) /* 汉字: 两个字节一个字 */
        {
            if (gb[n + 1] == '\0')
                break; /* 只有半个汉字, 丢弃 */
            w = font_size;
            if ((uint16_t)(px + w) > max_px)
                break;
            n = (uint16_t)(n + 2U);
        }
        else /* ASCII: 一个字节半个字宽 */
        {
            w = (uint16_t)(font_size / 2U);
            if ((uint16_t)(px + w) > max_px)
                break;
            n++;
        }

        px = (uint16_t)(px + w);
    }

    gb[n] = '\0';
}
'''

text = C_TEMPLATE.replace("/*@UNI@*/", uni_text).replace("/*@GB@*/", gb_text)

# 与仓库里其它源文件一致: UTF-8 无 BOM(注释含中文, 与 App.c 等同为 armcc 可接受的写法)
with open(OUT_C, "wb") as f:
    f.write(text.encode("utf-8"))

print("生成 %s" % OUT_C)
print("一级汉字 %d 字, 表占 %d 字节" % (len(pairs), len(pairs) * 4))
