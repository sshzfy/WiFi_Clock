#ifndef __LFS_CONFIG_H__
#define __LFS_CONFIG_H__

#include "W25Q64.h"

/* 内存分配配置 */
#define LFS_READ_SIZE         16 // 最小读取大小（字节）
#define LFS_PROGRAM_SIZE      16 // 最小编程大小（字节）

/* 缓存配置 */
#define LFS_BLOCK_SIZE        W25Q64_SECTOR_SIZE  // 最小擦除块大小 = 4KB 扇区
#define LFS_BLOCK_COUNT       W25Q64_SECTOR_COUNT // 块数量 = 8MB/4KB = 2048
#define LFS_CACHE_SIZE        16                  // 缓存大小
#define LFS_LOOKAHEAD_SIZE    16                  // 预读大小(必须为8的倍数)

/* 块设备配置 */
#define LFS_BLOCK_CYCLES      500 // 磨损均衡周期

/* 文件系统配置 */
// #define LFS_NAME_MAX          255 // 最大文件名长度
// #define LFS_FILE_MAX          0   // 最大文件大小
// #define LFS_ATTR_MAX          0   // 最大属性大小

#endif /* __LFS_CONFIG_H__ */
