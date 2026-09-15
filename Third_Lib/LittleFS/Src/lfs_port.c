#include "lfs_port.h"

/* ================ LittleFS 移植 ==================
 * littlefs 不直接操作 Flash，而是通过 lfs_config 中注册的
 * read / prog / erase / sync 四个回调访问底层存储
 * 本文件负责：
 *   1. 定义全局 littlefs 实例 g_lfs
 *   2. 提供 W25Q64 的读、写、擦除、同步回调
 *   3. 提供挂载、自动格式化、强制格式化接口
 * ====================================*/

/* littlefs 全局实例。
 * 后续所有文件操作，例如 lfs_file_open(&g_lfs, ...)，都使用这个实例。
 * 它保存了挂载后的文件系统运行时状态。 */
lfs_t g_lfs;

static int lfs_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size);
static int lfs_program(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size);
static int lfs_erase(const struct lfs_config *c, lfs_block_t block);
static int lfs_sync(const struct lfs_config *c);

/* ================== littlefs 运行需要三个缓冲区 ==================
 *   read_buffer      : 读缓存，大小必须 >= cache_size；
 *   prog_buffer      : 写缓存，大小必须 >= cache_size；
 *   lookahead_buffer : 块分配预读位图，大小必须为 8 的倍数。
 * ==================================== */

static uint8_t s_lfs_read_buffer[LFS_CACHE_SIZE];
static uint8_t s_lfs_prog_buffer[LFS_CACHE_SIZE];
static uint8_t s_lfs_lookahead_buffer[LFS_LOOKAHEAD_SIZE];

/* ================== littlefs 配置对象 ==================
 * 该结构体告诉 littlefs：
 *   - 底层块设备如何读写擦除；
 *   - 存储器的读/写/擦除粒度；
 *   - 有多少个可擦除块；
 *   - 使用哪些缓存区。
 * ==================================== */

const struct lfs_config g_lfs_config = {
    /* 底层操作函数 */
    .read = lfs_read,    // 读回调, 从 W25Q64 读取数据
    .prog = lfs_program, // 写回调, 向 W25Q64 写入数据
    .erase = lfs_erase,  // 擦除回调, 擦除 W25Q64 一个逻辑块
    .sync = lfs_sync,    // 同步回调, W25Q64 底层已阻塞等待，空实现即可
    /* 粒度配置 */
    .read_size = LFS_READ_SIZE,     // 最小读取大小
    .prog_size = LFS_PROGRAM_SIZE,  // 最小编程大小
    .block_size = LFS_BLOCK_SIZE,   // 擦除块大小 = 4KB 扇区
    .block_count = LFS_BLOCK_COUNT, // 块数量 = 8MB/4KB = 2048
    /* 缓存配置 */
    .cache_size = LFS_CACHE_SIZE,         // 缓存大小
    .lookahead_size = LFS_LOOKAHEAD_SIZE, // 预读大小(必须为8的倍数)
    .block_cycles = LFS_BLOCK_CYCLES,     // 磨损均衡周期
    /* 缓存区配置 */
    .read_buffer = s_lfs_read_buffer,           // 读缓存
    .prog_buffer = s_lfs_prog_buffer,           // 写缓存
    .lookahead_buffer = s_lfs_lookahead_buffer, // 预读缓存
};

/**
 * @brief 读回调
 *
 * @param c      littlefs 配置对象，包含 block_size 等信息
 * @param block  逻辑块号，从 0 开始
 * @param off    块内字节偏移，范围 [0, block_size)
 * @param buffer 输出缓冲区，用于接收读到的数据
 * @param size   要读取的字节数
 *
 * @return LFS_ERR_OK 表示读取成功
 *
 * @note
 *   W25Q64 支持任意地址读取，因此这里直接把逻辑块号换算成物理地址：
 *   addr = block * block_size + off
 *   由于 LFS_BLOCK_SIZE 等于 W25Q64 的扇区大小 4096，
 *   所以 block 号本质上就是扇区号。
 */
static int lfs_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size)
{
    uint32_t addr = (uint32_t)block * c->block_size + off; // 计算物理地址
    W25Q64_Read(addr, (uint8_t *)buffer, size);

    return LFS_ERR_OK;
}

/**
 * @brief 写回调
 *
 * @param c      littlefs 配置对象
 * @param block  逻辑块号
 * @param off    块内字节偏移
 * @param buffer 输入缓冲区，保存要写入的数据
 * @param size   要编程的字节数
 *
 * @return LFS_ERR_OK 成功；LFS_ERR_IO 底层忙等待超时或写失败
 *
 * @note
 *   littlefs 保证：
 *   1. 写入区域已经擦除；
 *   2. size 是 prog_size 的整数倍；
 *   3. 因为 prog_size = 16 能整除 W25Q64 页大小 256，
 *      所以单次写入不会跨页。
 */
static int lfs_program(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size)
{
    uint32_t addr = (uint32_t)block * c->block_size + off; // 计算物理地址

    if (!W25Q64_PageProgram(addr, (const uint8_t *)buffer, size))
    {
        return LFS_ERR_IO;
    }

    return LFS_ERR_OK;
}

/**
 * @brief 擦除回调
 *
 * @param c      littlefs 配置对象
 * @param block  要擦除的逻辑块号
 *
 * @return LFS_ERR_OK 成功；LFS_ERR_IO 底层忙等待超时或擦除失败
 *
 * @note
 *   LFS_BLOCK_SIZE = 4096，正好等于 W25Q64 的最小擦除单位：扇区。
 *   因此一个 littlefs 逻辑块对应一个 W25Q64 扇区。
 */
static int lfs_erase(const struct lfs_config *c, lfs_block_t block)
{
    uint32_t addr = (uint32_t)block * c->block_size; // 擦除时只需要块起始地址，不需要块内偏移

    if (!W25Q64_SectorErase(addr))
    {
        return LFS_ERR_IO;
    }

    return LFS_ERR_OK;
}

/**
 * @brief 同步回调
 *
 * @param c littlefs 配置对象
 *
 * @return LFS_ERR_OK 成功
 *
 * @note
 *   W25Q64 的读、写、擦除操作在底层都已经阻塞等待 BUSY 结束，
 *   所以当底层函数返回时，操作已经完成，这里无需额外处理。
 */
static int lfs_sync(const struct lfs_config *c)
{
    (void)c;

    return LFS_ERR_OK;
}

/**
 * @brief  初始化 littlefs
 *
 * 尝试挂载 littlefs。
 * 如果挂载失败，则认为可能是首次上电或尚未格式化，
 * 于是执行格式化，然后重新挂载。
 *
 * @return 0 成功；其他值为 littlefs 错误码
 *
 * @note
 *   自动格式化适合首次使用或开发阶段。
 *   如果产品中存储了重要数据，建议区分“未格式化”和“IO 故障”，
 *   避免因为临时读写错误而误格式化导致数据丢失。
 */
int LittleFS_Init(void)
{
    /* 尝试第一次挂载 */
    int err = lfs_mount(&g_lfs, &g_lfs_config);

    if (err != LFS_ERR_OK)
    {
        /* 首次上电或之前不是 littlefs 格式 ，需要格式化 */
        printf("littlefs mount failed, err = %d, formatting\n", err);

        /* 格式化 */
        err = lfs_format(&g_lfs, &g_lfs_config);
        if (err != LFS_ERR_OK)
        {
            printf("littlefs format failed, err = %d\n", err);
            return err;
        }

        /* 重新挂载 */
        err = lfs_mount(&g_lfs, &g_lfs_config);
        if (err != LFS_ERR_OK)
        {
            printf("littlefs remount failed, err = %d\n", err);
            return err;
        }
    }

    /* 打印文件系统容量信息，便于确认配置是否正确 */
    printf("littlefs init success, block = %u x %u, total = %u KB\n",
           (unsigned)g_lfs_config.block_size,
           (unsigned)g_lfs_config.block_count,
           (unsigned)(g_lfs_config.block_size / 1024U * g_lfs_config.block_count));

    /* 检查 W25Q64 忙等待超时计数。如果非 0，说明底层驱动曾经等待 BUSY 超时，可能存在硬件或时序问题。 */
    if (W25Q64_GetBusyTimeoutCount() != 0U)
    {
        printf("WARN: W25Q64 busy timeout count = %u\n", (unsigned)W25Q64_GetBusyTimeoutCount());
    }

    return 0;
}

/**
 * @brief  强制格式化并重新挂载
 *
 * 无论当前是否已经挂载，都执行 lfs_format，然后重新 lfs_mount。
 *
 * @return 0 成功；其他值为 littlefs 错误码
 *
 * @note
 *   格式化会清空文件系统数据。
 *   如果 g_lfs 已经挂载，调用前应确保没有打开的文件，
 *   必要时先 lfs_unmount(&g_lfs)，再执行本函数。
 */
int LittleFS_Format(void)
{
    /* 强制格式化 */
    int err = lfs_format(&g_lfs, &g_lfs_config);
    if (err != LFS_ERR_OK)
    {
        printf("littlefs format failed, err = %d\n", err);
        return err;
    }

    /* 格式化后重新挂载 */
    err = lfs_mount(&g_lfs, &g_lfs_config);
    if (err != LFS_ERR_OK)
    {
        printf("littlefs mount after format failed, err = %d\n", err);
        return err;
    }

    return 0;
}
