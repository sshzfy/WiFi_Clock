#ifndef __OTA_H__
#define __OTA_H__

#include <stdint.h>
#include <stdbool.h>

/** OTA 升级结果 */
typedef enum
{
    OTA_OK = 0,
    OTA_ERR_TIMEOUT, // 等待数据超时
    OTA_ERR_PROTO,   // 协议帧不对（握手/元数据 magic 错）
    OTA_ERR_HWID,    // 板型标识不匹配
    OTA_ERR_SIZE,    // 包大小非法，或剩余空间不足
    OTA_ERR_VERSION, // 版本不高于当前运行版本
    OTA_ERR_STORE,   // littlefs 读写失败
    OTA_ERR_CRC,     // 固件 CRC 错误（传输/存储损坏）
    OTA_ERR_BACKUP,  // 备份当前固件失败
    OTA_ERR_PARAM,   // 参数区写入失败
} ota_result_t;

/* 元数据帧，与 ota_send.py 打包格式一致（小端）。
 * 20 B：2+2+4x4，无填充，struct.pack("<HHIIII") 直接对应。 */
typedef struct
{
    uint16_t magic;     // 0x55AA
    uint16_t len;       // 元数据帧长度(20 B)
    uint32_t total_len; // .ota 包总字节数（64 头 + payload，4 对齐）
    uint32_t crc32;     // payload 的 CRC32（zlib，跳过 64 B 头）
    uint32_t version;   // major<<16 | minor<<8 | patch
    uint32_t hw_id;     // OTA_PKG_HW_ID
} ota_meta_t;

ota_result_t OTA_Run(void);
uint32_t OTA_AppSelfSize(void);

/* 提交已下载好的 /ota/firmware.tmp：改名 -> 备份 -> 写参数 -> 复位。
 * 串口离线升级与 Phase 2 网络升级共用；成功不返回（直接复位）。 */
ota_result_t OTA_CommitFirmware(uint32_t fw_size, uint32_t fw_crc32, uint32_t fw_version);
bool OTA_CalcAppInfo(uint32_t *size, uint32_t *crc32);

#endif
