#include "OTA_Store.h"

#include "lfs.h"
#include "lfs_config.h"
#include "lfs_port.h"
#include "boot_layout.h"

/* ============================================================
 * OTA 文件存储：App 侧对 littlefs 的写接口
 * ------------------------------------------------------------
 * 为什么不在 OTA.c 里直接调 lfs_*？
 *   1. 项目约定"外部 Flash 只由固定入口访问"：字库图片走 Asset，
 *      OTA 文件走这里，路径、锁、错误码都收拢；
 *   2. 所有操作都包 LittleFS_Lock：OTA 写文件期间 ui 任务可能
 *      同时读字库/图片，两者共享 SPI1 和同一个 lfs 实例，
 *      不加锁会互相踩；
 *   3. 单句柄设计：同一时刻只打开一个文件，OTA 任务独占使用，
 *      不需要管理句柄数组。
 *
 * 锁的粒度说明：OpenWrite 拿锁只保护 open 动作本身，
 * Write/Close 各自拿锁。会话期间文件句柄保持打开，但锁是
 * 逐操作释放的——这样 ui 任务读资源只是被"每个 4 KB 写操作"
 * 短暂阻塞，而不是被整个 15 秒下载过程饿死。
 * ============================================================ */

static lfs_file_t s_file;        // 当前打开的文件句柄
static bool s_file_open = false; // 当前是否有打开的文件句柄

/** @brief  确保 /ota 目录存在 */
int OTA_Store_EnsureDir(void)
{
    int err;

    LittleFS_Lock();
    err = lfs_mkdir(&g_lfs, OTA_DIR);
    LittleFS_Unlock();

    if (err == LFS_ERR_EXIST)
        err = LFS_ERR_OK;

    return err;
}

/**
 * @brief  打开（或创建）文件准备写入
 * @note   LFS_O_TRUNC：从 0 开始覆盖，配合 tmp 文件策略不留旧内容。
 */
int OTA_Store_OpenWrite(const char *path)
{
    int err;

    if (s_file_open)
        return LFS_ERR_IO;

    LittleFS_Lock();
    err = lfs_file_open(&g_lfs, &s_file, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err == LFS_ERR_OK)
        s_file_open = true;
    LittleFS_Unlock();

    return err;
}

/**
 * @brief  写入文件
 * @param  buf 数据缓冲
 * @param  len 字节数
 * @return 实际写入字节数,负数错误
 */
int OTA_Store_Write(const uint8_t *buf, uint32_t len)
{
    int err;

    if (!s_file_open)
        return LFS_ERR_IO;

    LittleFS_Lock();
    err = lfs_file_write(&g_lfs, &s_file, buf, (lfs_size_t)len);
    LittleFS_Unlock();

    return err;
}

/** @brief  关闭文件 */
int OTA_Store_Close(void)
{
    int err = LFS_ERR_OK;

    if (s_file_open)
    {
        LittleFS_Lock();
        err = lfs_file_close(&g_lfs, &s_file);
        LittleFS_Unlock();
        s_file_open = false;
    }

    return err;
}

/**
 * @brief  原子改名：tmp -> 正式文件名
 * @param  old_path 旧路径
 * @param  new_path 新路径
 * @return LFS_ERR_OK 成功；其他为 littlefs 错误码
 * @note   littlefs 的 rename 是 copy-on-write 元数据操作，
 *         掉电时要么旧名要么新名，不存在中间态。
 */
int OTA_Store_Rename(const char *old_path, const char *new_path)
{
    int err;

    LittleFS_Lock();
    err = lfs_rename(&g_lfs, old_path, new_path);
    LittleFS_Unlock();

    return err;
}

/** @brief  删除文件 */
int OTA_Store_Remove(const char *path)
{
    int err;

    LittleFS_Lock();
    err = lfs_remove(&g_lfs, path);
    LittleFS_Unlock();

    return err;
}

/**
 * @brief  查文件大小
 * @param  path 文件路径
 * @param  size 输出：字节数
 * @return LFS_ERR_OK 成功；其他为 littlefs 错误码
 */
int OTA_Store_FileSize(const char *path, uint32_t *size)
{
    struct lfs_info info;
    int err;

    LittleFS_Lock();
    err = lfs_stat(&g_lfs, path, &info);
    LittleFS_Unlock();

    if (err == LFS_ERR_OK && size != NULL)
        *size = (uint32_t)info.size;

    return err;
}

/**
 * @brief  文件系统剩余空间（字节）
 * @return 剩余字节数
 */
uint32_t OTA_Store_FreeBytes(void)
{
    lfs_ssize_t used;
    uint32_t free_bytes = 0U;

    LittleFS_Lock();
    used = lfs_fs_size(&g_lfs); // 已用块数
    LittleFS_Unlock();

    if (used > 0 && (uint32_t)used < LFS_BLOCK_COUNT)
        free_bytes = (uint32_t)(LFS_BLOCK_COUNT - (uint32_t)used) * LFS_BLOCK_SIZE;

    return free_bytes;
}
