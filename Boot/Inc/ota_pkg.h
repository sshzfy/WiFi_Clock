#ifndef __OTA_PKG_H__
#define __OTA_PKG_H__

#include <stdint.h>

/* ============================================================
 * .ota 固件包格式
 * ------------------------------------------------------------
 * 布局（全部小端，与 PC 端 ota_pack.py 的 struct.pack 一致）：
 * head：
 *   +0x00  magic        'OTA1' = 0x4F544131
 *   +0x04  header_size  固定 64 B 头大小
 *   +0x06  header_crc16 头部前 60 B 的 CRC16-CCITT（防头部损坏）
 *   +0x08  fw_version   major<<16 | minor<<8 | patch
 *   +0x0C  fw_size      payload 字节数
 *   +0x10  fw_crc32     payload 的 CRC32（zlib 标准反射算法）
 *   +0x14  hw_id        板型标识，防止把别的机型的固件刷进来
 *   +0x18  build_time   Unix 时间戳（诊断用）
 *   +0x1C  flags        保留
 *   +0x20  min_boot_ver 最低 Bootloader 版本
 *   +0x24  reserved[28] 预留（签名等），补齐 64 B
 * payload：
 *   +0x40  payload      App .bin
 * ============================================================ */

#define OTA_PKG_MAGIC              0x4F544131UL
#define OTA_PKG_HEADER_SIZE        64UL
#define OTA_PKG_HW_ID              0x04074001UL

/* OTA 固件包头 */
typedef struct
{
    uint32_t magic;
    uint16_t header_size;
    uint16_t header_crc16;
    uint32_t fw_version;
    uint32_t fw_size;
    uint32_t fw_crc32;
    uint32_t hw_id;
    uint32_t build_time;
    uint32_t flags;
    uint32_t min_boot_ver;
    uint8_t reserved[28];
} ota_hdr_t;

#endif
