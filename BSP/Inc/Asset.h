/* ============================================================
 * 资源层: 外部 Flash(W25Q64 + littlefs)的唯一读写入口

 * 两类数据, 两种用法:
 *   1.只读资源 —— 字库 /font/ *.bin 与图片 /img/ *.bin
 *     由烧录固件(Provision)一次性写入, 运行期只读:
 *        字模: 按资源 ID + 偏移 O(1) 读取, 句柄常开, 不必反复 open
 *        图片: 打开 -> 分块读 -> 关闭, 直接喂给 LCD 流式渲染
 *     本层**不提供运行期写资源的接口**: 资源写坏屏上就是花屏或白屏,
 *     要换字库/图片请重新烧录 Provision 固件。
 *
 *   2.运行数据 —— /data/<name>.bin
 *     保存配置、缓存、计数等小块数据, 键值式用法:
 *        Asset_SaveData("wifi", buf, len);            // 存
 *        Asset_LoadData("wifi", buf, sizeof(buf));    // 取, 返回实际字节数
 *     name 只允许字母/数字/'_'/'-', 路径由本层拼成 /data/<name>.bin,
 *     因此不可能误写到字库或图片上; littlefs 写入是写时复制, 覆盖写
 *     是原子的 —— 中途掉电保留旧值, 不会写出半个文件。
 * ============================================================ */
#ifndef __ASSET_H__
#define __ASSET_H__

#include "main.h"
#include <stdio.h>
#include <stdbool.h>
#include "Font.h"
#include "Image.h"
#include "lfs.h"
#include "lfs_port.h"
#include "W25Q64.h"

#define ASSET_IMAGE_HEADER_SIZE        4U                                   // 图片文件头: width(2, 大端) + height(2, 大端), 与 LFS_Operation 的格式一致
#define ASSET_DATA_NAME_MAX            32U                                  // 运行数据文件名长度上限(不含结尾 '\0')
#define ASSET_DATA_PATH_MAX            (7U + ASSET_DATA_NAME_MAX + 4U + 1U) // 运行数据路径缓冲: "/data/" + name(<=32) + ".bin" + '\0'
#define ASSET_POS_INVALID              0xFFFFFFFFU                          // 句柄位置失效标记
#define ASSET_IMAGE_MASK_BITS          32U                                  // 图片自检结果掩码的位数(一张图一位)

/* 字库资源文件表: 顺序必须与 Font.h 里的 Asset_FontId_t 枚举严格一致 */
typedef struct
{
    const char *path; // W25Q64 上的 littlefs 路径
    uint32_t size;    // 文件应有的字节数, 自检用
} Asset_FontFile_t;

/* 统一错误码: 0 成功, 负数为错误码 */
typedef enum
{
    ASSET_OK = 0,          // 成功
    ASSET_ERR_PARAM = -1,  // 参数非法: 空指针 / 名字不合法 / 长度为 0
    ASSET_ERR_STATE = -2,  // 资源层不可用: 未初始化或挂载失败
    ASSET_ERR_RANGE = -3,  // 偏移或长度越界
    ASSET_ERR_NOENT = -4,  // 目标不存在
    ASSET_ERR_SPACE = -5,  // 空间不足或目标缓冲区太小
    ASSET_ERR_IO = -6,     // 底层读写失败(含未检测到 W25Q64)
    ASSET_ERR_FORMAT = -7, // 内容格式不符(例如图片头非法)
    ASSET_ERR_MISSING = -8 // 挂载成功但资源自检有缺失项: 读资源会失败, 运行数据仍可用
} Asset_Err_t;

int Asset_Init(void);
bool Asset_Ready(void);
uint32_t Asset_MissingCount(void);
uint32_t Asset_CheckedCount(void);
int Asset_Space(uint32_t *total_bytes, uint32_t *used_bytes);

int Asset_ReadFont(uint8_t fid, uint32_t off, void *buf, uint32_t len);

bool Asset_ImageOk(const Image_t *img);
int Asset_ImageInfo(const char *path, uint16_t *w, uint16_t *h);
int Asset_ImageOpen(const Image_t *img, lfs_file_t *f);
int Asset_ImageRead(lfs_file_t *f, void *buf, uint32_t len);
void Asset_ImageClose(lfs_file_t *f);

int Asset_SaveData(const char *name, const void *buf, uint32_t len);
int Asset_LoadData(const char *name, void *buf, uint32_t buf_size);
int Asset_DataSize(const char *name, uint32_t *size);
bool Asset_HasData(const char *name);
int Asset_DeleteData(const char *name);

#endif /* __ASSET_H__ */
