#ifndef __BOOT_PARAM_H__
#define __BOOT_PARAM_H__

#include <stdint.h>
#include <stdbool.h>

#define BOOT_PARAM_MAGIC         0x424F4F54UL // 'BOOT' 
#define BOOT_PARAM_LAYOUT        1U           // 结构体布局版本，改字段就 +1 

#define BOOT_PARAM_SLOT_SIZE         512U // 每条记录 64 B
#define BOOT_PARAM_SLOT_COUNT        32U  // 循环使用 32 条记录

/* 启动标志位 */
#define BOOT_FLAG_CONFIRMED             (1UL << 0) // App 已确认能正常启动 
#define BOOT_FLAG_BACKUP_VALID          (1UL << 1) // backup.bin 有效，可回滚 
#define BOOT_FLAG_OTA_VERIFIED          (1UL << 2) // 待升级固件已通过 CRC 
#define BOOT_FLAG_FORCE_RECOVERY        (1UL << 3) // 强制进入串口救援 

/* 升级状态机：IDLE -> OTA_PENDING -> OTA_WRITING -> APP_UNCONFIRMED -> IDLE
 * 异常分支：APP_UNCONFIRMED 连续 3 次未确认 -> ROLLBACK；无固件可用 -> RECOVERY */
typedef enum
{
    BOOT_ST_IDLE = 0,        // 正常：App 有效且已确认
    BOOT_ST_OTA_PENDING,     // 新固件就绪，等待搬运 
    BOOT_ST_OTA_WRITING,     // 正在搬运，掉电后据此重入 
    BOOT_ST_APP_UNCONFIRMED, // 新 App 已写入，等待首次确认 
    BOOT_ST_ROLLBACK,        // 等待从 backup.bin 恢复 
    BOOT_ST_RECOVERY         // 无可用固件，等待串口救援 
} boot_state_t;

/* 参数区记录结构体(均为 4 字节对齐) */
typedef struct
{
    uint32_t magic;  // BOOT_PARAM_MAGIC 
    uint32_t seq;    // 单调递增，取最大者有效 
    uint32_t layout; // BOOT_PARAM_LAYOUT 
    uint32_t state;  // boot_state_t 

    uint32_t app_size;    // 当前 App 有效字节数（CRC 只算这么多） 
    uint32_t app_crc32;   // 当前 App 的 CRC32 
    uint32_t app_version; // major<<16 | minor<<8 | patch 

    uint32_t ota_size;    // 待升级固件负载大小 
    uint32_t ota_crc32;   // 待升级固件 CRC32 
    uint32_t ota_version; // 待升级版本 
    uint32_t ota_slot;    // 预留：将来若改物理裸分区，记录 A/B 槽 

    uint32_t boot_try;   // 新 App 已尝试启动次数，达到 3 次未确认就回滚 
    uint32_t flags;      // BOOT_FLAG_* 
    uint32_t reserved[2]; // 补齐 64 B（17 个 uint32_t 中的最后 2 个预留位） 
    uint32_t crc32;       // 前面所有字节的 CRC32，必须放最后 
} boot_param_t;

/* 编译期断言：记录必须是 64 B（正好 16 次 FLASH_ProgramWord）。
 * 不满足时数组长度为负，直接编译报错。 */
typedef char boot_param_size_must_be_64[(sizeof(boot_param_t) == 64) ? 1 : -1];

void Boot_Param_Default(boot_param_t *p);
bool Boot_Param_Load(boot_param_t *p);
bool Boot_Param_Save(boot_param_t *p);

/* App 侧：新固件首次启动后写 CONFIRMED；已确认过则直接返回 */
bool Boot_Param_Confirm(void);
bool Boot_Param_IsConfirmed(void);

/* App 侧：确认的同时写入自身固件 size/crc/version，
 * 供 Bootloader 校验与显示；已确认且 size 有效时不重复写。 */
bool Boot_Param_SetAppInfo(uint32_t size, uint32_t crc32, uint32_t version);

#endif
