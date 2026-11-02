#include "boot_param.h"

#include "boot_layout.h"
#include "boot_crc32.h"

#include "stm32f4xx.h"
#include "stm32f4xx_flash.h"

#include <stddef.h>
#include <string.h>

/** @brief 找第 slot 个槽的 Flash 地址并返回 */
static uint32_t Param_Addr(uint32_t slot)
{
    return BOOT_PARAM_BASE + slot * BOOT_PARAM_SLOT_SIZE;
}

/**
 * @brief  填充一份默认记录
 * @param  p 输出结构体
 * @note   只写 state=IDLE，seq=0，magic，layout。
 */
void Boot_Param_Default(boot_param_t *p)
{
    if (p == NULL)
        return;

    memset(p, 0, sizeof(*p));
    p->magic = BOOT_PARAM_MAGIC;
    p->layout = BOOT_PARAM_LAYOUT;
    p->state = BOOT_ST_IDLE;
}

/**
 * @brief  从参数区加载最新一条有效记录
 * @param  p 输出结构体
 * @return true = 找到有效记录；false = 参数区为空(seq=0)或全部损坏
 */
bool Boot_Param_Load(boot_param_t *p)
{
    boot_param_t rec;
    bool found = false;

    if (p == NULL)
        return false;

    Boot_Param_Default(p); // 初始化默认记录

    for (uint32_t i = 0U; i < BOOT_PARAM_SLOT_COUNT; i++)
    {
        memcpy(&rec, (const void *)Param_Addr(i), sizeof(rec));

        if (rec.magic != BOOT_PARAM_MAGIC)
            continue;
        if (rec.layout != BOOT_PARAM_LAYOUT)
            continue;
        if (rec.crc32 != Boot_CRC32(&rec, offsetof(boot_param_t, crc32)))
            continue;

        if (!found || rec.seq > p->seq)
        {
            *p = rec;
            found = true;
        }
    }

    return found;
}

/**
 * @brief  追加保存一条记录
 * @param  p 待保存记录（magic/layout/crc32 由本函数填充）
 * @return true = 写入并回读校验成功
 */
bool Boot_Param_Save(boot_param_t *p)
{
    uint32_t slot = BOOT_PARAM_SLOT_COUNT;

    if (p == NULL)
        return false;

    /* 找到第一个完全空槽 */
    for (uint32_t i = 0U; i < BOOT_PARAM_SLOT_COUNT; i++)
    {
        uint32_t first_word;
        memcpy(&first_word, (const void *)Param_Addr(i), sizeof(first_word));
        if (first_word == 0xFFFFFFFFUL)
        {
            slot = i;
            break;
        }
    }

    /* 32 条写满：擦掉整个扇区，从 0 号槽重新开始 */
    if (slot >= BOOT_PARAM_SLOT_COUNT)
    {
        uint32_t primask = __get_PRIMASK();
        __disable_irq();

        FLASH_Unlock();
        FLASH_Status status = FLASH_EraseSector(FLASH_Sector_3, VoltageRange_3);
        FLASH_Lock();

        __set_PRIMASK(primask);

        if (status != FLASH_COMPLETE)
            return false;

        slot = 0U;
    }

    /* 记录内容(magic/layout/crc32) */
    p->magic = BOOT_PARAM_MAGIC;
    p->layout = BOOT_PARAM_LAYOUT;
    p->crc32 = Boot_CRC32(p, offsetof(boot_param_t, crc32));

    /* 写入记录到 Flash 的参数区 */
    uint32_t addr = Param_Addr(slot); // 第 slot 个槽的 Flash 地址
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    FLASH_Unlock();
    for (uint32_t off = 0U; off < sizeof(*p); off += 4U)
    {
        uint32_t word = 0U;
        memcpy(&word, (const uint8_t *)p + off, sizeof(word)); // 写 boot_param_t 字段(使用 memcpy 避免非对齐访问)

        if (FLASH_ProgramWord(addr + off, word) != FLASH_COMPLETE)
        {
            FLASH_Lock();
            __set_PRIMASK(primask);
            return false;
        }
    }
    FLASH_Lock();

    __set_PRIMASK(primask);

    return memcmp((const void *)addr, p, sizeof(*p)) == 0;
}

/**
 * @brief  把当前 App 标记为"已确认"
 * @return true = 已确认（或本次确认成功）
 * @note   已确认过则直接返回，避免每次开机写 Flash。
 */
bool Boot_Param_Confirm(void)
{
    boot_param_t p;

    Boot_Param_Default(&p);
    if (Boot_Param_Load(&p))
    {
        if ((p.flags & BOOT_FLAG_CONFIRMED) != 0U)
            return true;
        p.seq++;
    }
    else
    {
        p.seq = 1U;
    }

    p.state = BOOT_ST_IDLE;
    p.flags |= BOOT_FLAG_CONFIRMED;

    return Boot_Param_Save(&p);
}

/**
 * @brief  查询当前 App 是否已确认
 * @return true = 参数区里有 CONFIRMED 标志
 */
bool Boot_Param_IsConfirmed(void)
{
    boot_param_t p;

    if (!Boot_Param_Load(&p))
        return false;

    return (p.flags & BOOT_FLAG_CONFIRMED) != 0U;
}

/**
 * @brief  确认的同时写入自身固件 size/crc/version
 * @param  size    固件字节数
 * @param  crc32   固件 CRC32
 * @param  version 版本号编码
 * @return true = 已记录（或本次写入成功）
 * @note   App 启动确认任务调用；Bootloader 用这些值显示版本、
 *         校验固件。已确认且 size 有效时不重复写。
 */
bool Boot_Param_SetAppInfo(uint32_t size, uint32_t crc32, uint32_t version)
{
    boot_param_t p;

    if (Boot_Param_Load(&p))
    {
        /* 内容与版本完全一致，且已确认 → 无需写新记录 */
        if ((p.flags & BOOT_FLAG_CONFIRMED) != 0U &&
            p.app_size == size &&
            p.app_crc32 == crc32 &&
            p.app_version == version)
            return true;
        p.seq++;
    }
    else
    {
        Boot_Param_Default(&p);
        p.seq = 1U;
    }

    p.state = BOOT_ST_IDLE;
    p.flags |= BOOT_FLAG_CONFIRMED;
    p.app_size = size;
    p.app_crc32 = crc32;
    p.app_version = version;

    return Boot_Param_Save(&p);
}
