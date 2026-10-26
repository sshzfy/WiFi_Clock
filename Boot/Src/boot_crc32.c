#include "boot_crc32.h"

/* ============================================================
 * CRC32（标准反射算法，多项式 0xEDB88320）
 * ------------------------------------------------------------
 * 1. 不用 STM32 硬件 CRC 外设:
 *    F4 的 CRC 外设是 MSB-first、不反射，结果与 PC 端常用的
 *    zlib.crc32() 不一致；统一用软件标准算法，PC 端零成本对齐。
 * 2. 用 nibble 查表而不是 256 项大表:
 *    256 项表要 1 KB Flash；nibble 表 16 项只占 64 B，
 *    对 100~400 KB 固件来说速度完全够用。
 * 3. Boot_CRC32_Update 设计为增量接口：搬运固件时按 4 KB
 *    分块读、边读边算，不需要把整个固件放进 RAM。
 * ============================================================ */

/* CRC32 查表 */
static const uint32_t s_crc32_table[16] =
    {
        0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL,
        0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
        0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
        0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL};

/**
 * @brief  增量计算 CRC32
 * @param  crc   上一次计算的中间值；第一次传 0xFFFFFFFF
 * @param  data  数据首地址
 * @param  len   字节数
 * @return 未取反的中间值，可继续传给下一次调用
 * @note   最后要自己取反：result = ~Boot_CRC32_Update(...)
 */
uint32_t Boot_CRC32_Update(uint32_t crc, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    while (len-- != 0U)
    {
        crc ^= (uint32_t)*p++;
        crc = (crc >> 4) ^ s_crc32_table[crc & 0x0FU];
        crc = (crc >> 4) ^ s_crc32_table[crc & 0x0FU];
    }

    return crc;
}

/**
 * @brief  一次性计算一段数据的 CRC32
 * @param  data 数据首地址
 * @param  len  字节数
 * @return 最终 CRC32，可直接与 zlib.crc32() 比较
 */
uint32_t Boot_CRC32(const void *data, uint32_t len)
{
    return ~Boot_CRC32_Update(0xFFFFFFFFUL, data, len);
}
