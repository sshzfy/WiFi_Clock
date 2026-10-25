#ifndef __OTA_PKG_H__
#define __OTA_PKG_H__

#include <stdint.h>

/* ============================================================
 * .ota 固件包格式
 * ------------------------------------------------------------
 * 布局（全部小端，与 PC 端 ota_pack.py 的 struct.pack 一致）：
 *
 *   +0x00  magic        'OTA1' = 0x4F544131
 *   +0x04  header_size  固定 64
 *   +0x06  header_crc16 头部前 60 B 的 CRC16-CCITT（防头部损坏）
 *   +0x08  fw_version   major<<16 | minor<<8 | patch
 *   +0x0C  fw_size      payload 字节数
 *   +0x10  fw_crc32     payload 的 CRC32（zlib 标准反射算法）
 *   +0x14  hw_id        板型标识，防止把别的机型的固件刷进来
 *   +0x18  build_time   Unix 时间戳（诊断用）
 *   +0x1C  flags        保留
 *   +0x20  min_boot_ver 最低 Bootloader 版本（Phase 3 用）
 *   +0x24  reserved[28] 预留（签名等），补齐 64 B
 *   +0x40  payload      裸 App .bin
 *
 * 设计要点：
 *   1. 为什么不直接传 .bin？
 *      需要版本号（App 比较是否要升级）、CRC（校验完整性）、
 *      hw_id（防止刷错机型），这些必须和固件绑在一起传输。
 *   2. 为什么 payload 要 4 字节对齐？
 *      Bootloader 用 FLASH_ProgramWord 逐 4 字节写内部 Flash，
 *      长度不是 4 的倍数时最后一次写入会越界，打包工具补 0xFF
 *      （擦除态）到 4 的倍数。
 *   3. CRC 为什么只覆盖 payload 不覆盖头部？
 *      头部中的 header_crc16 已经单独保护头部；
 *      分开算便于"边下载边算 CRC"（跳过前 64 B 即可）。
 *   4. 本头文件同时被 Bootloader 和 App 包含，两侧解析口径一致，
 *      改动字段必须同步升级 BOOT_PARAM_LAYOUT 或做好兼容。
 * ============================================================ */

#define OTA_PKG_MAGIC              0x4F544131UL  // 'OTA1'
#define OTA_PKG_HEADER_SIZE        64UL          // 固定 64 B 头
#define OTA_PKG_HW_ID              0x04074001UL  // 板型标识，防止刷错机型

typedef struct
{
    uint32_t magic;          // OTA_PKG_MAGIC，不识别的直接拒绝 
    uint16_t header_size;    // 固定 64，未来扩展头的长度依据 
    uint16_t header_crc16;   // 头部前 60 B 的 CRC16-CCITT 
    uint32_t fw_version;     // major<<16 | minor<<8 | patch 
    uint32_t fw_size;        // payload 字节数（4 字节对齐）
    uint32_t fw_crc32;       // payload 的 CRC32，与 zlib 一致 
    uint32_t hw_id;          // OTA_PKG_HW_ID 
    uint32_t build_time;     // Unix 时间戳 
    uint32_t flags;          // 保留 
    uint32_t min_boot_ver;   // 最低 Bootloader 版本，Phase 3 用 
    uint8_t  reserved[28];   // 预留签名等，补齐 64 B 
} ota_hdr_t;

#endif
