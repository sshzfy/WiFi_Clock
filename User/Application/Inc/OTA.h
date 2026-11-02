#ifndef __OTA_H__
#define __OTA_H__

#include <stdint.h>
#include <stdbool.h>

/** OTA 升级结果 */
typedef enum
{
    OTA_OK = 0,
    OTA_ERR_STORE,  // littlefs 读写失败
    OTA_ERR_BACKUP, // 备份当前固件失败
    OTA_ERR_PARAM,  // 参数区写入失败
} ota_result_t;

uint32_t OTA_AppSelfSize(void);
ota_result_t OTA_CommitFirmware(uint32_t fw_size, uint32_t fw_crc32, uint32_t fw_version);
bool OTA_CalcAppInfo(uint32_t *size, uint32_t *crc32);

#endif
