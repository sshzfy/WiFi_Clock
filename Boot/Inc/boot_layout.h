#ifndef __BOOT_LAYOUT_H__
#define __BOOT_LAYOUT_H__

/* 内部 Flash 三段分区（STM32F407VET6，512 KB）：
 *   0x08000000  48 KB   Bootloader  Sector 0-2
 *   0x0800C000  16 KB   参数区      Sector 3
 *   0x08010000  448 KB  App         Sector 4-7
 * Bootloader 与参数区不能共用一个扇区，否则擦参数会把 Bootloader 擦掉。 */

 /* Bootloader 地址范围: 0x08000000 - 0x0800C000 */
#define BOOT_BASE        0x08000000UL    // Bootloader 地址
#define BOOT_SIZE        (48UL * 1024UL) // Bootloader 大小, 48 KB
#define BOOT_END         (BOOT_BASE + BOOT_SIZE)

 /* 参数区 地址范围: 0x0800C000 - 0x08010000 */
#define BOOT_PARAM_BASE        0x0800C000UL    // 参数区 地址
#define BOOT_PARAM_SIZE        (16UL * 1024UL) // 参数区大小, 16 KB
#define BOOT_PARAM_END         (BOOT_PARAM_BASE + BOOT_PARAM_SIZE)

 /* RAM 地址范围: 
  * IRAM1（主SRAM）: 0x20000000 - 0x20020000
  * IRAM2（CCM RAM）: 0x10000000 - 0x10010000 */
#define IRAM1_BASE        0x20000000UL // IRAM1（主SRAM） 地址
#define IRAM1_SIZE        0x00020000UL // IRAM1 大小，128 KB
#define IRAM1_END         (IRAM1_BASE + IRAM1_SIZE)
#define IRAM2_BASE        0x10000000UL // IRAM2（CCM RAM） 地址
#define IRAM2_SIZE        0x00010000UL // IRAM2 大小，64 KB
#define IRAM2_END         (IRAM2_BASE + IRAM2_SIZE)

 /* App 地址范围: 0x08010000 - 0x08080000 */
#define APP_BASE        0x08010000UL     // App 区地址
#define APP_SIZE        (448UL * 1024UL) // App 大小, 448 KB
#define APP_END         (APP_BASE + APP_SIZE)

/* OTA 文件放目录: /ota
 * 先写 .tmp，校验通过后再 rename，避免 Bootloader 读到写了一半的固件。 */
#define OTA_DIR                    "/ota"              // OTA 文件目录
#define OTA_FW_PATH                "/ota/firmware.bin" // OTA 固件文件路径
#define OTA_FW_TMP_PATH            "/ota/firmware.tmp" // OTA 固件临时文件路径
#define OTA_BACKUP_PATH            "/ota/backup.bin"   // OTA 备份文件路径
#define OTA_BACKUP_TMP_PATH        "/ota/backup.tmp"   // OTA 备份临时文件路径

#endif /* __BOOT_LAYOUT_H__ */
