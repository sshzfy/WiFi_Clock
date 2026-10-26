#ifndef __BOOT_CRC32_H__
#define __BOOT_CRC32_H__

#include <stdint.h>

uint32_t Boot_CRC32_Update(uint32_t crc, const void *data, uint32_t len);
uint32_t Boot_CRC32(const void *data, uint32_t len);

#endif
