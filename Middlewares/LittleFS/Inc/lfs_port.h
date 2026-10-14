#ifndef __LFS_PORT_H__
#define __LFS_PORT_H__

#include "lfs.h"
#include "W25Q64.h"
#include "lfs_config.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

extern lfs_t g_lfs;
extern const struct lfs_config g_lfs_config;

int LittleFS_Init(void);
int LittleFS_Format(void);

/* ============================================================
 * 挂载状态与访问互斥
 * ------------------------------------------------------------
 * 全工程只有资源层(BSP/Asset.c)与裸机测试会用到 littlefs, 它们共用
 * 同一个挂载实例 g_lfs, 因此:
 *   1. 全工程只允许通过 LittleFS_Mount() 挂载: 它带状态记录, 重复
 *      调用直接返回成功, 不会把已打开的句柄/缓存冲掉;
 *   2. 每次访问 littlefs 前必须持锁: W25Q64 挂在 SPI1 上, 一个任务
 *      正在写(擦除一个 4KB 扇区要几十毫秒)时另一个任务插进来读,
 *      片选与时钟会交错, 数据必然出错。
 * ============================================================ */

int LittleFS_Mount(void);   /* 幂等挂载: 已挂载则直接返回 LFS_ERR_OK */
int LittleFS_Unmount(void); /* 卸载: 未挂载时直接返回 LFS_ERR_OK */
bool LittleFS_IsMounted(void);

/* 访问互斥(递归)。USE_FREERTOS=0 时为空操作。
 * 允许嵌套, 但必须成对调用。 */
void LittleFS_Lock(void);
void LittleFS_Unlock(void);

#endif /* __LFS_PORT_H__ */
