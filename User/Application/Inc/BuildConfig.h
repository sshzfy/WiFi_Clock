#ifndef __BUILD_CONFIG_H__
#define __BUILD_CONFIG_H__

/* ============================================================
 * Build switches
 * ------------------------------------------------------------
 * FreeRTOS 开关:
 *   1 = 使用FreeRTOS调度
 *   0 = 裸机,方便测试模块完成后加入FreeRTOS调度
 * ============================================================ */

#define USE_FREERTOS        1

/* ============================================================
 * 多色彩支持(默认关闭,仅使用默认的几种颜色)
 * ------------------------------------------------------------
 * 纯黑、纯白、中灰、深天蓝、薰衣草淡紫
 * ============================================================ */

#define MULTICOLLOR_SUPPORT        0

/* ============================================================
 * Debug switches
 * 串口调试打印总开关
 * ------------------------------------------------------------
 * DEBUG_SUPPORT:
 *   0 = 正式固件。DBG_PRINTF() 编译为空, USART2 不初始化,
 *       printf / fputc / DMA / 调试互斥量都不参与链接,
 *       不占 Flash/RAM, 也不占用 PA2/PA3。
 *   1 = 调试固件。DBG_PRINTF() -> printf -> USART2(PA2=TX/PA3=RX),
 *       115200-8N1, 整行经 DMA 发送。
 * 注意: 资源烧录固件(RESOURCE_PROVISION=1)和裸机测试固件
 * (USE_FREERTOS=0)的进度也走 DBG_PRINTF, 需要串口日志时把
 * 这里改成 1 再编译。
 * ============================================================ */

#define DEBUG_SUPPORT        0

#if (DEBUG_SUPPORT == 1)
#include <stdio.h>
#define DBG_PRINTF(...)      printf(__VA_ARGS__)
#else
#define DBG_PRINTF(...)      ((void)0)
#endif

/* ============================================================
 * 资源存储开关 (字库/图片已迁移到外部 W25Q64)
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
 *       只是被 #if 排除在正式固件之外, 随时可重新烧录补救。
 * ============================================================ */

#define RESOURCE_DATA_IN_ROM        0
#define RESOURCE_PROVISION          0
#define PROVISION_BATCH             1

/* 烧录固件必然需要数据本体, 这里自动联动, 避免两个开关不一致 */
#if (RESOURCE_PROVISION == 1)
#undef  RESOURCE_DATA_IN_ROM
#define RESOURCE_DATA_IN_ROM        1
#endif

#if (USE_FREERTOS == 0)
/* 裸机模块测试项: 在此切换。
 * 注意: 该宏是唯一入口, 不要在 bare_test.c 里另建同名变量。 */
#ifndef BM_TEST_MODULE
#define BM_TEST_MODULE        BM_TEST_MODULE_TOUCH
#endif
#endif /* USE_FREERTOS == 0 */

/* ============================================================
 * 触摸开关
 * ------------------------------------------------------------
 * 校准策略:
 *   0 = 正常使用。开机先读 /data/touch_cal.bin:
 *       读到了  → 直接用, 不再校准(日志: calibration loaded: kx=... )
 *       没读到  → 要求现场点 5 个十字, 成功后写回 Flash, 之后开机就不用了
 *   1 = 强制重新校准。每次上电都清掉内存里的参数重新走 5 点校准
 * ============================================================ */

#define TOUCH_FORCE_CALIBRATE        0

#endif /* __BUILD_CONFIG_H__ */
