#include "OTA.h"

#include "boot_layout.h"
#include "boot_param.h"
#include "boot_crc32.h"
#include "ota_pkg.h"
#include "Version.h"
#include "OTA_Store.h"
#include "OTA_Serial.h"

#include "FreeRTOS.h"
#include "task.h"

#include "lfs.h"
#include "BuildConfig.h"

#include <string.h>

/* ============================================================
 * 离线 OTA 状态机（App 侧）
 * ------------------------------------------------------------
 * 职责：通过 USART2 接收 PC 发来的 .ota 文件，写入 littlefs，
 * 校验无误后备份当前固件、写参数请求 Bootloader 搬运，最后复位。
 *
 * 协议（PC 工具 ota_send.py 与此保持一致）：
 *
 *   PC -> MCU: 0x55 0xAA 'O' 'T' 'A'            握手
 *   MCU -> PC: 0xAA 0x55 'O' 'K'                 就绪
 *   PC -> MCU: 元数据帧（ota_meta_t，20 B，小端）
 *   MCU -> PC: 0xAA 0x55 'O' 'K'                 元数据通过
 *   PC -> MCU: 裸 .ota 文件字节流（total_len 字节）
 *   MCU -> PC: 0xAA 0x55 'D' 'O' 'N' 'E'         成功（随后复位）
 *          或 0xAA 0x55 'F' 'A' 'I' 'L'         失败（由 OTA_Task 发送）
 *
 * 先写 .tmp 再 rename:
 *   Bootloader 只认 /ota/firmware.bin。如果下载到一半掉电，
 *   .bin 是半个固件，Bootloader 会拿到 CRC 错误的文件（虽然
 *   不会搬，但要等它校验才知道）。用 .tmp 接收、校验通过后
 *   原子地 rename 成 .bin，Bootloader 任何时候看到的 .bin
 *   都是完整文件。
 * ============================================================ */

#define OTA_META_SIZE 20U // 元数据帧大小
#define OTA_CHUNK 4096U   // 4 KB 收发缓冲

static const uint8_t s_ack_ok[4] = {0xAA, 0x55, 'O', 'K'};
static const uint8_t s_ack_done[6] = {0xAA, 0x55, 'D', 'O', 'N', 'E'};
static uint8_t s_buf[OTA_CHUNK]; // 4 KB 收发缓冲

/**
 * @brief  计算自身固件大小（备份/回滚/自检共用）
 * @return 固件字节数（4 字节对齐）；0 = 无可用来源
 * @note   两级来源：
 *           1. 参数区 app_size：OTA 升级时 Bootloader 按 .ota
 *              包头 fw_size 写入，精确等于 bin 长度；
 *           2. APP_FW_SIZE_FALLBACK：参数区为空（ST-Link 直接
 *              烧录、从未 OTA 过）时的兜底常量，BuildConfig.h
 *              中维护，必须 >= 实际固件大小。
 *         备份读多少字节就按多少算 CRC、回滚写多少字节，
 *         两条路径各自自洽，不要求与打包工具完全一致。
 */
uint32_t OTA_AppSelfSize(void)
{
    boot_param_t p;
    uint32_t size = 0U;

    /* 一级来源：参数区 app_size。但只有 APP_UNCONFIRMED 状态可信：
     * 它由 Bootloader 刚按 .ota 包头 fw_size 写入，精确等于 bin 长度。
     * IDLE/CONFIRMED 状态的值可能是"上一次 ST-Link 烧录"留下的旧值，
     * 继承它可能小于当前固件造成备份截断（Phase 1 黑屏事故的变体）。 */
    if (Boot_Param_Load(&p) &&
        p.state == BOOT_ST_APP_UNCONFIRMED &&
        p.app_size >= 0x1000U && p.app_size <= APP_SIZE)
        size = p.app_size;

    /* 二级来源：其余场景（参数区空 / ST-Link 烧录后）用兜底常量，
     * 人工保证 >= 实际固件大小；多读尾部 0xFF 无害。 */
    if (size == 0U)
        size = APP_FW_SIZE_FALLBACK;

    if (size == 0U || size > APP_SIZE)
        return 0U;

    /* 向上取整到 4：Bootloader 按 4 字节写 Flash */
    return (size + 3U) & ~3U;
}

/**
 * @brief  计算自身固件的大小与 CRC32
 * @param  size  输出：固件字节数（可为 NULL）
 * @param  crc32 输出：固件 CRC32（可为 NULL）
 * @return true = 计算成功
 * @note   Boot_Confirm_Task（app_task.c）在启动确认时调用，
 *         把结果写进参数区，Bootloader 显示版本、OTA 备份时复用。
 *         读内部 Flash 约 0.3 s，只执行一次。
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
 * @brief  CRC16-CCITT（poly 0x1021，init 0xFFFF）
 * @param  data 数据首地址
 * @param  len  字节数
 * @return 16 位校验值
 * @note   用于 backup.bin 的包头 header_crc16，与打包工具算法一致；
 *         和 CRC32 一样，PC 端必须用同一套参数，否则对不上。
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
 * @brief  等待收满 len 字节
 * @param  buf        接收缓冲
 * @param  len        期望字节数
 * @param  timeout_ms 超时
 * @return true = 收满；false = 超时
 * @note   用 vTaskDelay(1) 让出 CPU：字节由 USART2 中断写进
 *         环形缓冲，任务只需轮询缓冲，不需要忙等。
 */
static bool Wait_Bytes(uint8_t *buf, uint32_t len, uint32_t timeout_ms)
{
    uint32_t got = 0U;
    uint32_t start = (uint32_t)xTaskGetTickCount();

    while (got < len)
    {
        uint16_t n = OTA_Serial_Read(buf + got, (uint16_t)(len - got));
        got += n;

        if (got >= len)
            return true;

        if ((uint32_t)((uint32_t)xTaskGetTickCount() - start) >=
            (timeout_ms / portTICK_PERIOD_MS))
            return false;

        vTaskDelay(1);
    }

    return true;
}

/**
 * @brief  备份当前固件为 .ota 格式写入 backup.bin
 * @return OTA_OK 成功；OTA_ERR_BACKUP 失败
 * @note   新固件可能"CRC 正确但跑不起来"（逻辑 bug），回滚是
 *         唯一不需要人工介入的恢复手段。

 *         为什么在写参数 OTA_PENDING 之前做？
 *         备份失败就直接中止，旧 App 完好无损。
 *         流程：读自身 Flash -> 算 CRC -> 构造 .ota 头 ->
 *         写 .tmp -> rename（原子替换）。
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
 * @brief  执行一次完整 OTA 会话
 * @return 错误码；成功路径以 NVIC_SystemReset 结束，OTA_OK 不会返回
 * @note   九步流程：握手 -> 元数据 -> 目录与空间 -> 接收写 tmp
 *         （边收边算 CRC）-> CRC 校验 -> rename -> 备份 ->
 *         写参数 OTA_PENDING -> 回执并复位。
 */
ota_result_t OTA_Run(void)
{
    ota_meta_t meta;
    uint32_t remaining;
    uint32_t crc;
    uint32_t skipped;
    uint32_t fw_size;
    int err;

    /* 会话开始先清空接收缓冲：上次残留的字节会破坏帧同步 */
    OTA_Serial_Flush();

    /* 1. 握手
     * 五字节魔数，用于和串口助手乱发的内容区分。
     * 超时给 60 s：等待用户操作 PC 工具。 */
    {
        uint8_t hs[5];

        if (!Wait_Bytes(hs, sizeof(hs), 60000U))
            return OTA_ERR_TIMEOUT;

        if (memcmp(hs, "\x55\xAAOTA", 5) != 0)
            return OTA_ERR_PROTO;
    }
    OTA_Serial_Send(s_ack_ok, sizeof(s_ack_ok));

    /* 2. 元数据
     * 先拿到包大小/CRC/版本，一切合法才允许 PC 开始发数据。 */
    if (!Wait_Bytes((uint8_t *)&meta, OTA_META_SIZE, 10000U))
        return OTA_ERR_TIMEOUT;

    if (meta.magic != 0x55AAU || meta.len != OTA_META_SIZE)
        return OTA_ERR_PROTO;
    if (meta.hw_id != OTA_PKG_HW_ID)
        return OTA_ERR_HWID;
    if (meta.total_len <= OTA_PKG_HEADER_SIZE ||
        meta.total_len > OTA_PKG_HEADER_SIZE + APP_SIZE)
        return OTA_ERR_SIZE;
    if (meta.version <= APP_VERSION) // 只允许升级到更高版本；如需降级测试可临时放开
        return OTA_ERR_VERSION;

    fw_size = meta.total_len - (uint32_t)OTA_PKG_HEADER_SIZE;
    OTA_Serial_Send(s_ack_ok, sizeof(s_ack_ok));

    /* 3. 目录与空间
     * 需要容纳：新固件 + 即将写入的备份 + 64 KB 余量。
     * 提前检查，避免写到一半 littlefs 报 NOSPC。 */
    if (OTA_Store_EnsureDir() != LFS_ERR_OK)
        return OTA_ERR_STORE;
    if (OTA_Store_FreeBytes() < meta.total_len * 2U + 65536U)
        return OTA_ERR_SIZE;

    /* 4. 接收并写入 firmware.tmp，边收边算 payload CRC
     * CRC 跳过前 64 B 包头（包头有自己的 header_crc16）。
     * 接收超时 5 s/块：115200 下 4 KB 约 0.36 s，5 s 很宽松。 */
    if (OTA_Store_OpenWrite(OTA_FW_TMP_PATH) != LFS_ERR_OK)
        return OTA_ERR_STORE;

    remaining = meta.total_len;
    crc = 0xFFFFFFFFUL;
    skipped = 0U; // 已经跳过的头字节

    while (remaining > 0U)
    {
        uint32_t n = (remaining > OTA_CHUNK) ? OTA_CHUNK : remaining; // 每次接收 OTA_CHUNK 字节

        if (!Wait_Bytes(s_buf, n, 5000U))
        {
            (void)OTA_Store_Close();
            return OTA_ERR_TIMEOUT;
        }

        if (OTA_Store_Write(s_buf, n) != (int)n)
        {
            (void)OTA_Store_Close();
            return OTA_ERR_STORE;
        }

        /* 收流 = 64 B 头 + payload；头不参与 payload CRC */
        if (skipped < OTA_PKG_HEADER_SIZE)
        {
            uint32_t skip = (uint32_t)OTA_PKG_HEADER_SIZE - skipped; // 还余没有跳过的头字节

            /* 本块全是头字节 */
            if (n <= skip)
            {
                skipped += n;
            }
            /* 本块头字节和 payload 字节混合 */
            else
            {
                crc = Boot_CRC32_Update(crc, s_buf + skip, n - skip);
                skipped = (uint32_t)OTA_PKG_HEADER_SIZE;
            }
        }
        /* 已经跳过头字节 */
        else
        {
            crc = Boot_CRC32_Update(crc, s_buf, n);
        }

        remaining -= n;
    }

    err = OTA_Store_Close();
    if (err != LFS_ERR_OK)
        return OTA_ERR_STORE;

    /* 5. CRC 校验，失败清理 tmp，旧固件不受影响 */
    if ((~crc) != meta.crc32)
    {
        (void)OTA_Store_Remove(OTA_FW_TMP_PATH);
        return OTA_ERR_CRC;
    }

    /* 6~9. 先回执 DONE 给 PC，再走与网络升级共用的提交流程 */
    OTA_Serial_Send(s_ack_done, sizeof(s_ack_done));

    return OTA_CommitFirmware(fw_size, meta.crc32, meta.version);
}


/**
 * @brief  提交已下载好的 /ota/firmware.tmp：改名 -> 备份 -> 写参数 -> 复位
 * @param  fw_size    固件 payload 字节数（不含 64 B 包头）
 * @param  fw_crc32   payload CRC32（调用前必须已校验通过）
 * @param  fw_version 版本编码（major<<16 | minor<<8 | patch）
 * @return 失败返回错误码；成功不返回（延时 1 秒后复位）
 * @note   串口离线升级（OTA_Run）与 Phase 2 网络升级共用本函数。
 *         调用前必须保证 firmware.tmp 已完整写入且 CRC 已通过。
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
    {
        ota_result_t br = Backup_Current();
        if (br != OTA_OK)
        {
            (void)OTA_Store_Remove(OTA_FW_PATH);
            return br;
        }
    }

    /* 3. 写参数：请求 Bootloader 搬运。这是"点球时刻"：写完这条
     * 参数，复位后 Bootloader 就会擦 App 区开始搬运 */
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

    /* 4. 留 1 秒给 UI/串口回执，然后复位进 Bootloader 搬运 */
    vTaskDelay(pdMS_TO_TICKS(1000));
    NVIC_SystemReset();

    return OTA_OK;
}
