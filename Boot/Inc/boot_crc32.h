#ifndef __BOOT_CRC32_H__
#define __BOOT_CRC32_H__

#include <stdint.h>

/* 标准反射 CRC32（多项式 0xEDB88320），结果与 Python zlib.crc32() 一致。
 * 不使用 STM32 硬件 CRC：它是 MSB-first、不反射，PC 端难以对齐。
 * Boot_CRC32_Update 可分段调用，便于搬运时边读边算，不必整包放 RAM。 */

uint32_t Boot_CRC32_Update(uint32_t crc, const void *data, uint32_t len);
uint32_t Boot_CRC32(const void *data, uint32_t len);

#endif
