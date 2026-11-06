#ifndef __OTA_UPDATE_H__
#define __OTA_UPDATE_H__

#include <stdint.h>

/* 更新阶段/结果 */
typedef enum
{
    OTA_UPD_IDLE = 0,     // 未开始
    OTA_UPD_CHECKING,     // 正在查询最新版本
    OTA_UPD_UP_TO_DATE,   // 已是最新
    OTA_UPD_DOWNLOADING,  // 正在下载
    OTA_UPD_APPLYING,     // 校验/备份/写参数
    OTA_UPD_REBOOTING,    // 即将复位
    OTA_UPD_ERR_NET,      // 连接/传输失败
    OTA_UPD_ERR_HTTP,     // HTTP 状态错误
    OTA_UPD_ERR_JSON,     // 最新 tag 解析失败
    OTA_UPD_ERR_NO_ASSET, // 拼不出附件地址
    OTA_UPD_ERR_CRC,      // 下载内容 CRC 错
    OTA_UPD_ERR_STORE,    // littlefs 读写失败
    OTA_UPD_ERR_SPACE,    // 剩余空间不足
    OTA_UPD_ERR_BACKUP,   // 备份当前固件失败
    OTA_UPD_ERR_PARAM,    // 参数区写入失败
} ota_upd_state_t;

/* 进度共享区*/
typedef struct
{
    volatile uint32_t state; // ota_upd_state_t
    volatile uint32_t total; // 固件总字节数
    volatile uint32_t got;   // 已下载字节数
} ota_prog_t;

extern ota_prog_t g_ota_prog;

void OTA_Update_Run(void);

#endif /* __OTA_UPDATE_H__ */
