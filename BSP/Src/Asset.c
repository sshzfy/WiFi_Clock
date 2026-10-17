#include "Asset.h"

/* ================================================
 * 资源层的实现: 一个 littlefs 挂载 + 一把访问锁 + 两类数据
 *   只读资源: 字库(/font/ *.bin) 与图片(/img/ *.bin)
 *   运行数据: /data/<name>.bin
 * 原则: 任何一次文件系统访问都持 LittleFS_Lock(); 参数与越界
 *       校验做在进入 littlefs 之前; 失败一律返回负错误码并打日志。
 * ================================================ */

static const Asset_FontFile_t s_fontFiles[ASSET_FID_COUNT] =
    {
        {"/font/cn16.bin", ASSET_CN16_SIZE},   // ASSET_FID_CN16
        {"/font/cn22.bin", ASSET_CN22_SIZE},   // ASSET_FID_CN22
        {"/font/as12.bin", ASSET_AS12_SIZE},   // ASSET_FID_AS12
        {"/font/as16.bin", ASSET_AS16_SIZE},   // ASSET_FID_AS16
        {"/font/as16b.bin", ASSET_AS16B_SIZE}, // ASSET_FID_AS16B
        {"/font/as22.bin", ASSET_AS22_SIZE},   // ASSET_FID_AS22
        {"/font/as22b.bin", ASSET_AS22B_SIZE}, // ASSET_FID_AS22B
        {"/font/as32.bin", ASSET_AS32_SIZE},   // ASSET_FID_AS32
        {"/font/as32b.bin", ASSET_AS32B_SIZE}, // ASSET_FID_AS32B
        {"/font/as48.bin", ASSET_AS48_SIZE},   // ASSET_FID_AS48
        {"/font/as48b.bin", ASSET_AS48B_SIZE}, // ASSET_FID_AS48B
};

static bool s_mounted = false; // littlefs 是否已挂载
static uint32_t s_missing = 0; // 自检发现的问题项数
static uint32_t s_checked = 0; // 自检核对过的项数

/* 常开句柄: 字模读取非常频繁, 每次 open/close 都要走一遍目录查找, 太浪费 */
static lfs_file_t s_file[ASSET_FID_COUNT]; // 字库文件句柄
static bool s_open[ASSET_FID_COUNT];       // 字库文件是否已打开
static uint32_t s_pos[ASSET_FID_COUNT];    // 记录当前文件位置, 位置相同就跳过 seek

static uint32_t s_imageMask = 0; // 图片自检结果位图(最多 32 张)

/* ================ static 函数 ================ */
static int Asset_MapErr(int err);
static int Asset_MkdirLocked(const char *path);
static void Asset_EnsureDirs(void);
static void Asset_CloseFonts(void);
static void Asset_OpenFonts(void);
static void Asset_SelfCheck(void);
static bool Asset_DataNameLegal(const char *name);
static int Asset_DataPathAssemble(const char *name, char *out, uint32_t out_size);

static int Asset_MapErr(int err)
{
    switch (err)
    {
    case LFS_ERR_OK:
        return ASSET_OK;
    case LFS_ERR_NOENT:
        return ASSET_ERR_NOENT;
    case LFS_ERR_NOSPC:
        return ASSET_ERR_SPACE;
    case LFS_ERR_INVAL:
    case LFS_ERR_NAMETOOLONG:
    case LFS_ERR_NOTDIR:
    case LFS_ERR_ISDIR:
        return ASSET_ERR_PARAM;
    default:
        return ASSET_ERR_IO;
    }
}

/* 创建目录(调用者必须已持有 LittleFS_Lock); 已存在视为成功 */
static int Asset_MkdirLocked(const char *path)
{
    struct lfs_info info;

    /* 检查目录是否存在 */
    int err = lfs_stat(&g_lfs, path, &info);
    /* 目录存在且是目录, 成功 */
    if (err == LFS_ERR_OK)
    {
        return (info.type == LFS_TYPE_DIR) ? ASSET_OK : ASSET_ERR_PARAM;
    }
    /* 其他错误码, 直接返回 */
    if (err != LFS_ERR_NOENT)
    {
        return Asset_MapErr(err);
    }

    /* 创建目录 */
    err = lfs_mkdir(&g_lfs, path);
    if ((err != LFS_ERR_OK) && (err != LFS_ERR_EXIST))
    {
        return Asset_MapErr(err);
    }

    return ASSET_OK;
}

/* 确保字库/图片/数据三个目录存在(格式化后目录会全丢, 需要能重建) */
static void Asset_EnsureDirs(void)
{
    LittleFS_Lock();

    (void)Asset_MkdirLocked("/font");
    (void)Asset_MkdirLocked("/img");
    (void)Asset_MkdirLocked("/data");

    LittleFS_Unlock();
}

/* 关闭全部常开字库句柄 */
static void Asset_CloseFonts(void)
{
    LittleFS_Lock();

    for (int i = 0; i < ASSET_FID_COUNT; i++)
    {
        if (s_open[i])
        {
            /* 句柄可能已经因为格式化而失效, 关闭失败不影响后续流程 */
            (void)lfs_file_close(&g_lfs, &s_file[i]);
            s_open[i] = false;
        }
        s_pos[i] = ASSET_POS_INVALID; // 重置位置
    }

    LittleFS_Unlock();
}

/* 打开全部字库文件并常开句柄 */
static void Asset_OpenFonts(void)
{
    LittleFS_Lock();

    /* 以只读模式打开全部字库文件 */
    for (int i = 0; i < ASSET_FID_COUNT; i++)
    {
        int err = lfs_file_open(&g_lfs, &s_file[i], s_fontFiles[i].path, LFS_O_RDONLY);
        if (err == LFS_ERR_OK)
        {
            s_open[i] = true;
        }
        else
        {
            s_open[i] = false;
            DBG_PRINTF("[ASSET] open %s failed, err = %d\r\n", s_fontFiles[i].path, err);
        }
        s_pos[i] = ASSET_POS_INVALID;
    }

    LittleFS_Unlock();
}

/**
 * @brief 逐项核对资源文件是否存在且大小正确
 * @note 字库按固定尺寸核对, 图片按 4 字节头 + 宽*高*2 核对
 */
static void Asset_SelfCheck(void)
{
    struct lfs_info info;

    LittleFS_Lock();

    /* 字库文件
     * 逐项核对字库文件是否存在且大小正确*/
    for (int i = 0; i < ASSET_FID_COUNT; i++)
    {
        int err = lfs_stat(&g_lfs, s_fontFiles[i].path, &info);
        s_checked++; // 记录已核对的字库文件数量

        /* 文件有错误或大小不正确 */
        if ((err != LFS_ERR_OK) || (info.size != s_fontFiles[i].size))
        {
            s_missing++; // 记录错误字库的数量
            /* 文件存在但大小不正确 */
            if (err == LFS_ERR_OK)
            {
                DBG_PRINTF("[ASSET] BADSIZE %s size=%lu want=%lu\r\n",
                       s_fontFiles[i].path, (unsigned long)info.size,
                       (unsigned long)s_fontFiles[i].size);
            }
            /* 文件不存在 */
            else
            {
                DBG_PRINTF("[ASSET] MISSING %s err=%d\r\n", s_fontFiles[i].path, err);
            }
        }
    }

    /* 图片文件
     * 逐项核对图片文件是否存在且大小正确*/
    for (uint32_t i = 0; i < g_AllImageCount; i++)
    {
        const Image_t *im = g_AllImages[i];
        uint32_t want = (uint32_t)ASSET_IMAGE_HEADER_SIZE +
                        (uint32_t)im->width * (uint32_t)im->height * 2U; // 4 字节头 + 宽*高*2,一张图片理论的正确大小
        int err = lfs_stat(&g_lfs, im->path, &info);
        s_checked++; // 记录已核对的图片文件数量

        /* 文件有错误或大小不正确 */
        if ((err != LFS_ERR_OK) || (info.size != want))
        {
            s_missing++; // 记录错误图片的数量
            /* 文件存在但大小不正确 */
            if (err == LFS_ERR_OK)
            {
                DBG_PRINTF("[ASSET] BADSIZE %s size=%lu want=%lu\r\n",
                       im->path, (unsigned long)info.size, (unsigned long)want);
            }
            /* 文件不存在 */
            else
                DBG_PRINTF("[ASSET] MISSING %s err=%d\r\n", im->path, err);
        }
        /* 掩码只有 32 位, 超出范围的图片不记掩码 */
        else if (i < ASSET_IMAGE_MASK_BITS)
        {
            s_imageMask |= (1UL << i);
        }
    }

    LittleFS_Unlock();
}

/** @brief 运行数据名是否合法: 只允许字母/数字/'_'/'-', 长度 1 ~ ASSET_DATA_NAME_MAX
 * @param name 数据名
 * @return true 合法, false 不合法
 */
static bool Asset_DataNameLegal(const char *name)
{
    if ((name == NULL) || (name[0] == '\0'))
        return false;

    /* 检查是否有非法字符 */
    for (uint32_t i = 0; name[i] != '\0'; i++)
    {
        char c = name[i];
        bool ok = ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) ||
                  ((c >= '0') && (c <= '9')) || (c == '_') || (c == '-');

        /* 名字里不允许出现 '/', '.', '\\' 等字符: 路径由本层拼, 调用者无法越出 /data */
        if ((!ok) || (i >= ASSET_DATA_NAME_MAX))
            return false;
    }

    return true;
}

/** @brief 把数据名拼成完整路径 /data/<name>.bin
 * @param name 数据名
 * @param out 输出缓冲区
 * @param out_size 输出缓冲区大小
 * @return ASSET_OK 成功; 其他负数为错误码(参数错误/缓冲区太小)
 */
static int Asset_DataPathAssemble(const char *name, char *out, uint32_t out_size)
{
    if (!Asset_DataNameLegal(name))
        return ASSET_ERR_PARAM;

    int n = snprintf(out, out_size, "/data/%s.bin", name);
    if ((n <= 0) || ((uint32_t)n >= out_size))
    {
        return ASSET_ERR_PARAM;
    }

    return ASSET_OK;
}

/* ================ public函数 ================ */

/* ================ 初始化与状态 ================ */

/**
 * @brief 初始化资源层: W25Q64 -> 挂载 -> 常开字库句柄 -> 自检 -> 建目录
 * @return ASSET_OK 一切就绪; ASSET_ERR_MISSING 文件系统可用但资源不全;
 *         其他负数为错误码(未检测到 Flash / 挂载失败)
 * @note  可重复调用。挂载失败时绝不自动格式化 —— 那会把已经烧录好的
 *        字库与图片全部清空, 只能提示重新烧录 Provision 固件。
 */
int Asset_Init(void)
{
    /* 重复调用时先放掉旧句柄, 避免 littlefs 句柄泄漏 */
    if (s_mounted)
    {
        Asset_CloseFonts();
    }

    /* 初始化状态 */
    s_mounted = false;
    s_missing = 0;
    s_checked = 0;
    s_imageMask = 0;
    for (int i = 0; i < ASSET_FID_COUNT; i++)
    {
        s_open[i] = false;
        s_pos[i] = ASSET_POS_INVALID;
    }

    /* 1. 硬件: 确认 SPI Flash 在位(读 JEDEC ID) */
    W25Q64_Init();
    uint32_t jedec = W25Q64_ReadJedecId();
    if (jedec != 0xEF4017U)
    {
        DBG_PRINTF("[ASSET] ERROR: W25Q64 not detected (jedec = 0x%06lX)\r\n", (unsigned long)jedec);
        DBG_PRINTF("[ASSET] fonts/images/data all unavailable\r\n");
        return ASSET_ERR_IO;
    }
    DBG_PRINTF("[ASSET] W25Q64 jedec = 0x%06lX\r\n", (unsigned long)jedec);

    /* 2. 文件系统: 只挂载(幂等), 失败不格式化 */
    int err = LittleFS_Mount();
    if (err != LFS_ERR_OK)
    {
        DBG_PRINTF("[ASSET] ERROR: littlefs mount failed, err = %d\r\n", err);
        DBG_PRINTF("[ASSET] flash the provision firmware (RESOURCE_PROVISION=1) first\r\n");
        return Asset_MapErr(err);
    }
    s_mounted = true; // 标记为已挂载
    DBG_PRINTF("[ASSET] littlefs mounted: %lu blocks x %lu B\r\n",
           (unsigned long)g_lfs_config.block_count, (unsigned long)g_lfs_config.block_size);

    /* 3. 目录: 字库与图片目录由烧录固件创建, 这里只是补齐缺失的 */
    Asset_EnsureDirs();

    /* 4. 常开字库句柄 + 资源自检 */
    Asset_OpenFonts();
    Asset_SelfCheck();
    DBG_PRINTF("[ASSET] selfcheck: checked=%lu, missing=%lu\r\n",
           (unsigned long)s_checked, (unsigned long)s_missing);
    /* 全部缺失是最常见的情形: 资源从未烧录进 W25Q64 */
    if ((s_missing > 0U) && (s_missing == s_checked))
    {
        DBG_PRINTF("[ASSET] HINT: no resources on flash, flash the provision firmware first\r\n");
    }

    return (s_missing == 0U) ? ASSET_OK : ASSET_ERR_MISSING;
}

/* 文件系统是否已挂载可用(资源缺失也算可用, 见 Asset_MissingCount) */
bool Asset_Ready(void)
{
    return s_mounted && LittleFS_IsMounted();
}

/* 自检发现的问题项数(文件缺失或大小不符) */
uint32_t Asset_MissingCount(void)
{
    return s_missing;
}

/* 自检核对过的资源项总数 */
uint32_t Asset_CheckedCount(void)
{
    return s_checked;
}

/**
 * @brief 查询文件系统空间
 * @param total_bytes 输出: 总容量, 可传 NULL
 * @param used_bytes  输出: 已占用(按块统计), 可传 NULL
 * @return ASSET_OK 成功; 负数为错误码
 * @note  lfs_fs_size() 要扫描整个文件系统的块状态, 耗时随容量增长,
 *        适合偶尔查询, 不要放在渲染循环里。
 */
int Asset_Space(uint32_t *total_bytes, uint32_t *used_bytes)
{
    if (!Asset_Ready())
    {
        return ASSET_ERR_STATE;
    }

    /* 查询已占用块数 */
    LittleFS_Lock();
    lfs_ssize_t used_blocks = lfs_fs_size(&g_lfs);
    LittleFS_Unlock();

    if (used_blocks < 0)
    {
        return ASSET_ERR_IO;
    }

    uint32_t block_size = (uint32_t)g_lfs_config.block_size; // 每个块的大小

    if (total_bytes != NULL)
    {
        *total_bytes = block_size * (uint32_t)g_lfs_config.block_count; // 总容量字节数
    }
    if (used_bytes != NULL)
    {
        *used_bytes = (uint32_t)used_blocks * block_size; // 已占用字节数
    }

    return ASSET_OK;
}

/**
 * @brief 从字库资源文件读取字体数据,只读
 * @param fid 资源文件 ID(见 Asset_FontId_t)
 * @param off 文件内字节偏移
 * @param buf 输出缓冲区
 * @param len 要读取的字节数
 * @return ASSET_OK 成功; 负数为错误码
 * @note  句柄常开且位置被记住, 连续读同一段时不需要重新 seek。
 */
int Asset_ReadFont(uint8_t fid, uint32_t off, void *buf, uint32_t len)
{
    if ((fid >= ASSET_FID_COUNT) || (buf == NULL) || (len == 0U))
    {
        return ASSET_ERR_PARAM;
    }
    if ((!Asset_Ready()) || (!s_open[fid]))
    {
        return ASSET_ERR_STATE;
    }
    /* 越界保护: 先比 off 再比 len, 避免 off + len 溢出 */
    if ((off > s_fontFiles[fid].size) || ((len + off) > (s_fontFiles[fid].size)))
    {
        return ASSET_ERR_RANGE;
    }

    LittleFS_Lock();

    /* 当位置不匹配时需要 seek */
    if (s_pos[fid] != off)
    {
        if (lfs_file_seek(&g_lfs, &s_file[fid], (lfs_off_t)off, LFS_SEEK_SET) < 0)
        {
            LittleFS_Unlock();
            return ASSET_ERR_IO;
        }
    }
    /* 读取数据 */
    if (lfs_file_read(&g_lfs, &s_file[fid], buf, len) != (lfs_ssize_t)len)
    {
        s_pos[fid] = ASSET_POS_INVALID; // 句柄位置已不可信, 下次强制 seek
        LittleFS_Unlock();
        return ASSET_ERR_IO;
    }
    s_pos[fid] = off + len; // 成功读取后,更新句柄位置

    LittleFS_Unlock();

    return ASSET_OK;
}

/**
 * @brief 该图片是否已通过开机自检(存在且大小与描述一致)
 * @param img 图片描述
 * @return true 已通过自检; false 未通过(或不在资源清单里)
 */
bool Asset_ImageOk(const Image_t *img)
{
    if (img == NULL)
    {
        return false;
    }

    for (uint32_t i = 0; i < g_AllImageCount; i++)
    {
        if (g_AllImages[i] == img)
        {
            /* 掩码只有 32 位: 超出范围的图片无法用掩码表示, 一律当作不可用 */
            if (i >= ASSET_IMAGE_MASK_BITS)
            {
                return false;
            }
            return ((s_imageMask >> i) & 1U) != 0U;
        }
    }

    return false;
}

/**
 * @brief 只读图片文件头, 取出宽高
 * @param path 图片路径(绝对路径, 如 "/img/wifi.bin")
 * @param w,h  输出: 宽高
 * @return ASSET_OK 成功; ASSET_ERR_FORMAT 文件头非法; 其他负数为错误码
 */
int Asset_ImageInfo(const char *path, uint16_t *w, uint16_t *h)
{
    if ((path == NULL) || (path[0] != '/') || (w == NULL) || (h == NULL))
    {
        return ASSET_ERR_PARAM;
    }
    if (!Asset_Ready())
    {
        return ASSET_ERR_STATE;
    }

    uint8_t header[ASSET_IMAGE_HEADER_SIZE]; // 图片文件头

    LittleFS_Lock();

    lfs_file_t file;
    int err = lfs_file_open(&g_lfs, &file, path, LFS_O_RDONLY);
    if (err != LFS_ERR_OK)
    {
        LittleFS_Unlock();
        if (err != LFS_ERR_NOENT)
        {
            DBG_PRINTF("[ASSET] image info %s: open failed, err = %d\r\n", path, err);
        }
        return Asset_MapErr(err);
    }
    /* 从临时打开的图片文件读取文件头 */
    lfs_ssize_t rd = lfs_file_read(&g_lfs, &file, header, sizeof(header)); // rd 为实际读取的字节数
    (void)lfs_file_close(&g_lfs, &file);

    LittleFS_Unlock();

    if (rd != (lfs_ssize_t)sizeof(header))
    {
        DBG_PRINTF("[ASSET] image info %s: header incomplete (%d bytes)\r\n", path, (int)rd);
        return ASSET_ERR_FORMAT;
    }

    uint16_t width = (uint16_t)(((uint16_t)header[0] << 8) | (uint16_t)header[1]);
    uint16_t height = (uint16_t)(((uint16_t)header[2] << 8) | (uint16_t)header[3]);

    if ((width == 0U) || (height == 0U))
    {
        DBG_PRINTF("[ASSET] image info %s: bad size %ux%u\r\n", path, (unsigned)width, (unsigned)height);
        return ASSET_ERR_FORMAT;
    }

    *w = width;
    *h = height;

    return ASSET_OK;
}

/**
 * @brief 打开图片文件并跳过 4 字节文件头
 * @param img 图片描述(宽高在编译期已知, 头只用来占位跳过)
 * @param f   输出: 文件句柄, 由调用方持有到 Asset_ImageClose()
 * @return ASSET_OK 成功; 负数为错误码
 * @note  每次读都持 LittleFS_Lock(), 不会与别的任务写 Flash 抢 SPI1。
 */
int Asset_ImageOpen(const Image_t *img, lfs_file_t *f)
{
    if ((img == NULL) || (f == NULL))
    {
        return ASSET_ERR_PARAM;
    }
    if (!Asset_Ready())
    {
        return ASSET_ERR_STATE;
    }

    LittleFS_Lock();

    int err = lfs_file_open(&g_lfs, f, img->path, LFS_O_RDONLY);
    if (err != LFS_ERR_OK)
    {
        LittleFS_Unlock();
        DBG_PRINTF("[ASSET] open %s failed, err = %d\r\n", img->path, err);
        return Asset_MapErr(err);
    }
    /* 跳过 4 字节文件头(width/height 编译期已知, 不必读出来) */
    if (lfs_file_seek(&g_lfs, f, (lfs_off_t)ASSET_IMAGE_HEADER_SIZE, LFS_SEEK_SET) < 0)
    {
        (void)lfs_file_close(&g_lfs, f);
        LittleFS_Unlock();
        return ASSET_ERR_IO;
    }

    LittleFS_Unlock();

    return ASSET_OK;
}

/**
 * @brief 从已打开的图片文件顺序读取 len 字节
 * @param f   图片文件句柄
 * @param buf 输出缓冲区
 * @param len 读取的字节数
 * @return ASSET_OK 成功; 负数为错误码
 */
int Asset_ImageRead(lfs_file_t *f, void *buf, uint32_t len)
{
    if ((f == NULL) || (buf == NULL) || (len == 0U))
    {
        return ASSET_ERR_PARAM;
    }
    if (!Asset_Ready())
    {
        return ASSET_ERR_STATE;
    }

    LittleFS_Lock();

    if (lfs_file_read(&g_lfs, f, buf, len) != (lfs_ssize_t)len)
    {
        LittleFS_Unlock();
        return ASSET_ERR_IO;
    }

    LittleFS_Unlock();

    return ASSET_OK;
}

/* 关闭图片文件句柄 */
void Asset_ImageClose(lfs_file_t *f)
{
    if (s_mounted && (f != NULL))
    {
        LittleFS_Lock();
        (void)lfs_file_close(&g_lfs, f);
        LittleFS_Unlock();
    }
}

/* ================ 运行数据: /data/<name>.bin ================ */

/**
 * @brief 保存运行数据(覆盖写, 不存在则创建)
 * @param name 数据名(字母/数字/'_'/'-', 最长 ASSET_DATA_NAME_MAX)
 * @param buf  数据
 * @param len  字节数(必须 > 0)
 * @return ASSET_OK 全部字节已落盘; 负数为错误码
 * @note  littlefs 写入是写时复制: 中途掉电保留旧内容, 不会写出半个文件。
 */
int Asset_SaveData(const char *name, const void *buf, uint32_t len)
{
    char path[ASSET_DATA_PATH_MAX]; // 数据文件路径

    if (!Asset_Ready())
    {
        return ASSET_ERR_STATE;
    }
    if ((buf == NULL) || (len == 0U))
    {
        return ASSET_ERR_PARAM;
    }

    int ret = Asset_DataPathAssemble(name, path, sizeof(path));
    if (ret != ASSET_OK)
    {
        return ret;
    }

    LittleFS_Lock();

    lfs_file_t file;
    int err = lfs_file_open(&g_lfs, &file, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC); // 打开文件, 并创建(不存在)或截断(存在)
    if (err == LFS_ERR_OK)
    {
        lfs_ssize_t written = lfs_file_write(&g_lfs, &file, buf, len); // 写入数据,并返回写入的字节数
        int close_err = lfs_file_close(&g_lfs, &file);                 // 空间不足常在 close(sync) 时才暴露

        /* 检查写入是否成功 */
        if (written != (lfs_ssize_t)len)
        {
            err = (written < 0) ? (int)written : LFS_ERR_IO;
        }
        else if (close_err != LFS_ERR_OK)
        {
            err = close_err;
        }
    }

    LittleFS_Unlock();

    if (err != LFS_ERR_OK)
    {
        DBG_PRINTF("[ASSET] save %s failed, err = %d\r\n", path, err);
        return Asset_MapErr(err);
    }

    return ASSET_OK;
}

/**
 * @brief 读取运行数据(整块)
 * @param name     数据名
 * @param buf      输出缓冲区
 * @param buf_size 缓冲区大小
 * @return 实际读取字节数; 负数为错误码(放不下时为 ASSET_ERR_SPACE)
 */
int Asset_LoadData(const char *name, void *buf, uint32_t buf_size)
{
    char path[ASSET_DATA_PATH_MAX]; // 数据文件路径

    if (!Asset_Ready())
    {
        return ASSET_ERR_STATE;
    }
    if (buf == NULL)
    {
        return ASSET_ERR_PARAM;
    }

    int ret = Asset_DataPathAssemble(name, path, sizeof(path));
    if (ret != ASSET_OK)
    {
        return ret;
    }

    LittleFS_Lock();

    lfs_file_t file;
    int err = lfs_file_open(&g_lfs, &file, path, LFS_O_RDONLY); // 一只读方式打开文件
    if (err != LFS_ERR_OK)
    {
        LittleFS_Unlock();
        return Asset_MapErr(err);
    }

    lfs_soff_t size = lfs_file_size(&g_lfs, &file); // 获取文件大小
    if (size < 0)
    {
        (void)lfs_file_close(&g_lfs, &file);
        LittleFS_Unlock();
        return ASSET_ERR_IO;
    }

    /* 一次性读完: 缓冲区必须放得下整块数据, 不做部分读取 */
    if ((uint32_t)size > buf_size)
    {
        (void)lfs_file_close(&g_lfs, &file);
        LittleFS_Unlock();
        DBG_PRINTF("[ASSET] load %s: need %lu bytes, buffer = %lu\r\n",
               path, (unsigned long)size, (unsigned long)buf_size);
        return ASSET_ERR_SPACE;
    }

    lfs_ssize_t rd = (size > 0) ? lfs_file_read(&g_lfs, &file, buf, (lfs_size_t)size) : 0; // 读取文件内容，返回读取的字节数
    (void)lfs_file_close(&g_lfs, &file);

    LittleFS_Unlock();

    if (rd != (lfs_ssize_t)size)
    {
        DBG_PRINTF("[ASSET] load %s: %d of %ld bytes\r\n", path, (int)rd, (long)size);
        return (rd < 0) ? Asset_MapErr((int)rd) : ASSET_ERR_IO;
    }

    return (int)size;
}

/**
 * @brief 查询运行数据的大小
 * @return ASSET_OK 成功; ASSET_ERR_NOENT 还没存过; 其他负数为错误码
 */
int Asset_DataSize(const char *name, uint32_t *size)
{
    char path[ASSET_DATA_PATH_MAX];
    struct lfs_info info;

    if (size == NULL)
    {
        return ASSET_ERR_PARAM;
    }
    if (!Asset_Ready())
    {
        return ASSET_ERR_STATE;
    }

    int ret = Asset_DataPathAssemble(name, path, sizeof(path));
    if (ret != ASSET_OK)
    {
        return ret;
    }

    LittleFS_Lock();
    int err = lfs_stat(&g_lfs, path, &info); // 获取文件信息
    LittleFS_Unlock();

    if (err != LFS_ERR_OK)
    {
        return Asset_MapErr(err);
    }

    *size = (uint32_t)info.size;

    return ASSET_OK;
}

/* 该运行数据是否存在 */
bool Asset_HasData(const char *name)
{
    char path[ASSET_DATA_PATH_MAX];
    struct lfs_info info;

    if ((!Asset_Ready()) || (Asset_DataPathAssemble(name, path, sizeof(path)) != ASSET_OK))
    {
        return false;
    }

    LittleFS_Lock();
    int err = lfs_stat(&g_lfs, path, &info);
    LittleFS_Unlock();

    return (err == LFS_ERR_OK);
}

/**
 * @brief 删除运行数据
 * @return ASSET_OK 成功; ASSET_ERR_NOENT 本来就不存在; 其他负数为错误码
 */
int Asset_DeleteData(const char *name)
{
    char path[ASSET_DATA_PATH_MAX];

    if (!Asset_Ready())
    {
        return ASSET_ERR_STATE;
    }

    int ret = Asset_DataPathAssemble(name, path, sizeof(path));
    if (ret != ASSET_OK)
    {
        return ret;
    }

    LittleFS_Lock();
    int err = lfs_remove(&g_lfs, path);
    LittleFS_Unlock();

    if (err != LFS_ERR_OK)
    {
        if (err != LFS_ERR_NOENT)
            DBG_PRINTF("[ASSET] delete %s failed, err = %d\r\n", path, err);
        return Asset_MapErr(err);
    }

    return ASSET_OK;
}
