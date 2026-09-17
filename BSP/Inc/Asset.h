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

/* ============================================================
 * 资源访问层
 * ------------------------------------------------------------
 * 全部字模(ASCII 表 + 汉字库)与图片都存放在 W25Q64 的 littlefs
 * 文件里, MCU Flash 中只有描述信息。本模块负责:
 *   1. 挂载 littlefs (失败时绝不自动格式化, 否则会清空全部资源)
 *   2. 常开字库文件句柄, 避免每次取字模都 open/close
 *   3. 开机自检, 逐项核对文件是否存在且大小正确
 *   4. 提供 O(1) 的字模读取与分块图片读取
 *
 * 所有接口在资源不可用时都会安全返回失败, 渲染层据此退化为
 * 填充背景色, 不会死机也不会重启。
 * ============================================================ */

#define ASSET_IMAGE_HEADER_SIZE    4U /* 图片文件头: width(2, 大端) + height(2, 大端), 与 LFS_Operation 一致 */

typedef struct
{
    const char *path; // W25Q64 上的 littlefs 路径
    uint32_t size;    // 文件应有的字节数, 自检用
} Asset_FontFile_t;

int Asset_Init(void);
bool Asset_Ready(void);
uint32_t Asset_MissingCount(void);
uint32_t Asset_CheckedCount(void);
int Asset_ReadFont(uint8_t fid, uint32_t off, void *buf, uint32_t len);
int Asset_ImageOpen(const Image_t *img, lfs_file_t *f);
int Asset_ImageRead(lfs_file_t *f, void *buf, uint32_t len);
void Asset_ImageClose(lfs_file_t *f);
bool Asset_ImageOk(const Image_t *img);

#endif /* __ASSET_H__ */
