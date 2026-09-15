/*
 * 资源烧录固件实现
 *
 * 本文件只在 RESOURCE_PROVISION == 1 时参与编译, 用于把备份在源码里的
 * 字库与图片写入 W25Q64 的 littlefs 文件系统。正式固件不含任何资源数据。
 *
 * 资源总量约 822KB 超过 512KB Flash, 必须按 PROVISION_BATCH 分三批烧录:
 *   1 = 汉字库 16 + 22        (367990 字节)
 *   2 = 开机图 + 主页图       (307200 字节)
 *   3 = 9 张 ASCII 表 + 图标  (183460 字节)
 */
#include "Provision.h"

#if (RESOURCE_PROVISION == 1)

#include "Board.h"
#include "Usart.h"
#include "Timer.h"
#include "W25Q64.h"
#include "LFS_Operation.h"
#include "Font.h"
#include "Image.h"
#include <stdio.h>

/* 字库资源项: 裸数据直接写入 */
typedef struct
{
    const char *path;
    const unsigned char *data;
    uint32_t size;
} Provision_Font_t;

/* 图片资源项: Image_t 提供路径与宽高, data 提供原始像素数据 */
typedef struct
{
    const Image_t *img;
    const unsigned char *data;
} Provision_Image_t;

/* 每个批次只定义自己真正需要的表, 避免出现无人引用的占位数组 */
#if (PROVISION_BATCH == 1)

static const Provision_Font_t s_fonts[] = {
    {"/font/cn16.bin", Chinese_Font16_Data, ASSET_CN16_SIZE},
    {"/font/cn22.bin", Chinese_Font22_Data, ASSET_CN22_SIZE},
};

#elif (PROVISION_BATCH == 2)

static const Provision_Image_t s_images[] = {
    {&Boot_Page_Waitconnect, gImage_Boot_Page_Waitconnect},
    {&Image_Main_Page, gImage_Main_Page},
};

#elif (PROVISION_BATCH == 3)

static const Provision_Font_t s_fonts[] = {
    {"/font/as12.bin", Font_12_Table, ASSET_AS12_SIZE},
    {"/font/as16.bin", Font_16_Table, ASSET_AS16_SIZE},
    {"/font/as16b.bin", Font_16B_Table, ASSET_AS16B_SIZE},
    {"/font/as22.bin", Font_22_Table, ASSET_AS22_SIZE},
    {"/font/as22b.bin", Font_22B_Table, ASSET_AS22B_SIZE},
    {"/font/as32.bin", Font_32_Table, ASSET_AS32_SIZE},
    {"/font/as32b.bin", Font_32B_Table, ASSET_AS32B_SIZE},
    {"/font/as48.bin", Font_48_Table, ASSET_AS48_SIZE},
    {"/font/as48b.bin", Font_48B_Table, ASSET_AS48B_SIZE},
};

static const Provision_Image_t s_images[] = {
    {&Image_err, gImage_err},
    {&Image_wifi, gImage_wifi},
    {&Image_wifi_off, gImage_wifi_off},
    {&Image_location, gImage_location},
    {&Image_no_location, gImage_no_location},
    {&Image_temperature, gImage_temperature},
    {&Image_humidity, gImage_humidity},
    {&Image_thermometer, gImage_thermometer},
    {&Image_sunny, gImage_sunny},
    {&Image_star, gImage_star},
    {&Image_overcast, gImage_overcast},
    {&Image_cloudy, gImage_cloudy},
    {&Image_light_rain, gImage_light_rain},
    {&Image_moderate_rain, gImage_moderate_rain},
    {&Image_heavy_rain, gImage_heavy_rain},
    {&Image_storm, gImage_storm},
    {&Image_heavy_storm, gImage_heavy_storm},
    {&Image_severe_storm, gImage_severe_storm},
    {&Image_ice_rain, gImage_ice_rain},
    {&Image_shower, gImage_shower},
    {&Image_thundershower, gImage_thundershower},
    {&Image_thundershower_with_hail, gImage_thundershower_with_hail},
    {&Image_sleet, gImage_sleet},
    {&Image_snow_flurry, gImage_snow_flurry},
    {&Image_light_snow, gImage_light_snow},
    {&Image_moderate_snow, gImage_moderate_snow},
    {&Image_heavy_snow, gImage_heavy_snow},
    {&Image_snowstorm, gImage_snowstorm},
    {&Image_foggy, gImage_foggy},
    {&Image_haze, gImage_haze},
};

#else
#error "PROVISION_BATCH must be 1, 2 or 3"
#endif

/* 本批是否包含字库/图片。用独立的布尔宏而不是直接判断 count,
 * 因为 #if 无法对 sizeof 求值。 */
#if (PROVISION_BATCH == 1) || (PROVISION_BATCH == 3)
#define PROV_HAS_FONTS  1
#define s_fontCount     ((uint32_t)(sizeof(s_fonts) / sizeof(s_fonts[0])))
#else
#define PROV_HAS_FONTS  0
#define s_fontCount     (0U)
#endif

#if (PROVISION_BATCH == 2) || (PROVISION_BATCH == 3)
#define PROV_HAS_IMAGES 1
#define s_imageCount    ((uint32_t)(sizeof(s_images) / sizeof(s_images[0])))
#else
#define PROV_HAS_IMAGES 0
#define s_imageCount    (0U)
#endif

#if (PROV_HAS_IMAGES)
/**
 * @brief 写一个图片文件: 4 字节大端头(宽/高) + 原始 RGB565 像素
 * @note 与 LFS_Operation 的 SaveImage 保持同一格式, Asset 自检才能对上大小
 */
static int Provision_SaveImage(const char *path, const unsigned char *data, uint16_t w, uint16_t h)
{
    lfs_file_t f;
    uint8_t header[4];
    uint32_t px_bytes = (uint32_t)w * (uint32_t)h * 2U;
    int err;

    header[0] = (uint8_t)(w >> 8);
    header[1] = (uint8_t)(w & 0xFFU);
    header[2] = (uint8_t)(h >> 8);
    header[3] = (uint8_t)(h & 0xFFU);

    err = lfs_file_open(&g_lfs, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err != LFS_ERR_OK)
        return err;

    if (lfs_file_write(&g_lfs, &f, header, sizeof(header)) != (lfs_ssize_t)sizeof(header))
    {
        lfs_file_close(&g_lfs, &f);
        return LFS_ERR_IO;
    }

    if (lfs_file_write(&g_lfs, &f, data, px_bytes) != (lfs_ssize_t)px_bytes)
    {
        lfs_file_close(&g_lfs, &f);
        return LFS_ERR_IO;
    }

    return lfs_file_close(&g_lfs, &f);
}
#endif /* PROV_HAS_IMAGES */

void Provision_Run(void)
{
    uint32_t wrote = 0;
    uint32_t pass = 0;
    uint32_t fail = 0;

    printf("\r\n[PROV] ==== resource provision ====\r\n");
    printf("[PROV] batch = %d, fonts = %lu, images = %lu\r\n",
           PROVISION_BATCH, (unsigned long)s_fontCount, (unsigned long)s_imageCount);

    W25Q64_Init();

    uint32_t jedec = W25Q64_ReadJedecId();
    printf("[PROV] W25Q64 jedec = 0x%06lX\r\n", (unsigned long)jedec);
    if (jedec != 0xEF4017U)
    {
        printf("[PROV] ERROR: W25Q64 not detected, stopped\r\n");
        for (;;)
            delay_ms(1000);
    }

    int err;

#if (PROVISION_BATCH == 1)
    /* 批次 1 是起始批次: 直接重建文件系统, 清掉 bare_test 等遗留的旧文件,
     * 保证资源集合干净可预测。注意: 重跑批次 1 会清空已写入的批次 2、3。 */
    printf("[PROV] batch 1: format filesystem first\r\n");
    err = LittleFS_Format();
#else
    /* 批次 2、3 追加到已有文件系统; 只有挂载失败时才自动格式化。 */
    err = LittleFS_Init();
#endif
    if (err != 0)
    {
        printf("[PROV] ERROR: littlefs init failed, err = %d, stopped\r\n", err);
        for (;;)
            delay_ms(1000);
    }

    (void)lfs_mkdir(&g_lfs, "/font");
    (void)lfs_mkdir(&g_lfs, "/img");

#if (PROV_HAS_FONTS)
    /* 字库 */
    for (uint32_t i = 0; i < s_fontCount; i++)
    {
        const Provision_Font_t *it = &s_fonts[i];

        err = SaveFont(it->path, it->data, it->size);
        printf("[PROV] %-24s %8lu  %s\r\n", it->path, (unsigned long)it->size,
               (err == 0) ? "OK" : "FAIL");
        if (err == 0)
        {
            wrote += it->size;
            pass++;
        }
        else
        {
            fail++;
        }
    }
#endif

#if (PROV_HAS_IMAGES)
    /* 图片 */
    for (uint32_t i = 0; i < s_imageCount; i++)
    {
        const Provision_Image_t *it = &s_images[i];
        uint32_t sz = 4U + (uint32_t)it->img->width * (uint32_t)it->img->height * 2U;

        err = Provision_SaveImage(it->img->path, it->data, it->img->width, it->img->height);
        printf("[PROV] %-24s %8lu  %s\r\n", it->img->path, (unsigned long)sz,
               (err == 0) ? "OK" : "FAIL");
        if (err == 0)
        {
            wrote += sz;
            pass++;
        }
        else
        {
            fail++;
        }
    }
#endif

    printf("[PROV] batch %d done: PASS %lu, FAIL %lu, wrote %lu bytes\r\n",
           PROVISION_BATCH, (unsigned long)pass, (unsigned long)fail, (unsigned long)wrote);
    printf("[PROV] fs used blocks = %ld / %lu\r\n",
           (long)lfs_fs_size(&g_lfs), (unsigned long)g_lfs_config.block_count);
    printf("[PROV] next: change PROVISION_BATCH, or set RESOURCE_PROVISION back to 0\r\n");

    for (;;)
    {
        delay_ms(3000);
        printf("[PROV] batch %d alive: PASS %lu FAIL %lu\r\n",
               PROVISION_BATCH, (unsigned long)pass, (unsigned long)fail);
    }
}

#endif /* RESOURCE_PROVISION == 1 */
