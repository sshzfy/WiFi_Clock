#ifndef __ASSET_H__
#define __ASSET_H__

#include "main.h"
#include "Font.h"
#include "Image.h"
#include "lfs.h"
#include <stdbool.h>

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

/* 图片文件头: width(2, 大端) + height(2, 大端), 与 LFS_Operation 一致 */
#define ASSET_IMAGE_HEADER_SIZE 4U

/**
 * @brief 初始化资源层: 初始化 W25Q64 -> 挂载 littlefs -> 打开字库 -> 自检
 * @return 0 表示全部资源就绪; 负数表示失败或存在缺失项(详见串口日志)
 */
int Asset_Init(void);

/** @brief littlefs 是否已成功挂载 */
bool Asset_Ready(void);

/** @brief 自检发现的问题项数(文件缺失或大小不符) */
uint32_t Asset_MissingCount(void);

/** @brief 自检核对过的资源项总数 */
uint32_t Asset_CheckedCount(void);

/**
 * @brief 从字库资源文件读取一段数据
 * @param fid 资源文件 ID(见 Asset_FontId_t)
 * @param off 文件内字节偏移
 * @param buf 输出缓冲区
 * @param len 要读取的字节数
 * @return 0 成功; 负数失败
 */
int Asset_ReadFont(uint8_t fid, uint32_t off, void *buf, uint32_t len);

/**
 * @brief 打开图片文件并跳过 4 字节文件头
 * @param img 图片描述
 * @param f   输出: 打开的文件句柄
 * @return 0 成功; 负数失败
 */
int Asset_ImageOpen(const Image_t *img, lfs_file_t *f);

/** @brief 从已打开的图片文件顺序读取 len 字节 */
int Asset_ImageRead(lfs_file_t *f, void *buf, uint32_t len);

/** @brief 关闭图片文件 */
void Asset_ImageClose(lfs_file_t *f);

/** @brief 该图片是否已通过自检(存在且大小正确) */
bool Asset_ImageOk(const Image_t *img);

#endif /* __ASSET_H__ */
