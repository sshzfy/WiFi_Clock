#ifndef __BUILD_CONFIG_H__
#define __BUILD_CONFIG_H__

/* ============================================================
 * FreeRTOS 开关:
 *   1 = 使用FreeRTOS调度
 *   0 = 裸机,方便测试模块完成后加入FreeRTOS调度
 * ============================================================ */

#define USE_FREERTOS        1

/* ============================================================
 * 是否使用更多颜色(默认纯黑、纯白、中灰、深天蓝、薰衣草淡紫)
 * ============================================================ */

#define MULTICOLLOR_SUPPORT        0

/* ============================================================
 * Debug switches:
 *   0 = 正式固件。DBG_PRINTF() 编译为空, USART2 不初始化
 *   1 = 调试固件。DBG_PRINTF() -> printf -> USART2,115200-8N1, 
 *       整行经 DMA 发送
 * ============================================================ */

#define DEBUG_SUPPORT        0

#if (DEBUG_SUPPORT == 1)
#include <stdio.h>
#define DBG_PRINTF(...)      printf(__VA_ARGS__)

#else
#define DBG_PRINTF(...)      ((void)0)
#endif

/* ============================================================
 * 内存监控开关:
 *   1 = 启用 Task_Monitor 任务, 周期打印堆用量与各任务栈峰值
 * ============================================================ */

#define MONITOR_TASK_ENABLE        0

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

/* ============================================================
 * OTA 串口离线测试:
 *   1 = 创建 ota 任务，通过 USART2(PA2/PA3) 接收离线 .ota 固件。
 *       与 DEBUG_SUPPORT 互斥(都占用 USART2)，测试时保持 DEBUG_SUPPORT=0。
 *   0 = 不创建任务，不占用 USART2。
 * ============================================================ */

#define OTA_SERIAL_TEST        0

/* ============================================================
 * OTA 固件大小兜底值:
 * App 备份/自检需要知道"自身固件有多大"。优先取参数区里
 * Bootloader 记录的 app_size；仅当参数区为空时才使用下面的常量。
 * 必须 >= 实际固件大小，否则备份会被截断。注意手动维护。
 * ============================================================ */

#define APP_FW_SIZE_FALLBACK    (160U * 1024U)

/* ============================================================
 * OTA 更新源: Gitee Releases:
 * ------------------------------------------------------------
 * 版本自动发现，发布新版本不用改代码：
 *   1. GET https://gitee.com/<owner>/<repo>/releases/latest
 *      服务器回 302，Location 指向最新 tag：
 *        https://gitee.com/<owner>/<repo>/releases/tag/v1.0.7
 *   2. 从 Location 尾部抠出 tag -> v1.0.7
 *   3. 拼出附件地址并下载：
 *        https://gitee.com/<owner>/<repo>/releases/download/v1.0.7/fw_1.0.7.ota
 *
 * 发布约定：
 *   1. tag    形如 v1.0.7    —— 三段十进制数字，不带前导零
 *   2. 附件名  fw_1.0.7.ota  —— "fw_" + tag去掉'v' + ".ota"
 *   两项必须严格对应，否则设备拼出的地址会 404。
 * 注意：发布顺序必须单调递增。若先发 v1.0.8 再补发 v1.0.7，
 *       releases/latest 仍可能指向 1.0.8，补发的版本收不到。
 * ============================================================ */

#define OTA_GITEE_OWNER         "ssh_zfy"
#define OTA_GITEE_REPO          "WiFi_Clock"

/* ============================================================
 * 触摸校准开关:
 *   0 = 正常使用。开机先读 /data/touch_cal.bin:
 *       读到了  → 直接用, 不再校准
 *       没读到  → 要求现场点 5 个十字, 成功后写回 Flash, 之后开机就不用了
 *   1 = 强制重新校准。每次上电都清掉内存里的参数重新走 5 点校准
 * ============================================================ */

#define TOUCH_FORCE_CALIBRATE        0

#endif /* __BUILD_CONFIG_H__ */
