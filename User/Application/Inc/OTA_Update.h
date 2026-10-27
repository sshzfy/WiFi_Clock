#ifndef __OTA_UPDATE_H__
#define __OTA_UPDATE_H__

/* ============================================================
 * OTA_Update —— 网络升级（Gitee Releases）
 * ------------------------------------------------------------
 * 流程：读 /releases/latest 的 302 头拿到最新 tag -> 版本比较
 *       -> 拼出 fw_<ver>.ota 地址 -> SSL 流式下载到 littlefs
 *       -> 校验 -> 复用 Phase 1 的备份/写参数/复位。
 * 全部在 netTask 内阻塞执行（下载期间独占 AT 模组），进度由
 * g_ota_prog 暴露给 uiTask 轮询刷新。
 * ============================================================ */

#include <stdint.h>

/* 更新阶段/结果（UI 按此显示文案与进度）*/
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

/* 进度共享区（OTA_Update 写, uiTask 读）*/
typedef struct
{
    volatile uint32_t state; // ota_upd_state_t
    volatile uint32_t total; // 固件总字节数
    volatile uint32_t got;   // 已下载字节数
} ota_prog_t;

extern ota_prog_t g_ota_prog;

void OTA_Update_Run(void);

#endif /* __OTA_UPDATE_H__ */
