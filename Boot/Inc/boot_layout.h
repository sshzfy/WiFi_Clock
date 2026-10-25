#ifndef __BOOT_LAYOUT_H__
#define __BOOT_LAYOUT_H__

/* 内部 Flash 三段分区（STM32F407VET6，512 KB）：
 *   0x08000000  48 KB  Bootloader  Sector 0-2
 *   0x0800C000  16 KB  参数区      Sector 3
 *   0x08010000 448 KB  App         Sector 4-7
 * Bootloader 与参数区不能共用一个扇区，否则擦参数会把 Bootloader 擦掉。 */

#define BOOT_BASE        0x08000000UL     // Bootloader 地址
#define BOOT_SIZE        (48UL * 1024UL)  // Bootloader 大小, 48 KB
#define BOOT_END         (BOOT_BASE + BOOT_SIZE)

/* 参数区：Sector 3，内部按 32 条 x 512 B 日志式追加 */
#define BOOT_PARAM_BASE        0x0800C000UL     // 参数区 地址
#define BOOT_PARAM_SIZE        (16UL * 1024UL)  // 参数区大小, 16 KB
#define BOOT_PARAM_END         (BOOT_PARAM_BASE + BOOT_PARAM_SIZE)

/* App 的 Keil IROM 必须与此一致：Start=0x08010000，Size=0x70000 */
#define APP_BASE        0x08010000UL      // App 地址
#define APP_SIZE        (448UL * 1024UL)  // App 大小, 448 KB
#define APP_END         (APP_BASE + APP_SIZE)

/* OTA 文件放在 littlefs 里，不是物理裸分区。
 * 先写 .tmp，校验通过后再 rename，避免 Bootloader 读到写了一半的固件。 */
#define OTA_DIR                    "/ota"
#define OTA_FW_PATH                "/ota/firmware.bin"
#define OTA_FW_TMP_PATH            "/ota/firmware.tmp"
#define OTA_BACKUP_PATH            "/ota/backup.bin"
#define OTA_BACKUP_TMP_PATH        "/ota/backup.tmp"

#endif /* __BOOT_LAYOUT_H__ */
