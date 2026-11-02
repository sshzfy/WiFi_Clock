#ifndef __BUILD_CONFIG_H__
#define __BUILD_CONFIG_H__

#define USE_FREERTOS               1 // FreeRTOS 开关：1 = 使用FreeRTOS调度，0 = 裸机
#define MONITOR_TASK_ENABLE        0 // 内存监控开关：1 = 启用 Task_Monitor 任务, 周期打印堆用量与各任务栈峰值
#define DEBUG_SUPPORT              0 // Debug switches：0 = 正式固件。无串口打印，1 = 调试固件。串口 DMA 发送

#if (DEBUG_SUPPORT == 1)
#include <stdio.h>
#define DBG_PRINTF(...)      printf(__VA_ARGS__)
#else
#define DBG_PRINTF(...)      ((void)0)
#endif

#define TOUCH_FORCE_CALIBRATE        0 // 触摸校准开关：0 = 使用 /data/touch_cal.bin 保存的数据，1 = 强制重新校准
#define MULTICOLLOR_SUPPORT          0 // 是否使用更多颜色(默认纯黑、纯白、中灰、深天蓝、薰衣草淡紫)
#define APP_FW_SIZE_FALLBACK         (160U * 1024U) // OTA 固件大小兜底值（注意手动维护 ）

/* 资源存储开关 (字库/图片已迁移到外部 W25Q64)
 * ------------------------------------------------------------
 * RESOURCE_DATA_IN_ROM:
 *   0 = 正式固件。源码中的资源数据不参与编译, 全部字模与图片
 *       都从 W25Q64 的 littlefs 文件读取, 省下约 285KB Flash。
 *   1 = 烧录固件。把备份在源码里的资源数据编译进来, 上电后由
 *       Provision_Run() 写入 W25Q64。仅烧录时短暂使用。
 *
 * RESOURCE_PROVISION:
 *   1 = main() 直接进入资源烧录流程(不启动 FreeRTOS, 不初始化LCD)。
 *
 * PROVISION_BATCH:
 *   烧录批次 1..3。资源总量约 822KB 超过 512KB Flash, 必须分三次
 *   编译烧录, 每批只把该批数据编译进来:
 *     1 = 汉字库 16 + 22        (367990 字节)
 *     2 = 开机全屏图 + 主页全屏图 (307200 字节)
 *     3 = 9 张 ASCII 表 + 33 张图标 (183460 字节)
 *
 * 注意: 备份数据仍完整保留在 Screen_Resource 的 .c 文件里,
 *       只是被 #if 排除在正式固件之外, 随时可重新烧录补救
 */
#define RESOURCE_DATA_IN_ROM        0
#define RESOURCE_PROVISION          0
#define PROVISION_BATCH             1

/* 烧录固件必然需要数据本体, 自动联动, 避免两个开关不一致 */
#if (RESOURCE_PROVISION == 1)
#undef  RESOURCE_DATA_IN_ROM
#define RESOURCE_DATA_IN_ROM        1
#endif

#if (USE_FREERTOS == 0)
/* 裸机模块测试项: 在此切换 */
#ifndef BM_TEST_MODULE
#define BM_TEST_MODULE        BM_TEST_MODULE_TOUCH
#endif
#endif /* USE_FREERTOS == 0 */

#endif /* __BUILD_CONFIG_H__ */
