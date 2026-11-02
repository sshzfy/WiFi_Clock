#include "OTA.h"

#include "boot_layout.h"
#include "boot_param.h"
#include "boot_crc32.h"
#include "ota_pkg.h"
#include "Version.h"
#include "OTA_Store.h"

#include "FreeRTOS.h"
#include "task.h"

#include "lfs.h"
#include "BuildConfig.h"
#include "Secrets.h"

#include <string.h>

/* ============================================================
 * OTA 提交与自检（App 侧，网络升级共用）
 * ------------------------------------------------------------
 * 把已下载并校验通过的 /ota/firmware.tmp 提交为正式固件：
 *   rename -> 备份当前固件 -> 写参数请求 Bootloader 搬运 -> 复位。
 * 另提供自身固件大小/CRC 的自检函数，供启动确认任务使用。
 * ============================================================ */

#define OTA_CHUNK 4096U // 4 KB 缓冲

static uint8_t s_buf[OTA_CHUNK]; // 4 KB 缓冲区

/**
 * @brief  计算自身固件大小（备份/回滚/自检共用）
 * @return 固件字节数（4 字节对齐）；0 = 无可用来源
 */
uint32_t OTA_AppSelfSize(void)
{
    boot_param_t p;
    uint32_t size = 0U;

    /* 一级来源：参数区 app_size。但只有 APP_UNCONFIRMED 状态可信 */
    if (Boot_Param_Load(&p) &&
        p.state == BOOT_ST_APP_UNCONFIRMED &&
        p.app_size >= 0x1000U && p.app_size <= APP_SIZE)
        size = p.app_size;

    /* 二级来源：BuildConfig.h 中手动维护的常量 */
    if (size == 0U)
        size = APP_FW_SIZE_FALLBACK;

    if (size == 0U || size > APP_SIZE)
        return 0U;

    /* 向上取整到 4 字节写 Flash */
    return (size + 3U) & ~3U;
}

/**
 * @brief  计算自身固件的大小与 CRC32
 * @param  size  输出：固件字节数（可为 NULL）
 * @param  crc32 输出：固件 CRC32（可为 NULL）
 * @return true = 计算成功
 */
bool OTA_CalcAppInfo(uint32_t *size, uint32_t *crc32)
{
    uint32_t sz = OTA_AppSelfSize();
    uint32_t crc = 0xFFFFFFFFUL;

    if (sz == 0U || sz > APP_SIZE)
        return false;

    for (uint32_t off = 0U; off < sz; off += OTA_CHUNK)
    {
        uint32_t n = (sz - off > OTA_CHUNK) ? OTA_CHUNK : (sz - off);

        memcpy(s_buf, (const void *)(APP_BASE + off), n);
        crc = Boot_CRC32_Update(crc, s_buf, n);
    }

    if (size != NULL)
        *size = sz;
    if (crc32 != NULL)
        *crc32 = ~crc;

    return true;
}

/**
 * @brief  CRC16-CCITT OTA t头部校验（poly 0x1021，init 0xFFFF）
 * @param  data 数据首地址
 * @param  len  字节数
 * @return 16 位校验值
 */
static uint16_t Ota_Crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFU;

    for (uint32_t i = 0U; i < len; i++)
    {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
        {
            if ((crc & 0x8000U) != 0U)
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            else
                crc = (uint16_t)(crc << 1);
        }
    }

    return crc;
}

/**
 * @brief  备份当前固件为 .ota 格式写入 backup.bin
 * @return OTA_OK 成功；OTA_ERR_BACKUP 失败
 */
static ota_result_t Backup_Current(void)
{
    ota_hdr_t hdr;
    uint32_t size = OTA_AppSelfSize();
    uint32_t crc = 0xFFFFFFFFUL;
    int err;

    if (size == 0U || size > APP_SIZE)
        return OTA_ERR_BACKUP;

    /* 读取 Flash 数据, 并计算 CRC32 */
    for (uint32_t off = 0U; off < size; off += OTA_CHUNK)
    {
        uint32_t n = (size - off > OTA_CHUNK) ? OTA_CHUNK : (size - off);

        memcpy(s_buf, (const void *)(APP_BASE + off), n);
        crc = Boot_CRC32_Update(crc, s_buf, n);
    }
    crc = ~crc;

    /* 构造 .ota 头 */
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = OTA_PKG_MAGIC;
    hdr.header_size = (uint16_t)OTA_PKG_HEADER_SIZE;
    hdr.fw_version = APP_VERSION;
    hdr.fw_size = size;
    hdr.fw_crc32 = crc;
    hdr.hw_id = OTA_PKG_HW_ID;
    hdr.header_crc16 = Ota_Crc16((const uint8_t *)&hdr, 60U);

    /* 写入 .ota(tmp) 头 */
    if (OTA_Store_OpenWrite(OTA_BACKUP_TMP_PATH) != LFS_ERR_OK)
        return OTA_ERR_BACKUP;

    err = OTA_Store_Write((const uint8_t *)&hdr, sizeof(hdr));
    if (err != (int)sizeof(hdr))
        goto fail;

    /* 写入 .ota 数据 */
    for (uint32_t off = 0U; off < size; off += OTA_CHUNK)
    {
        uint32_t n = (size - off > OTA_CHUNK) ? OTA_CHUNK : (size - off);

        memcpy(s_buf, (const void *)(APP_BASE + off), n);
        if (OTA_Store_Write(s_buf, n) != (int)n)
            goto fail;
    }

    if (OTA_Store_Close() != LFS_ERR_OK)
        return OTA_ERR_BACKUP;

    /* 原子替换：删除旧备份 -> tmp 改名。若这一步失败，
     * 旧 backup.bin 可能已被删掉，回滚能力暂时丢失，
     * 但本次升级会中止，旧 App 不受影响。 */
    (void)OTA_Store_Remove(OTA_BACKUP_PATH);
    if (OTA_Store_Rename(OTA_BACKUP_TMP_PATH, OTA_BACKUP_PATH) != LFS_ERR_OK)
        return OTA_ERR_BACKUP;

    return OTA_OK;

fail:
    (void)OTA_Store_Close();
    return OTA_ERR_BACKUP;
}

/**
 * @brief  提交已下载好的 /ota/firmware.tmp：改名 -> 备份 -> 写参数 -> 复位
 * @param  fw_size    固件 payload 字节数（不含 64 B 包头）
 * @param  fw_crc32   payload CRC32（调用前必须已校验通过）
 * @param  fw_version 版本编码
 * @return 失败返回错误码；成功不返回（延时 1 秒后复位）
 */
ota_result_t OTA_CommitFirmware(uint32_t fw_size, uint32_t fw_crc32, uint32_t fw_version)
{
    boot_param_t p;

    /* 1. tmp -> firmware.bin：littlefs 的 rename 是原子操作，
     * Bootloader 任何时候打开 .bin 看到的都是完整文件 */
    (void)OTA_Store_Remove(OTA_FW_PATH);
    if (OTA_Store_Rename(OTA_FW_TMP_PATH, OTA_FW_PATH) != LFS_ERR_OK)
        return OTA_ERR_STORE;

    /* 2. 备份当前固件（失败则删除新固件并中止） */
    ota_result_t bk_ret = Backup_Current();
    if (bk_ret != OTA_OK)
    {
        (void)OTA_Store_Remove(OTA_FW_PATH);
        return bk_ret;
    }

    /* 3. 写参数：请求 Bootloader 搬运 */
    Boot_Param_Default(&p);
    if (Boot_Param_Load(&p))
        p.seq++;
    else
        p.seq = 1U;

    p.state = BOOT_ST_OTA_PENDING;
    p.ota_size = fw_size;
    p.ota_crc32 = fw_crc32;
    p.ota_version = fw_version;
    p.flags |= BOOT_FLAG_BACKUP_VALID;

    if (!Boot_Param_Save(&p))
        return OTA_ERR_PARAM;

    /* 4. 留 1 秒给 UI 回执，然后复位进 Bootloader 搬运 */
    vTaskDelay(pdMS_TO_TICKS(1000));
    NVIC_SystemReset();

    return OTA_OK;
}
