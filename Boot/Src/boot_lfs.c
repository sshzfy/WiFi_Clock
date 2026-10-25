#include "boot_lfs.h"

#include "lfs.h"
#include "lfs_config.h"
#include "W25Q64.h"

#include "stm32f4xx.h"

#include <string.h>

/* ============================================================
 * Bootloader 侧 littlefs 只读访问
 * ------------------------------------------------------------
 * 为什么 Bootloader 要读 littlefs？
 *   OTA 固件存放在 W25Q64 的 littlefs 里（/ota/firmware.bin），
 *   因为"不重新烧录资源"的约束不允许把 W25Q64 重新划裸分区。
 *   Bootloader 要搬运固件，就必须能读文件系统。
 *
 * 为什么只读？
 *   Bootloader 的职责是"校验 + 搬运 + 跳转"，它不需要写文件：
 *   备份由 App 在重启前完成。只读的好处：
 *     - lfs.c 以 LFS_READONLY 编译，代码约小一半；
 *     - 绝对不会因为 Bootloader 误写破坏资源文件系统；
 *     - 挂载过程不产生任何写操作，上电即读。
 *
 * 实现要点：
 *   1. littlefs 不直接操作 Flash，而是通过 lfs_config 里的回调：
 *      本文件的 bf_read 把"逻辑块号 x 块大小 + 块内偏移"换算成
 *      W25Q64 物理地址后调用 W25Q64_Read。
 *   2. block_size/block_count 必须与 App 侧 lfs_port.c 完全一致
 *      （4096 / 2048），否则挂载失败；缓存大小只影响性能。
 *   3. 读文件用 lfs_file_opencfg 指定 4 KB 缓冲，减少 SPI 事务次数；
 *      lfs_file_read 可能一次读不满请求的长度，上层用
 *      File_ReadFull（见 boot_update.c）循环读满。
 *   4. Bootloader 是单线程裸机，不需要 FreeRTOS 互斥锁。
 * ============================================================ */

static lfs_t s_lfs;            // littlefs 实例
static bool s_mounted = false; // 是否挂载成功

static uint8_t s_read_buf[LFS_CACHE_SIZE];      // 读缓存
static uint8_t s_prog_buf[LFS_CACHE_SIZE];      // 写缓存（只读模式不用）
static uint8_t s_lookahead[LFS_LOOKAHEAD_SIZE]; // 分配预读位图

/* 文件读缓冲：作为 lfs_file_opencfg 的 cache。
 * 4 字节对齐是 lfs 的要求（它按 uint32 访问 cache）。 */
__attribute__((aligned(4))) static uint8_t s_file_buf[4096]; // 文件读缓冲, 4 KB
static lfs_file_t s_file;                                    // 文件句柄
static bool s_file_open = false;                             // 是否打开文件

/**
 * @brief  读回调：把 littlefs 逻辑块换算成 W25Q64 物理地址后读取
 * @note   block 从 0 开始连续铺满整个 W25Q64，与 lfs_port.c 约定一致。
 */
static int bf_read(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, void *buffer, lfs_size_t size)
{
    uint32_t addr = (uint32_t)block * (uint32_t)c->block_size + (uint32_t)off;
    W25Q64_Read(addr, (uint8_t *)buffer, (uint32_t)size);
    return LFS_ERR_OK;
}

/**
 * @brief  写回调：只读模式不会被调用，返回错误作为保险
 * @note   代码 bug 导致只读挂载时发生写操作，报错
 *         不写坏资源文件系统。
 */
static int bf_prog(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, const void *buffer, lfs_size_t size)
{
    (void)c;
    (void)block;
    (void)off;
    (void)buffer;
    (void)size;
    return LFS_ERR_IO;
}

/** @brief  擦除回调：只读模式不会被调用，返回错误作为保险 */
static int bf_erase(const struct lfs_config *c, lfs_block_t block)
{
    (void)c;
    (void)block;
    return LFS_ERR_IO;
}

/** @brief  同步回调：W25Q64 底层已阻塞等待，无需额外处理 */
static int bf_sync(const struct lfs_config *c)
{
    (void)c;
    return LFS_ERR_OK;
}

/* 只读挂载所需的完整配置 */
static const struct lfs_config s_cfg =
    {
        .context = NULL,
        .read = bf_read,
        .prog = bf_prog,
        .erase = bf_erase,
        .sync = bf_sync,
        .read_size = LFS_READ_SIZE,
        .prog_size = LFS_PROGRAM_SIZE,
        .block_size = LFS_BLOCK_SIZE,
        .block_count = LFS_BLOCK_COUNT,
        .block_cycles = LFS_BLOCK_CYCLES,
        .cache_size = LFS_CACHE_SIZE,
        .lookahead_size = LFS_LOOKAHEAD_SIZE,
        .compact_thresh = 0,
        .read_buffer = s_read_buf,
        .prog_buffer = s_prog_buf,
        .lookahead_buffer = s_lookahead,
};

/**
 * @brief  挂载 littlefs
 * @return LFS_ERR_OK 成功；其他为 littlefs 错误码
 */
int Boot_Lfs_Mount(void)
{
    if (s_mounted)
        return LFS_ERR_OK;

    /* W25Q64 驱动只配引脚/外设，不使能时钟（App 里由
     * Board_Peripheral_Init 使能），Bootloader 需要自己使能。 */
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE);
    W25Q64_Init();

    int err = lfs_mount(&s_lfs, &s_cfg);
    if (err == LFS_ERR_OK)
        s_mounted = true;

    return err;
}

/**
 * @brief  查询文件系统是否已挂载
 * @return true = 已挂载
 */
bool Boot_Lfs_IsMounted(void)
{
    return s_mounted;
}

/**
 * @brief  打开文件（只读），并挂上 4 KB cache
 * @param  path 文件路径
 * @return LFS_ERR_OK 成功；其他为 littlefs 错误码
 * @note   同一时刻只允许一个文件打开：搬运流程是严格顺序的，
 *         多个句柄只会浪费 RAM 和增加状态管理成本。
 */
int Boot_Lfs_FileOpen(const char *path)
{
    struct lfs_file_config fcfg;

    if (!s_mounted || s_file_open)
        return LFS_ERR_IO;

    memset(&fcfg, 0, sizeof(fcfg));
    fcfg.buffer = s_file_buf;

    int err = lfs_file_opencfg(&s_lfs, &s_file, path, LFS_O_RDONLY, &fcfg);
    if (err == LFS_ERR_OK)
        s_file_open = true;

    return err;
}

/**
 * @brief  顺序读文件
 * @param  buf 读入缓冲
 * @param  len 期望字节数
 * @return 实际读到的字节数（可能小于请求值）；负数为错误
 * @note   调用方需要自己循环读满（见 boot_update.c 的 File_ReadFull）。
 */
int Boot_Lfs_FileRead(void *buf, uint32_t len)
{
    if (!s_file_open)
        return LFS_ERR_IO;

    return (int)lfs_file_read(&s_lfs, &s_file, buf, (lfs_size_t)len);
}

/**
 * @brief  关闭当前打开的文件
 * @return LFS_ERR_OK 成功；其他为 littlefs 错误码
 */
int Boot_Lfs_FileClose(void)
{
    int err = LFS_ERR_OK;

    if (s_file_open)
    {
        err = lfs_file_close(&s_lfs, &s_file);
        s_file_open = false;
    }

    return err;
}

/**
 * @brief  查文件大小（不打开文件），可用于搬运前的合法性检查
 * @param  path 文件路径
 * @param  size 输出：文件字节数
 * @return LFS_ERR_OK 成功；其他为 littlefs 错误码
 */
int Boot_Lfs_FileSize(const char *path, uint32_t *size)
{
    struct lfs_info info;

    if (!s_mounted)
        return LFS_ERR_IO;

    int err = lfs_stat(&s_lfs, path, &info);
    if (err == LFS_ERR_OK && size != NULL)
        *size = (uint32_t)info.size;

    return err;
}
