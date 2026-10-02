#ifndef __UTF8_GB2312_H__
#define __UTF8_GB2312_H__

#include "main.h"

/* ============================================================
 * UTF-8 -> GB2312 转码 (仅 ASCII + GB2312 一级汉字 3755 字)
 * ------------------------------------------------------------
 * 接口层的文本(高德逆地理编码的省/市、心知天气的城市名)是 UTF-8,
 * 而 LCD 的汉字字模(cn16.bin / cn22.bin)按 GB2312 区位码索引,
 * 两者之间必须先转码, 否则汉字显示为空白。
 *
 * 实现见 BSP/Src/Utf8_Gb2312.c(由 Documents/Tools/gen_gb2312_table.py 生成)。
 * ============================================================ */

/**
 * @brief 把 UTF-8 字符串转成 GB2312(一级汉字) + ASCII 混合串
 * @param src      UTF-8 源串(NULL 视为空串)
 * @param dst      输出缓冲
 * @param dst_size 输出缓冲字节数(含结尾 '\0')
 * @return 写入 dst 的字节数(不含结尾 '\0')
 * @note  一级字库没有的汉字整字跳过(不占宽度), 放不下的整字丢弃。
 */
uint16_t Utf8_To_Gb2312(const char *src, char *dst, uint16_t dst_size);

/**
 * @brief 按像素宽度截断 GB2312 串(汉字 = font_size, ASCII = font_size/2)
 * @param gb        GB2312 串(就地截断)
 * @param max_px    可用像素宽度
 * @param font_size 字号(像素高)
 * @return None
 */
void Gb2312_Truncate_Px(char *gb, uint16_t max_px, uint16_t font_size);

#endif /* __UTF8_GB2312_H__ */
