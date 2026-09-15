#ifndef __PROVISION_H__
#define __PROVISION_H__

#include "main.h"
#include "BuildConfig.h"

/* ============================================================
 * 资源烧录固件
 * ------------------------------------------------------------
 * 字库与图片平时不编译进 MCU(正式固件里 RESOURCE_DATA_IN_ROM=0),
 * 需要写入 W25Q64 时, 把 BuildConfig.h 改成:
 *     #define RESOURCE_PROVISION 1
 *     #define PROVISION_BATCH    N     (N = 1..3)
 * 重新编译烧录, 上电后本固件会把该批资源写进 littlefs。
 *
 * 资源总量约 822KB 超过 512KB Flash, 单次编译装不下, 必须分三批:
 *   1 = 汉字库 16 + 22
 *   2 = 开机全屏图 + 主页全屏图
 *   3 = 9 张 ASCII 表 + 32 张图标
 * 详细步骤见 Documents/W25Q64资源迁移说明.md
 * ============================================================ */

#if (RESOURCE_PROVISION == 1)

/* 烧录入口, 由 main() 直接调用。函数内部是死循环, 不返回。 */
void Provision_Run(void);

#endif /* RESOURCE_PROVISION */

#endif /* __PROVISION_H__ */
