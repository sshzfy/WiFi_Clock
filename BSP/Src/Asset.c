#include "Asset.h"
#include "lfs_port.h"
#include "W25Q64.h"
#include <stdio.h>

/* ============================================================
 * 字库资源文件表
 * 顺序必须与 Font.h 里的 Asset_FontId_t 枚举严格一致。
 * ============================================================ */
typedef struct
{
    const char *path; // W25Q64 上的 littlefs 路径
    uint32_t size;    // 文件应有的字节数, 自检用
} Asset_FontFile_t;

static const Asset_FontFile_t s_fontFiles[ASSET_FID_COUNT] = {
    {"/font/cn16.bin", ASSET_CN16_SIZE},   /* ASSET_FID_CN16 */
    {"/font/cn22.bin", ASSET_CN22_SIZE},   /* ASSET_FID_CN22 */
    {"/font/as12.bin", ASSET_AS12_SIZE},   /* ASSET_FID_AS12 */
    {"/font/as16.bin", ASSET_AS16_SIZE},   /* ASSET_FID_AS16 */
    {"/font/as16b.bin", ASSET_AS16B_SIZE}, /* ASSET_FID_AS16B */
    {"/font/as22.bin", ASSET_AS22_SIZE},   /* ASSET_FID_AS22 */
    {"/font/as22b.bin", ASSET_AS22B_SIZE}, /* ASSET_FID_AS22B */
    {"/font/as32.bin", ASSET_AS32_SIZE},   /* ASSET_FID_AS32 */
    {"/font/as32b.bin", ASSET_AS32B_SIZE}, /* ASSET_FID_AS32B */
    {"/font/as48.bin", ASSET_AS48_SIZE},   /* ASSET_FID_AS48 */
    {"/font/as48b.bin", ASSET_AS48B_SIZE}, /* ASSET_FID_AS48B */
};

static bool s_ready = false;      // littlefs 是否已挂载
static uint32_t s_missing = 0;    // 自检发现的问题项数
static uint32_t s_checked = 0;    // 自检核对过的项数

/* 常开句柄: 字模读取非常频繁, 每次 open/close 都要走一遍目录查找, 太浪费 */
static lfs_file_t s_file[ASSET_FID_COUNT];
static bool s_open[ASSET_FID_COUNT];
static uint32_t s_pos[ASSET_FID_COUNT]; // 记录当前文件位置, 位置相同就跳过 seek

/* 图片自检结果位图, 最多 32 张 */
static uint32_t s_imageMask = 0;

/* ==================== 自检 ==================== */

/**
 * @brief 逐项核对资源文件是否存在且大小正确
 * @note 字库按固定尺寸核对, 图片按 4 字节头 + 宽*高*2 核对
 */
static void Asset_SelfCheck(void)
{
    struct lfs_info info;

    /* 字库文件 */
    for (int i = 0; i < ASSET_FID_COUNT; i++)
    {
        int err = lfs_stat(&g_lfs, s_fontFiles[i].path, &info);
        s_checked++;

        if ((err != LFS_ERR_OK) || (info.size != s_fontFiles[i].size))
        {
            s_missing++;
            if (err == LFS_ERR_OK)
            {
                printf("[ASSET] BADSIZE %s size=%lu want=%lu\r\n",
                       s_fontFiles[i].path, (unsigned long)info.size,
                       (unsigned long)s_fontFiles[i].size);
            }
            else
            {
                printf("[ASSET] MISSING %s err=%d\r\n", s_fontFiles[i].path, err);
            }
        }
    }

    /* 图片文件 */
    for (uint32_t i = 0; i < g_AllImageCount; i++)
    {
        const Image_t *im = g_AllImages[i];
        uint32_t want = (uint32_t)ASSET_IMAGE_HEADER_SIZE +
                        (uint32_t)im->width * (uint32_t)im->height * 2U;
        int err = lfs_stat(&g_lfs, im->path, &info);
        s_checked++;

        if ((err != LFS_ERR_OK) || (info.size != want))
        {
            s_missing++;
            if (err == LFS_ERR_OK)
            {
                printf("[ASSET] BADSIZE %s size=%lu want=%lu\r\n",
                       im->path, (unsigned long)info.size, (unsigned long)want);
            }
            else
            {
                printf("[ASSET] MISSING %s err=%d\r\n", im->path, err);
            }
        }
        else
        {
            s_imageMask |= (1UL << i);
        }
    }
}

/* ==================== 对外接口 ==================== */

int Asset_Init(void)
{
    s_ready = false;
    s_missing = 0;
    s_checked = 0;
    s_imageMask = 0;

    for (int i = 0; i < ASSET_FID_COUNT; i++)
    {
        s_open[i] = false;
        s_pos[i] = 0;
    }

    W25Q64_Init();

    uint32_t jedec = W25Q64_ReadJedecId();
    if (jedec != 0xEF4017U)
    {
        printf("[ASSET] ERROR: W25Q64 not detected (jedec = 0x%06lX)\r\n",
               (unsigned long)jedec);
        printf("[ASSET] all font/image resources unavailable\r\n");
        return -1;
    }
    printf("[ASSET] W25Q64 jedec = 0x%06lX\r\n", (unsigned long)jedec);

    /* 只挂载, 绝不自动格式化:
     * 正式固件里格式化会把字库和图片全部清空。挂载失败只能提示重新烧录。 */
    int err = lfs_mount(&g_lfs, &g_lfs_config);
    if (err != LFS_ERR_OK)
    {
        printf("[ASSET] ERROR: littlefs mount failed, err = %d\r\n", err);
        printf("[ASSET] flash the provision firmware (RESOURCE_PROVISION=1) first\r\n");
        return -2;
    }

    s_ready = true;
    printf("[ASSET] littlefs mounted: %lu blocks x %lu B\r\n",
           (unsigned long)g_lfs_config.block_count, (unsigned long)g_lfs_config.block_size);

    /* 常开全部字库句柄 */
    for (int i = 0; i < ASSET_FID_COUNT; i++)
    {
        err = lfs_file_open(&g_lfs, &s_file[i], s_fontFiles[i].path, LFS_O_RDONLY);
        if (err == LFS_ERR_OK)
        {
            s_open[i] = true;
        }
        else
        {
            printf("[ASSET] open %s failed, err = %d\r\n", s_fontFiles[i].path, err);
        }
    }

    Asset_SelfCheck();

    printf("[ASSET] selfcheck: checked=%lu, missing=%lu\r\n",
           (unsigned long)s_checked, (unsigned long)s_missing);

    /* 全部缺失是最常见的情形: 资源从未烧录进 W25Q64。
     * 注意 littlefs 挂载成功不代表资源存在 —— 空的旧文件系统同样能挂载。 */
    if ((s_missing > 0U) && (s_missing == s_checked))
    {
        printf("[ASSET] HINT: no resources on flash, flash the provision firmware first\r\n");
    }

    return (s_missing == 0U) ? 0 : -3;
}

bool Asset_Ready(void)
{
    return s_ready;
}

uint32_t Asset_MissingCount(void)
{
    return s_missing;
}

uint32_t Asset_CheckedCount(void)
{
    return s_checked;
}

int Asset_ReadFont(uint8_t fid, uint32_t off, void *buf, uint32_t len)
{
    if ((!s_ready) || (fid >= ASSET_FID_COUNT) || (!s_open[fid]) ||
        (buf == NULL) || (len == 0U))
    {
        return -1;
    }

    /* 越界保护: 先比 off 再比 len, 避免 off + len 溢出 */
    if ((off > s_fontFiles[fid].size) || (len > (s_fontFiles[fid].size - off)))
    {
        return -2;
    }

    /* 位置相同就不必再 seek, 连续读同一段时能省一次 CTZ 定位 */
    if (s_pos[fid] != off)
    {
        if (lfs_file_seek(&g_lfs, &s_file[fid], (lfs_off_t)off, LFS_SEEK_SET) < 0)
        {
            return -3;
        }
    }

    if (lfs_file_read(&g_lfs, &s_file[fid], buf, len) != (lfs_ssize_t)len)
    {
        return -4;
    }

    s_pos[fid] = off + len;
    return 0;
}

int Asset_ImageOpen(const Image_t *img, lfs_file_t *f)
{
    if ((!s_ready) || (img == NULL) || (f == NULL))
    {
        return -1;
    }

    int err = lfs_file_open(&g_lfs, f, img->path, LFS_O_RDONLY);
    if (err != LFS_ERR_OK)
    {
        printf("[ASSET] open %s failed, err = %d\r\n", img->path, err);
        return err;
    }

    /* 跳过 4 字节文件头(width/height 编译期已知, 不必读出来) */
    if (lfs_file_seek(&g_lfs, f, (lfs_off_t)ASSET_IMAGE_HEADER_SIZE, LFS_SEEK_SET) < 0)
    {
        lfs_file_close(&g_lfs, f);
        return -2;
    }

    return 0;
}

int Asset_ImageRead(lfs_file_t *f, void *buf, uint32_t len)
{
    if ((!s_ready) || (f == NULL) || (buf == NULL))
    {
        return -1;
    }

    if (lfs_file_read(&g_lfs, f, buf, len) != (lfs_ssize_t)len)
    {
        return -2;
    }

    return 0;
}

void Asset_ImageClose(lfs_file_t *f)
{
    if (s_ready && (f != NULL))
    {
        lfs_file_close(&g_lfs, f);
    }
}

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
            return ((s_imageMask >> i) & 1U) != 0U;
        }
    }

    return false;
}
