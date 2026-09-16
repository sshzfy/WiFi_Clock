#ifndef __BUILD_CONFIG_H__
#define __BUILD_CONFIG_H__

/* ============================================================
 * Build switches
 * ------------------------------------------------------------
 * FreeRTOS 开关:
 *   1 = 使用FreeRTOS调度
 *   0 = 裸机,方便测试模块完成后加入FreeRTOS调度
 *
 * BM_TEST_MODULE (only used when USE_FREERTOS == 0):
 *   BM_TEST_MODULE_OLED   = OLED (SSD1306 0.96, soft I2C PB6=SCL/PB7=SDA)
 *   BM_TEST_MODULE_LIGHT  = 光敏电阻 (DO: PC1/EXTI1; AO: PC0/ADC1_IN10)
 *   BM_TEST_MODULE_DS1302 = DS1302 外部RTC (PE7=RST/PE8=IO/PE9=CLK)
 *   BM_TEST_MODULE_W25Q64 = W25Q64 SPI Flash 裸驱动 (CS=PA4/CLK=PA5/MISO=PA6/MOSI=PA7)
 *   BM_TEST_MODULE_LFS    = littlefs 文件系统 (依赖 W25Q64)
 * ------------------------------------------------------------
 * 建议测试顺序: W25Q64 -> LFS。底层驱动不通时文件系统必然失败,
 * 先跑 W25Q64 可以把"硬件/SPI/时序问题"与"文件系统问题"分开定位。
 * ============================================================ */

#define USE_FREERTOS   1

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
 *   完整步骤见 Documents/W25Q64资源迁移说明.md
 *
 * 注意: 备份数据仍完整保留在 Screen_Resource 的 .c 文件里,
 *       只是被 #if 排除在正式固件之外, 随时可重新烧录补救。
 * ============================================================ */
#define RESOURCE_DATA_IN_ROM   0
#define RESOURCE_PROVISION     0
#define PROVISION_BATCH        1

/* 烧录固件必然需要数据本体, 这里自动联动, 避免两个开关不一致 */
#if (RESOURCE_PROVISION == 1)
#undef  RESOURCE_DATA_IN_ROM
#define RESOURCE_DATA_IN_ROM   1
#endif

#if (USE_FREERTOS == 0)
/* 裸机模块测试项: 在此切换。
 * 注意: 该宏是唯一入口, 不要在 bare_test.c 里另建同名变量。 */
#ifndef BM_TEST_MODULE
#define BM_TEST_MODULE BM_TEST_MODULE_LFS
#endif
#endif /* USE_FREERTOS == 0 */

#endif /* __BUILD_CONFIG_H__ */
