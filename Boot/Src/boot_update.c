#include "boot_update.h"

#include "boot_layout.h"
#include "boot_param.h"
#include "boot_crc32.h"
#include "boot_lfs.h"
#include "boot_jump.h"
#include "boot_uart.h"
#include "ota_pkg.h"
#include "lfs.h"

#include "stm32f4xx.h"
#include "stm32f4xx_flash.h"

#include <string.h>

/* ============================================================
 * 固件搬运（升级 / 回滚共用）
 * ------------------------------------------------------------
 * 搬运 = 把 littlefs 里的 .ota 文件内容写进内部 Flash 的 App 区。
 *
 * 顺序：
 *   1. 先校验后破坏 —— 擦 App 之前必须确认文件头合法、payload CRC 正确；
 *   2. 先记账后动手 —— 擦除前把 state=OTA_WRITING 写进参数区，
 *      掉电后下次启动能凭这个状态重新搬运；
 *   3. 写完回读验证 —— 写入成功不代表内容正确，回读算 CRC 再跳转；
 *   4. 任何失败不跳转 —— 返回 false 让上层尝试回滚。
 *
 * 掉电矩阵（搬运阶段）：
 *   擦除中掉电   -> state=OTA_WRITING，下次重新擦写（App 区全 0xFF）
 *   写入中掉电   -> 同上，重新擦写
 *   校验失败     -> 返回 false，上层回滚或停住
 * ============================================================ */

#define UPDATE_BUF_SIZE 4096U // 4 KB 缓冲

/* 用 uint32_t 数组保证 4 字节对齐 */
static uint32_t s_buf[UPDATE_BUF_SIZE / 4U]; // 搬运缓冲：既作 littlefs 读目标，也作 Flash 写源

/**
 * @brief  循环读满指定长度
 * @param  buf 读入缓冲
 * @param  len 期望字节数
 * @return 成功返回实际读入字节数；失败返回 -1
 */
static int File_ReadFull(void *buf, uint32_t len)
{
    uint8_t *p = (uint8_t *)buf; // 读入缓冲指针
    uint32_t got = 0U;           // 已读入字节数

    while (got < len)
    {
        int r_byte = Boot_Lfs_FileRead(p + got, len - got);
        if (r_byte <= 0)
            return -1;
        got += (uint32_t)r_byte;
    }

    return (int)got;
}

/**
 * @brief  擦除 App 区 Sector 4-7（共 448 KB）
 * @return true = 全部擦除成功
 */
static bool Flash_EraseApp(void)
{
    static const uint32_t sectors[4] = {FLASH_Sector_4, FLASH_Sector_5, FLASH_Sector_6, FLASH_Sector_7};

    FLASH_Unlock();
    for (uint32_t i = 0U; i < 4U; i++)
    {
        Boot_Uart_Puts("[BOOT] erase sector ");
        Boot_Uart_PutU32(i + 4U); // 打印逻辑扇区号 4/5/6/7
        Boot_Uart_Puts("\n");

        if (FLASH_EraseSector(sectors[i], VoltageRange_3) != FLASH_COMPLETE)
        {
            FLASH_Lock();
            return false;
        }
    }
    FLASH_Lock();

    return true;
}

/**
 * @brief  写一段数据到内部 Flash
 * @param  addr 目标地址（4 字节对齐）
 * @param  buf  数据缓冲
 * @param  len  字节数（4 的倍数）
 * @return true = 全部写入成功
 */
static bool Flash_Write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    FLASH_Unlock();
    for (uint32_t off = 0U; off < len; off += 4U)
    {
        uint32_t word;
        memcpy(&word, buf + off, sizeof(word));

        if (FLASH_ProgramWord(addr + off, word) != FLASH_COMPLETE)
        {
            FLASH_Lock();
            return false;
        }
    }
    FLASH_Lock();

    return true;
}

/**
 * @brief  包头合法性检查（静态字段）
 * @param  h 包头指针
 * @return true = 合法
 * @note   magic 识别格式、header_size 识别版本、hw_id 防止刷错
 *         机型、fw_size 防止越界写坏 Bootloader/参数区。
 */
static bool Header_IsValid(const ota_hdr_t *h)
{
    if (h->magic != OTA_PKG_MAGIC)
        return false;
    if (h->header_size != OTA_PKG_HEADER_SIZE)
        return false;
    if (h->hw_id != OTA_PKG_HW_ID)
        return false;
    if (h->fw_size == 0U || h->fw_size > APP_SIZE)
        return false;
    return true;
}

/**
 * @brief  流式校验文件 payload 的 CRC32
 * @param  path 文件路径
 * @param  h    已通过静态校验的包头
 * @return true = CRC 一致
 */
static bool Payload_Check(const char *path, const ota_hdr_t *h)
{
    uint32_t remain = h->fw_size; // payload 字节数
    uint32_t crc = 0xFFFFFFFFUL;

    if (Boot_Lfs_FileOpen(path) != LFS_ERR_OK)
        return false;

    /* 跳过 64 B 包头：读出来丢掉 */
    if (File_ReadFull(s_buf, OTA_PKG_HEADER_SIZE) < 0)
    {
        Boot_Lfs_FileClose();
        return false;
    }

    /* 读 payload 并算 CRC */
    while (remain > 0U)
    {
        uint32_t n = (remain > UPDATE_BUF_SIZE) ? UPDATE_BUF_SIZE : remain; // 需要读取的字节数

        if (File_ReadFull(s_buf, n) < 0)
        {
            Boot_Lfs_FileClose();
            return false;
        }

        crc = Boot_CRC32_Update(crc, s_buf, n);
        remain -= n;
    }

    Boot_Lfs_FileClose();

    return (~crc) == h->fw_crc32;
}

/**
 * @brief  通用搬运流程，升级与回滚共用
 * @param  path           源文件路径
 * @param  success_state  成功后参数 state（升级/回滚不同）
 * @param  success_flags  成功后参数 flags
 * @return false = 失败（文件打不开/包头错/CRC 错/擦写失败）
 */
static bool Update_Run(const char *path, uint32_t success_state, uint32_t success_flags)
{
    ota_hdr_t hdr;     // OTA 包头
    boot_param_t p;    // Bootloader 参数
    uint32_t remain;   // payload 字节数
    uint32_t last_pct; // 上次打印的进度

    /* 1.挂载文件系统 */
    Boot_Uart_Puts("[BOOT] mount littlefs ...\n");
    if (Boot_Lfs_Mount() != LFS_ERR_OK)
    {
        Boot_Uart_Puts("[BOOT] mount failed\n");
        return false;
    }

    Boot_Uart_Puts("[BOOT] open ");
    Boot_Uart_Puts(path);
    Boot_Uart_Puts("\n");

    /* 2.读 OTA 包头 */
    if (Boot_Lfs_FileOpen(path) != LFS_ERR_OK)
    {
        Boot_Uart_Puts("[BOOT] open failed\n");
        return false;
    }
    if (File_ReadFull(&hdr, sizeof(hdr)) < 0)
    {
        Boot_Lfs_FileClose();
        Boot_Uart_Puts("[BOOT] read header failed\n");
        return false;
    }
    Boot_Lfs_FileClose();

    /* 3.静态校验包头 */
    if (!Header_IsValid(&hdr))
    {
        Boot_Uart_Puts("[BOOT] bad header\n");
        return false;
    }

    Boot_Uart_Puts("[BOOT] fw_size=");
    Boot_Uart_PutU32(hdr.fw_size);
    Boot_Uart_Puts(" ver=");
    Boot_Uart_PutHex(hdr.fw_version);
    Boot_Uart_Puts("\n");

    /* 4.整包 CRC 校验 */
    Boot_Uart_Puts("[BOOT] check crc ...\n");
    if (!Payload_Check(path, &hdr))
    {
        Boot_Uart_Puts("[BOOT] crc mismatch\n");
        return false;
    }

    /* 5.标记"搬运中"：途中可能掉电，下次启动看到
     *   state=OTA_WRITING 会重新走完整搬运流程 */
    Boot_Param_Default(&p);
    if (Boot_Param_Load(&p))
        p.seq++;
    else
        p.seq = 1U;

    p.state = BOOT_ST_OTA_WRITING;
    p.ota_size = hdr.fw_size;
    p.ota_crc32 = hdr.fw_crc32;
    p.ota_version = hdr.fw_version;
    if (!Boot_Param_Save(&p))
    {
        Boot_Uart_Puts("[BOOT] param save failed\n");
        return false;
    }

    /* 6.擦除 App 区 */
    if (!Flash_EraseApp())
    {
        Boot_Uart_Puts("[BOOT] erase failed\n");
        return false;
    }

    Boot_Uart_Puts("[BOOT] write app ...\n");

    /* 7.重新打开文件从头读；先跳过 64 B 头再搬 payload */
    if (Boot_Lfs_FileOpen(path) != LFS_ERR_OK)
        return false;
    if (File_ReadFull(s_buf, OTA_PKG_HEADER_SIZE) < 0)
    {
        Boot_Lfs_FileClose();
        return false;
    }

    remain = hdr.fw_size; // payload 字节数
    last_pct = 0xFFFFFFFFUL;
    {
        uint32_t off = 0U; // 已写入节数

        while (remain > 0U)
        {
            uint32_t n = (remain > UPDATE_BUF_SIZE) ? UPDATE_BUF_SIZE : remain;

            /* 读 payload */
            if (File_ReadFull(s_buf, n) < 0)
            {
                Boot_Lfs_FileClose();
                Boot_Uart_Puts("[BOOT] read payload failed\n");
                return false;
            }

            /* 写 payload 到 App 区 */
            if (!Flash_Write(APP_BASE + off, (const uint8_t *)s_buf, n))
            {
                Boot_Lfs_FileClose();
                Boot_Uart_Puts("[BOOT] flash write failed\n");
                return false;
            }

            off += n;
            remain -= n;
        }
    }
    Boot_Lfs_FileClose();

    /* 8.回读校验：直接读内部 Flash 的 App 区算 CRC，
     *   与包头里的 fw_crc32 比对，确认写入过程没有位错误。 */
    Boot_Uart_Puts("[BOOT] verify ...\n");

    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t a = 0U; a < hdr.fw_size; a += 4U)
    {
        uint32_t word = *(volatile uint32_t *)(APP_BASE + a);
        crc = Boot_CRC32_Update(crc, &word, sizeof(word));
    }
    if ((~crc) != hdr.fw_crc32)
    {
        Boot_Uart_Puts("[BOOT] verify failed\n");
        return false;
    }

    /* 9.搬完更新参数。两种情况：
     *   升级：APP_UNCONFIRMED + boot_try=1
     *     —— "新 App 这是第 1 次运行"；之后每次 Bootloader
     *        启动都会 try++，3 次没确认就回滚。
     *   回滚：IDLE + CONFIRMED
     *     —— 备份固件是上一个已确认过的版本，直接恢复成正常状态。 */
    Boot_Param_Default(&p);
    if (Boot_Param_Load(&p))
        p.seq++;
    else
        p.seq = 1U;

    p.state = success_state;
    p.app_size = hdr.fw_size;
    p.app_crc32 = hdr.fw_crc32;
    p.app_version = hdr.fw_version;
    p.boot_try = (success_state == BOOT_ST_APP_UNCONFIRMED) ? 1U : 0U;
    p.flags = success_flags | BOOT_FLAG_OTA_VERIFIED;
    if (!Boot_Param_Save(&p))
    {
        Boot_Uart_Puts("[BOOT] param save failed\n");
        return false;
    }

    /* 10.跳转到 App 区 */
    Boot_Uart_Puts("[BOOT] done, jump\n");
    Boot_JumpToApp();

    return true;
}

/**
 * @brief  升级：从指定文件搬运新固件
 * @param  path 源文件路径(/ota/firmware.bin)
 * @return false = 失败；成功不返回
 * @note   成功后进入"待确认"（APP_UNCONFIRMED + boot_try=1）；
 *         App 启动 5 秒写 CONFIRMED，连续 3 次没写则回滚。
 */
bool Boot_Update_FromFile(const char *path)
{
    return Update_Run(path, BOOT_ST_APP_UNCONFIRMED, BOOT_FLAG_BACKUP_VALID);
}

/**
 * @brief  回滚：从备份文件恢复旧固件
 * @param  path 备份文件路径（通常为 /ota/backup.bin）
 * @return false = 失败；成功不返回
 * @note   备份是旧版本、之前已确认过，成功后直接 IDLE + CONFIRMED，
 *         不再要求重新确认（否则升级失败一次就要等 5 秒 x 3 次）。
 */
bool Boot_Rollback_FromFile(const char *path)
{
    return Update_Run(path, BOOT_ST_IDLE,
                      BOOT_FLAG_BACKUP_VALID | BOOT_FLAG_CONFIRMED);
}
