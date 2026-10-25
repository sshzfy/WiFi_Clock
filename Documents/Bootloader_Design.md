# Bootloader 与 OTA 升级设计方案

> 适用工程：`Project4`（STM32F407VET6 桌面天气时钟）
> 文档性质：设计规划，不包含完整实现代码
> 版本：v1.0 · 2026-10-22

---

## 1. 结论摘要

| 项目 | 方案 |
| --- | --- |
| 启动结构 | **Bootloader（48 KB）+ 参数区（16 KB）+ App（448 KB）** 三段式 |
| App 起始地址 | `0x08010000`，向量表偏移 `0x10000` |
| Bootloader 职责 | 参数读取、固件校验、从 W25Q64 搬运、跳转；**不联网、不带 FreeRTOS、不挂 littlefs** |
| OTA 缓存 | W25Q64 划出 **1 MB 双区**：slot A（新固件）+ slot B（升级前备份） |
| 下载通道 | App 内新增 OTA 任务，经 ESP32-C3 原始 TCP + HTTP 流式下载 |
| 回滚 | `confirmed` 标志 + 启动尝试计数，3 次未确认自动回滚 slot B |
| 半砖保护 | 先完整校验再擦 App；搬运中断可重入；slot B 兜底 |
| 实施节奏 | Phase 0 分区与跳转 → Phase 1 串口离线 OTA → Phase 2 网络 OTA → Phase 3 回滚/看门狗/救援 |

**关键前提**：W25Q64 当前被 littlefs 整片占用（2048 × 4 KB），必须缩容到 6 MB 并**重新格式化、重烧一次资源**。这是本方案唯一的一次性迁移成本。

---

## 2. 现状盘点

### 2.1 硬件与资源

| 项目 | 现状 | 对本方案的影响 |
| --- | --- | --- |
| MCU | STM32F407VET6，512 KB Flash，128 KB SRAM + 64 KB CCM | Flash 扇区非均匀：16/64/128 KB |
| 当前固件 | `Total ROM Size = 149,572 B`（约 146 KB，见 `MDK/Output/STM32F407.map`） | App 分区 448 KB，余量约 302 KB |
| 当前 RAM | `Total RW Size = 81,472 B`（约 79.6 KB） | 余量约 46 KB，够 OTA 缓冲 |
| 外部 Flash | W25Q64 8 MB，littlefs v2.11，`LFS_BLOCK_COUNT = 2048` | 必须重新分区 |
| 资源占用 | 858,778 B（11 字库 + 32 图片） | 6 MB 文件系统仍余 5.1 MB |
| 联网 | ESP32-C3-MINI-1，AT v4.1.1.0，USART1 @115200 | 下载通道，需流式 TCP |
| 工具链 | Keil MDK + AC5，无 scatter file | 用 Target 对话框的 IROM 配置即可 |
| 启动文件 | `Core/Startup/startup_stm32f40xx.s`，栈 4 KB、堆 4 KB | Bootloader 与 App 共用 |

### 2.2 当前启动链

```text
上电 → 0x08000000 向量表 → Reset_Handler → SystemInit()
     → __main（RW/ZI 初始化）→ main()
     → NVIC_PriorityGroupConfig → Board_Peripheral_Init
     → App_Task_Init → vTaskStartScheduler
     → ui 任务：Board_Init → Boot_Page_Wait → 建 net/dht22 → 等 EV_NET_READY
```

现状没有分区、没有向量表重定位、没有内部 Flash 编程代码（SPL 的 `stm32f4xx_flash.c` 已在工程中但未被调用）。

### 2.3 关键约束

1. **F407 单 Bank**：擦除/编程期间 CPU 从 Flash 取指会被总线 stall。Bootloader 擦写 App 区时自身在另一扇区，可正常执行；App 擦写参数区时会有 200 ms 级停顿，需要控制时机。
2. **littlefs 独占全片**：`lfs_port.c` 的地址换算是 `block * 4096`，block 0 对应 W25Q64 地址 0。不缩容就没有裸区可用。
3. **AT 层是逐行文本解析**：`AT.c` 的 `rx_buf[1024]`、`AT_Usart_Wait_Receive()` 按 `\r\n` 切行，无法承载二进制固件流。
4. **USART1 环形缓冲只有 512 B**：`Usart.c` 的 `USART1_RX_RING_SIZE`，OTA 时必须扩大，否则 TCP 段一冲就溢出。
5. **LCD 是 ui 任务独占资源**：OTA 进度只能通过事件让 ui 任务绘制，OTA 任务不得直接操作 LCD。
6. **W25Q64 是 SPI1 单总线**：OTA 裸写与 littlefs 读资源会并发，必须共用同一把锁。

---

## 3. 总体架构

### 3.1 启动结构

```text
┌─────────────────────────────────────────────────────────────┐
│ 0x08000000  Bootloader 48 KB (Sector 0-2)                   │
│   读参数 → 判断状态 → 需要时初始化 W25Q64/LCD → 搬运 → 跳转  │
├─────────────────────────────────────────────────────────────┤
│ 0x0800C000  参数区 16 KB (Sector 3)                         │
│   32 条 × 512 B 日志式记录，seq + CRC32，掉电安全            │
├─────────────────────────────────────────────────────────────┤
│ 0x08010000  App 448 KB (Sector 4-7)                         │
│   FreeRTOS + littlefs + LVGL + 原有业务 + OTA 模块           │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 升级数据流

```text
                ┌──────────────┐
   HTTP/AT      │  W25Q64      │     复位      ┌──────────────┐
 服务器 ──────> │  slot A      │ ───────────> │ Bootloader   │
                │  (新固件)    │              │ 校验+备份+擦写│
                └──────────────┘              └──────┬───────┘
                       ▲                             │
                       │ 升级前备份                    ▼
                ┌──────────────┐              ┌──────────────┐
                │  W25Q64      │              │ 内部 Flash   │
                │  slot B      │ <─────────── │ App 区       │
                │  (旧固件)    │   回滚时      └──────────────┘
                └──────────────┘
```

### 3.3 设计原则

1. **Bootloader 尽量小、尽量笨**：只做"校验、搬运、跳转"，把网络、文件系统、界面全部留在 App。
2. **先校验后破坏**：任何擦除 App 的动作之前，必须确认新固件 CRC 正确且完整。
3. **任何一步掉电都可恢复**：状态机 + 日志式参数 + slot B 备份。
4. **正常启动不加负担**：App 有效时 Bootloader 只做几十毫秒的轻量检查，不初始化 W25Q64 和 LCD。
5. **不改现有业务分层**：OTA 作为新的应用层模块接入，BSP 只增加传输封装。

---

## 4. 存储布局

### 4.1 内部 Flash（512 KB）

| 区域 | 起始 | 大小 | 扇区 | 用途 |
| --- | --- | --- | --- | --- |
| Bootloader | `0x08000000` | 48 KB | Sector 0-2 | 启动、校验、搬运 |
| 参数区 | `0x0800C000` | 16 KB | Sector 3 | 32 × 512 B 记录 |
| App | `0x08010000` | 448 KB | Sector 4-7 | 应用固件 |
| 合计 | | 512 KB | | |

> Sector 0-3 各 16 KB，Sector 4 为 64 KB，Sector 5-7 各 128 KB。
> App 余量：448 KB - 146 KB ≈ **302 KB**，足够容纳 OTA 模块与后续功能。

**为什么不把参数区放到 W25Q64？**
参数区决定"跳不跳 App"，必须不依赖外部 Flash。W25Q64 虚焊/损坏时，Bootloader 仍能凭参数区判断并跳转已有 App。

### 4.2 W25Q64（8 MB）

| 区域 | 起始 | 大小 | 用途 |
| --- | --- | --- | --- |
| littlefs | `0x000000` | 6 MB | 字库、图片、`/data` 运行数据 |
| OTA slot A | `0x600000` | 512 KB | 下载的新固件 |
| OTA slot B | `0x680000` | 512 KB | 升级前备份，用于回滚 |
| OTA 元数据 | `0x700000` | 4 KB | 断点续传进度（Phase 3） |
| 预留 | `0x701000` | 1 MB - 4 KB | 日志/资源扩展 |

littlefs 缩容改 `lfs_config.h`：

```c
/* 6 MB，剩余 2 MB 给 OTA 与预留；block 地址从 0 开始，不重叠 */
#define LFS_BLOCK_COUNT   1536U
```

并加编译期断言，防止以后误改：

```c
#if ((uint64_t)LFS_BLOCK_COUNT * LFS_BLOCK_SIZE > OTA_SLOT_A_BASE)
#error "littlefs overlaps OTA slot A"
#endif
```

**迁移步骤（一次性）**：

1. 修改 `LFS_BLOCK_COUNT`；
2. 编译烧录 App，首次启动 `LittleFS_Init()` 会自动格式化（或调用 `LittleFS_Format()`）；
3. 按 `PROVISION_BATCH = 1/2/3` 重新烧录三次资源；
4. 校验 `Asset_Init()` 自检 43 项全部通过。

> 资源总量 858,778 B，6 MB 文件系统余量 5.1 MB，不影响后续扩展。

### 4.3 参数区记录

```c
#define BOOT_PARAM_MAGIC        0x424F4F54UL   /* 'BOOT' */
#define BOOT_PARAM_LAYOUT       1U
#define BOOT_PARAM_BASE         0x0800C000UL
#define BOOT_PARAM_SECTOR       FLASH_Sector_3
#define BOOT_PARAM_SLOT_SIZE    512U
#define BOOT_PARAM_SLOT_COUNT   32U

typedef enum
{
    BOOT_ST_IDLE = 0,          /* 正常：App 有效且已确认 */
    BOOT_ST_OTA_PENDING,       /* slot A 有完整固件，等待搬运 */
    BOOT_ST_OTA_WRITING,       /* 搬运中（掉电后据此重入） */
    BOOT_ST_APP_UNCONFIRMED,   /* 新 App 已写入，等待首次确认 */
    BOOT_ST_ROLLBACK,          /* 等待从 slot B 恢复 */
    BOOT_ST_RECOVERY,          /* 无可用固件，等待串口救援 */
} boot_state_t;

typedef struct
{
    uint32_t magic;            /* BOOT_PARAM_MAGIC */
    uint32_t seq;              /* 单调递增，取最大者有效 */
    uint32_t layout;           /* BOOT_PARAM_LAYOUT */
    uint32_t state;            /* boot_state_t */

    uint32_t app_size;         /* 当前 App 有效字节数（CRC 覆盖范围） */
    uint32_t app_crc32;        /* 当前 App 的 CRC32 */
    uint32_t app_version;      /* major<<16 | minor<<8 | patch */

    uint32_t ota_size;         /* slot 中固件字节数 */
    uint32_t ota_crc32;        /* slot 中固件 CRC32 */
    uint32_t ota_version;      /* 待升级版本 */
    uint32_t ota_slot;         /* 0=A, 1=B */

    uint32_t boot_try;         /* 新 App 已尝试启动次数 */
    uint32_t flags;            /* bit0=confirmed, bit1=backup_valid,
                                  bit2=ota_verified, bit3=force_recovery */
    uint32_t reserved[3];
    uint32_t crc32;            /* 覆盖本记录 crc32 之前的所有字节 */
} boot_param_t;                /* 64 B，4 字节对齐 */
```

**写入策略**

- 顺序写：找第一个 `magic == 0xFFFFFFFF` 的槽位，写入新记录，`seq = last_seq + 1`。
- 槽位写满 32 条后擦除 Sector 3，从 0 重新写。正常使用（每次升级 2~3 条、首次确认 1 条）几十次才擦一次。
- 每条记录独立 CRC32，最后一条半写时回退到前一条，天然掉电安全。
- App 写记录时用 `__disable_irq()` 保护，单条 64 B 仅 16 次 `FLASH_ProgramWord`，耗时 < 1 ms。

### 4.4 地址宏统一

新增 `Boot/Inc/boot_layout.h`，Bootloader 与 App 共用：

```c
#define BOOT_BASE               0x08000000UL
#define BOOT_SIZE               (48UL * 1024UL)

#define BOOT_PARAM_BASE         0x0800C000UL
#define BOOT_PARAM_SIZE         (16UL * 1024UL)

#define APP_BASE                0x08010000UL
#define APP_SIZE                (448UL * 1024UL)
#define APP_END                 (APP_BASE + APP_SIZE)

#define OTA_SLOT_A_BASE         0x00600000UL
#define OTA_SLOT_B_BASE         0x00680000UL
#define OTA_SLOT_SIZE           (512UL * 1024UL)
#define OTA_META_BASE           0x00700000UL

#define OTA_PKG_MAGIC           0x4F544131UL   /* 'OTA1' */
#define OTA_PKG_HEADER_SIZE     64UL
#define OTA_HW_ID               0x0407VE00UL   /* 预留：板型标识 */
```

---

## 5. Bootloader 设计

### 5.1 职责边界

| 做 | 不做 |
| --- | --- |
| 读参数、快速检查 App、完整 CRC | 初始化 FreeRTOS |
| 初始化 SPI1 + W25Q64（仅升级/恢复时） | 挂载 littlefs |
| 从 slot 搬运到 App 区 | 初始化 LVGL / 触摸 / OLED |
| 初始化精简 LCD 显示进度（仅升级时） | 联网、AT 指令 |
| 写参数、跳转、串口救援 | 业务逻辑 |

这样 Bootloader 代码量可控制在 **30 KB 以内**（48 KB 分区余量约 18 KB），正常启动路径 **< 50 ms**。

### 5.2 启动状态机

```c
int main(void)
{
    /* SystemInit 已由启动文件调用，时钟 168 MHz */
    Boot_Gpio_Minimal_Init();          /* PA4-7 / PC10-12 / PE2-5 时钟 */
    Boot_Param_Load(&g_param);         /* 扫描 32 条，取 seq 最大且 CRC 正确者 */

    switch (g_param.state)
    {
    case BOOT_ST_OTA_PENDING:
    case BOOT_ST_OTA_WRITING:
        Boot_Do_Update(/*slot=*/g_param.ota_slot, /*do_backup=*/true);
        break;                          /* 成功后内部跳转，失败转恢复 */

    case BOOT_ST_ROLLBACK:
        Boot_Do_Update(/*slot=*/1, /*do_backup=*/false);
        break;

    case BOOT_ST_APP_UNCONFIRMED:
        if (!Boot_App_QuickValid() || g_param.boot_try >= 3U)
        {
            g_param.state = BOOT_ST_ROLLBACK;
            Boot_Param_Save(&g_param);
            Boot_Do_Update(/*slot=*/1, /*do_backup=*/false);
        }
        g_param.boot_try++;
        Boot_Param_Save(&g_param);
        Boot_JumpToApp();
        break;

    case BOOT_ST_RECOVERY:
        Boot_Recovery();                /* 串口等待，Phase 3 */
        break;

    case BOOT_ST_IDLE:
    default:
        if (!Boot_App_QuickValid())
        {
            if (Boot_Slot_Valid(OTA_SLOT_A_BASE))
                Boot_Do_Update(/*slot=*/0, /*do_backup=*/true);
            else
                Boot_Recovery();
        }
        Boot_JumpToApp();
        break;
    }
}
```

### 5.3 App 有效性判定（两级）

**轻量检查（每次启动，< 1 ms）**

```c
bool Boot_App_QuickValid(void)
{
    uint32_t sp = *(volatile uint32_t *)APP_BASE;
    uint32_t pc = *(volatile uint32_t *)(APP_BASE + 4U);

    /* 栈顶必须落在 SRAM 或 CCM 范围内 */
    if (!((sp >= 0x20000000UL && sp <= 0x20020000UL) ||
          (sp >= 0x10000000UL && sp <= 0x10010000UL)))
        return false;

    /* 复位向量必须在 App 区内，且为 Thumb 地址 */
    if (pc < APP_BASE || pc >= APP_END || (pc & 1U) == 0U)
        return false;

    /* 前 8 个字不能全是擦除态 */
    for (uint32_t i = 0; i < 8U; i++)
        if (*(volatile uint32_t *)(APP_BASE + i * 4U) != 0xFFFFFFFFUL)
            return true;
    return false;
}
```

**完整 CRC（升级后首次启动、或参数区要求时）**

```c
bool Boot_App_FullValid(void)
{
    if (!Boot_App_QuickValid()) return false;
    if (g_param.app_size == 0U || g_param.app_size > APP_SIZE) return false;
    return CRC32_Calc((const uint8_t *)APP_BASE, g_param.app_size) == g_param.app_crc32;
}
```

> `app_size` 只覆盖实际固件长度（如 146 KB），不要求整个 448 KB 有效，CRC 计算量与打包工具一致。

### 5.4 搬运算法

```c
bool Boot_Do_Update(uint32_t slot, bool do_backup)
{
    ota_hdr_t hdr;
    uint32_t slot_base = (slot == 0U) ? OTA_SLOT_A_BASE : OTA_SLOT_B_BASE;

    Boot_Stage_LCD();                  /* 初始化 LCD/W25Q64，显示"升级中" */
    if (!W25Q64_Read_Header(slot_base, &hdr))            return Boot_Fail("no header");
    if (hdr.fw_size == 0U || hdr.fw_size > APP_SIZE)      return Boot_Fail("bad size");
    if (OTA_CRC32_Flash(slot_base + OTA_PKG_HEADER_SIZE, hdr.fw_size) != hdr.fw_crc32)
        return Boot_Fail("slot crc");

    /* 1) 备份当前 App，失败则中止（不破坏旧固件） */
    if (do_backup && Boot_App_QuickValid() && g_param.app_size > 0U)
    {
        if (!Boot_Backup_App(OTA_SLOT_B)) return Boot_Fail("backup");
        g_param.flags |= BOOT_FLAG_BACKUP_VALID;
    }

    /* 2) 标记"搬运中"，掉电后可重入 */
    g_param.state     = BOOT_ST_OTA_WRITING;
    g_param.ota_slot  = slot;
    g_param.ota_size  = hdr.fw_size;
    g_param.ota_crc32 = hdr.fw_crc32;
    g_param.ota_version = hdr.fw_version;
    Boot_Param_Save(&g_param);

    /* 3) 擦除 App 区（Sector 4-7） */
    FLASH_Unlock();
    for (uint32_t s = FLASH_Sector_4; s <= FLASH_Sector_7; s++)
    {
        FLASH_EraseSector(s, VoltageRange_3);
        Boot_WdgFeed();
    }
    FLASH_Lock();

    /* 4) 逐 4 KB 搬运，边写边算 CRC */
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t off = 0U; off < hdr.fw_size; off += 4096U)
    {
        uint32_t n = MIN(4096U, hdr.fw_size - off);
        W25Q64_Read(slot_base + OTA_PKG_HEADER_SIZE + off, g_buf, n);
        crc = CRC32_Update(crc, g_buf, n);
        Boot_Flash_Write(APP_BASE + off, g_buf, n);   /* 4 字节对齐 */
        Boot_Progress(off, hdr.fw_size);
        Boot_WdgFeed();
    }
    if ((crc ^ 0xFFFFFFFFUL) != hdr.fw_crc32) return Boot_Fail("write crc");

    /* 5) 回读校验 App 区 */
    if (CRC32_Calc((const uint8_t *)APP_BASE, hdr.fw_size) != hdr.fw_crc32)
        return Boot_Fail("verify");

    /* 6) 更新参数，准备首次启动 */
    g_param.state       = BOOT_ST_APP_UNCONFIRMED;
    g_param.app_size    = hdr.fw_size;
    g_param.app_crc32   = hdr.fw_crc32;
    g_param.app_version = hdr.fw_version;
    g_param.boot_try    = 0U;
    g_param.flags      &= ~BOOT_FLAG_CONFIRMED;
    g_param.flags      |=  BOOT_FLAG_OTA_VERIFIED;
    Boot_Param_Save(&g_param);

    Boot_JumpToApp();
    return true;                       /* 不会到这里 */
}
```

**备份函数**

```c
bool Boot_Backup_App(uint32_t slot_base)
{
    ota_hdr_t hdr = {
        .magic       = OTA_PKG_MAGIC,
        .header_size = OTA_PKG_HEADER_SIZE,
        .fw_size     = g_param.app_size,
        .fw_crc32    = g_param.app_crc32,
        .fw_version  = g_param.app_version,
        .hw_id       = OTA_HW_ID,
    };

    /* 擦除实际需要的扇区（146 KB 约 36 个），不是整个 512 KB */
    uint32_t sectors = (g_param.app_size + 4095U) / 4096U;
    for (uint32_t i = 0U; i < sectors; i++)
    {
        W25Q64_SectorErase(slot_base + i * 4096U);
        Boot_Progress(i, sectors);
    }

    W25Q64_PageProgram(slot_base, (const uint8_t *)&hdr, sizeof(hdr));
    for (uint32_t off = 0U; off < g_param.app_size; off += 4096U)
    {
        uint32_t n = MIN(4096U, g_param.app_size - off);
        memcpy(g_buf, (const void *)(APP_BASE + off), n);
        W25Q64_PageProgram(slot_base + OTA_PKG_HEADER_SIZE + off, g_buf, n);
        Boot_Progress(off, g_param.app_size);
    }
    return true;
}
```

> 备份写在参数区标记之前；备份失败直接中止，旧 App 不动。

### 5.5 跳转序列

```c
typedef void (*pFunc)(void);

void Boot_JumpToApp(void)
{
    uint32_t sp = *(volatile uint32_t *)APP_BASE;
    uint32_t pc = *(volatile uint32_t *)(APP_BASE + 4U);
    pFunc    jump = (pFunc)pc;

    __disable_irq();

    /* 关 SysTick */
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;

    /* 清 NVIC 使能与挂起 */
    for (uint32_t i = 0U; i < 8U; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }

    /* 关掉 Bootloader 用过外设，避免残留动作 */
    SPI_Cmd(SPI1, DISABLE);
    SPI_Cmd(SPI3, DISABLE);
    USART_Cmd(USART2, DISABLE);
    DMA_Cmd(DMA1_Stream5, DISABLE);

    SCB->VTOR = APP_BASE;
    __DSB();
    __ISB();

    __set_MSP(sp);
    __enable_irq();          /* 必须开！否则 App 中断永远不响应 */
    jump();
}
```

> `__enable_irq()` 在 `__set_MSP()` 之后、`jump()` 之前调用。App 的 `Reset_Handler` 会重新配置 FPU、时钟、向量表。

### 5.6 显示与看门狗

**显示**：不复用 `BSP/LCD.c`（它依赖 `Asset.c`/littlefs），单独写 `Boot/Src/boot_lcd.c`：

- 只做 SPI3 + ST7789 初始化、`Fill`、`FillRect`、8×16 ASCII 文字；
- 内置 96 字符字模约 1.5 KB；
- 升级时显示：阶段文字（备份/擦除/写入/校验）+ 进度条 + 百分比；
- 正常启动**不初始化 LCD**，零延迟。

**看门狗**：Phase 1/2 不用 IWDG（App 目前没有喂狗，一旦启用会在 8 秒后复位）。Phase 3 统一：

- IWDG：LSI 32 kHz，prescaler 64，reload 4095 → 约 8.2 s；
- Bootloader 在擦除/写入循环中每 4 KB 喂一次；
- App 在 `ui` 任务主循环喂狗（最长阻塞路径是 AT 事务 10 s，需改成 5 s 超时或分段喂）。

### 5.7 工程配置

**Bootloader 工程**（新建 `Bootloader/STM32F407_Boot.uvprojx`）：

| 配置项 | 值 |
| --- | --- |
| Device | STM32F407VETx |
| IROM1 | Start `0x08000000`，Size `0xC000`（48 KB） |
| IRAM1 | `0x20000000`，`0x20000` |
| Define | `USE_STDPERIPH_DRIVER,STM32F40_41xxx,BOOT_BUILD` |
| 输出 | `Bootloader\Output\STM32F407_Boot.axf` + `.bin` |
| 源文件 | 启动文件、`system_stm32f4xx.c`、SPL（misc/rcc/gpio/spi/usart/flash/pwr/iwdg）、`W25Q64.c`、`Boot/Src/*.c` |

**App 工程改动**：

| 配置项 | 原值 | 新值 |
| --- | --- | --- |
| IROM1 Start | `0x8000000` | `0x8010000` |
| IROM1 Size | `0x80000` | `0x70000` |
| Define | `USE_STDPERIPH_DRIVER,STM32F40_41xxx` | 追加 `APP_BUILD` |
| User → After Build | 无 | `fromelf --bin --output=.\Output\STM32F407.bin .\Output\STM32F407.axf` |

**向量表偏移**：修改 `Core/system_stm32f4xx.c`：

```c
#if defined(BOOT_BUILD)
#define VECT_TAB_OFFSET  0x00000
#elif defined(APP_BUILD)
#define VECT_TAB_OFFSET  0x10000
#else
#define VECT_TAB_OFFSET  0x00000
#endif
```

双保险：App 的 `main()` 开头加一行 `SCB->VTOR = APP_BASE;`。

**烧录顺序**：先烧 Bootloader，再烧 App（Keil 会按 IROM 地址写入）。调试 App 时在 `main` 打断点即可，Bootloader 会在几十毫秒内跳过来。

### 5.8 救援模式（Phase 3，可选）

触发条件：`BOOT_ST_RECOVERY`，或上电时按住 PA0 3 秒。

- 通道：USART2（PA2/PA3，115200），复用调试串口；
- 协议：XMODEM-1K 或自定义"帧头 + 长度 + CRC16"；
- 流程：接收 `.ota` → 写入 slot A → 校验 → 执行 `Boot_Do_Update(slot A)`；
- 代码量约 200 行，Flash 约 2 KB。

> 没有串口工具时，也可以让 App 在正常运行时提供"从 USART2 接收固件"的维护模式，作为救援的替代。两者选其一即可。

---

## 6. App 侧改造

### 6.1 链接与启动

- IROM 改为 `0x08010000 / 0x70000`；
- `VECT_TAB_OFFSET = 0x10000`（`APP_BUILD`）；
- 生成 `.bin`（`fromelf`），供打包工具使用；
- `main()` 开头设置 `SCB->VTOR = APP_BASE;`。

### 6.2 新增模块

```text
Boot/Inc/boot_layout.h        地址宏（双方共用）
Boot/Inc/boot_param.h         参数结构、读写接口（双方共用）
Boot/Src/boot_param.c         日志式参数读写（双方共用）

BSP/Inc/OTA_Transport.h       AT 原始 TCP 传输封装
BSP/Src/OTA_Transport.c       CIPSTART / CIPSEND / CIPRECV 流式收发

User/Application/Inc/OTA.h    OTA 状态机、包解析、写 W25Q64
User/Application/Src/OTA.c
User/Task/Inc/OTA_Task.h      OTA 任务
User/Task/Src/OTA_Task.c
```

### 6.3 AT 传输层改造

**1）扩大 USART1 环形缓冲**

```c
/* BSP/Src/Usart.c */
#define USART1_RX_RING_SIZE  4096      /* 原 512，OTA 期间单次 TCP 段可达 1460 B */
```

RAM 增加 3.5 KB，当前余量 46 KB，足够。

**2）新增 AT 互斥量**

`AT.c` 已有 FreeRTOS 依赖，在 public 函数入口加递归互斥：

```c
void AT_Lock(void);
void AT_Unlock(void);
```

- `Net_Task` 的每次 AT 事务自动互斥；
- OTA 任务在**整个下载会话**开始时 `AT_Lock()`，结束时 `AT_Unlock()`，实现 USART1 独占；
- 最坏等待：Net_Task 正在执行 10 s 的 AT 事务，OTA 等 10 s 后开始。

**3）OTA 流式下载**

```c
/* 建连 */
AT_Send_Command("AT+CIPMUX=0", 2000);
AT_Send_Command("AT+CIPSTART=\"TCP\",\"ota.example.com\",80", 10000);
AT_Send_Command("AT+CIPRECVMODE=1", 2000);      /* 被动接收，适合大块二进制 */

/* 发请求（注意：CIPSEND 后原样发送，不追加 \r\n） */
char req[256];
snprintf(req, sizeof(req),
         "GET /ota/firmware.ota HTTP/1.1\r\n"
         "Host: ota.example.com\r\n"
         "Connection: close\r\n"
         "\r\n");
snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u", (unsigned)strlen(req));
AT_Send_Command(cmd, 2000);                      /* 等 '>' */
OTA_Usart_SendRaw(req, strlen(req));             /* 等 SEND OK */

/* 循环取数据：绕过 AT 的逐行解析 */
while (need > 0U)
{
    snprintf(cmd, sizeof(cmd), "AT+CIPRECVDATA=%u", chunk);  /* chunk <= 2048 */
    OTA_At_SendRaw(cmd);
    OTA_At_Read_CipRecvData(buf, &len, 5000);    /* 解析 "+CIPRECVDATA:<len>,<data>" */
    ...
}
```

**必须新增的底层函数**（`AT_Usart_Wait_Receive` 不能复用）：

```c
int  OTA_At_Read_CipRecvData(uint8_t *buf, uint32_t *len, uint32_t timeout);
bool OTA_Usart_SendRaw(const uint8_t *data, uint32_t len);
void OTA_Usart_RxFlush(void);
```

**兼容性风险**：`AT+CIPRECVMODE` / `AT+CIPRECVDATA` 需在 ESP-AT v4.1.1.0 上实测。若不支持，退回主动模式 `+IPD,<len>:<data>`，解析逻辑略复杂但可行（同样需要绕开逐行解析）。

**速率**：115200 8N1 理论 11.5 KB/s，146 KB 固件约 13 s，含 HTTP/AT 开销约 **20 s**。Phase 3 可把 USART1 提到 460800（STM32 与 ESP32 同时改，`AT+UART_DEF` 持久化），缩短到 5 s 左右。

### 6.4 OTA 任务

| 项目 | 建议值 |
| --- | --- |
| 优先级 | 2（与 net 同级，低于 ui） |
| 栈 | 512 words = 2 KB |
| 静态缓冲 | 4 KB 扇区缓冲 + 2 KB 接收缓冲（不用堆） |
| 触发 | 事件位 `EV_OTA_START` |
| 进度 | 每写 4 KB 置 `EV_OTA_PROGRESS`，`ui` 任务重绘 |
| 结果 | `EV_OTA_DONE` / `EV_OTA_FAIL` |

任务主流程：

```c
void OTA_Task(void *arg)
{
    for (;;)
    {
        xEventGroupWaitBits(Task_Events(), EV_OTA_START, pdTRUE, pdFALSE, portMAX_DELAY);
        OTA_Run();                  /* 下载 → 校验 → 写参数 → 重启 */
        xEventGroupSetBits(Task_Events(), g_ota_ok ? EV_OTA_DONE : EV_OTA_FAIL);
    }
}
```

### 6.5 UI 接入

复用现有 LVGL 配网页，避免新增屏幕：

1. `ui_WiFi.c` 底部加"检查更新"按钮，回调置 `EV_OTA_START`；
2. `UI_Input_HandleEvent()` 收到 `EV_OTA_START`：关闭 LVGL → 切到裸绘 OTA 页（标题、版本、进度条、状态）；
3. `UI_Mode_Tick()` 在 OTA 页每 50 ms 重绘进度条；
4. 成功：显示"升级完成，正在重启"2 s 后 `NVIC_SystemReset()`；
5. 失败：显示错误码，3 s 后回主页面。

约束：

- 夜间/低功耗状态禁止触发 OTA；触发时先退出夜间模式；
- OTA 期间保持屏幕常亮、背光开启；
- 触摸在 OTA 页只响应"取消"（取消时先断开 TCP，再回主页面）。

### 6.6 启动确认

App 启动后，在 `UI_Boot_Sequence()` 结束、`UI_Mode_Init()` 之前确认：

```c
/* 只在新固件首次启动时执行一次 */
if ((Boot_Param_Flags() & BOOT_FLAG_CONFIRMED) == 0U)
{
    Boot_Param_Confirm();       /* state=IDLE, flags|=CONFIRMED */
}
```

不依赖网络，正常启动 3~5 s 内完成，避免"用户快速断电"导致误回滚（`boot_try` 上限 3 次）。

### 6.7 并发与互斥

| 资源 | 现状 | OTA 接入后 |
| --- | --- | --- |
| USART1 | `net` 任务独占 | 新增 AT 递归互斥；OTA 会话期间独占 |
| W25Q64/SPI1 | `Asset` 层 + `LittleFS_Lock()` | OTA 裸写复用同一把锁（改名 `Asset_FlashLock()` 或直接调用 `LittleFS_Lock()`） |
| LCD | `ui` 任务独占 | OTA 任务只置事件，不直接写屏 |
| 内部 Flash | 无 | `Boot_Param_Save()` 内部关中断保护 |
| 低功耗状态 | `g_lowpower` | OTA 开始时清除，禁止进入夜间 |

> 注意：OTA 写 slot A 时，`ui` 任务可能同时读字库/图片。两者都走 SPI1，必须用同一把递归锁串行化，否则字模会读到固件数据。

---

## 7. OTA 端到端流程

### 7.1 正常升级时序

```text
用户点击"检查更新"
  → OTA 任务：AT_Lock，连接服务器，GET /ota/firmware.ota
  → 读 64 B 包头，校验 magic/hw_id/版本（同版本或更低则提示"已是最新"）
  → 擦除 slot A 前 N 个扇区（按 fw_size）
  → 循环：CIPRECVDATA 收 2 KB → 4 KB 缓冲 → 擦/写 W25Q64
  → 每 4 KB 置 EV_OTA_PROGRESS（ui 画进度）
  → 下载完成：校验整包 CRC32 与 Content-Length
  → 写参数：state=OTA_PENDING, ota_size/crc/version, ota_slot=0
  → 回读参数校验
  → 显示"正在重启升级"，延时 1 s，NVIC_SystemReset()
  → Bootloader：OTA_PENDING → 校验 slot A → 备份旧 App 到 slot B
                → 擦 App → 写 App → 校验 → state=APP_UNCONFIRMED, try=0
                → 跳转
  → App 启动 → 3~5 s 后 Boot_Param_Confirm() → state=IDLE, confirmed=1
```

### 7.2 掉电与异常矩阵

| 掉电/异常时机 | 参数区状态 | 下次启动行为 | 结果 |
| --- | --- | --- | --- |
| 下载中 | IDLE | 跳过 OTA，正常启动旧 App | 安全，需重下 |
| 下载完成、写参数前 | IDLE | 正常启动旧 App | 安全 |
| 写参数后、备份中 | OTA_PENDING | 重新校验 slot A → 重新备份 → 搬运 | 自动恢复 |
| 备份完成、擦除 App 前 | OTA_PENDING | 重新备份/搬运 | 自动恢复 |
| 擦除 App 中 | OTA_WRITING | App 无效 → 重新搬运 | 自动恢复 |
| 写 App 中 | OTA_WRITING | 同上 | 自动恢复 |
| 校验失败 | OTA_WRITING | 尝试 slot B 回滚；无备份则 RECOVERY | 可恢复/可救援 |
| 新 App 启动即崩溃（3 次） | APP_UNCONFIRMED, try=3 | ROLLBACK → 从 slot B 恢复 → 跳转 | 自动回滚 |
| 回滚中掉电 | ROLLBACK | 重新从 slot B 恢复 | 自动恢复 |
| slot A、slot B 都非法 | RECOVERY | 串口救援 / 等待 | 需人工 |

**关键点**：擦除 App 之前一定已经完成 slot A 校验和 slot B 备份；因此"App 区残缺"状态总能由 slot A 重写或 slot B 恢复。

### 7.3 回滚

```text
Bootloader 启动：
  state == APP_UNCONFIRMED
    ├─ App 快速检查通过 且 try < 3 → try++，跳转
    ├─ App 快速检查失败 或 try >= 3
    │    ├─ slot B 有效 → state=ROLLBACK → 从 slot B 搬运
    │    └─ slot B 无效 → RECOVERY
    └─ 回滚成功后：state=IDLE, confirmed=1, app_* 取自 slot B 头
```

回滚不覆盖 slot A（保留现场，便于串口/调试分析）。下次正常升级会覆盖 slot A。

---

## 8. 固件包与工具链

### 8.1 `.ota` 包格式

| 偏移 | 长度 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0x00 | 4 | `magic` | `'OTA1'` = `0x4F544131` |
| 0x04 | 2 | `header_size` | 固定 64 |
| 0x06 | 2 | `header_crc16` | 头部（前 60 B）CRC16 |
| 0x08 | 4 | `fw_version` | `major<<16 | minor<<8 | patch` |
| 0x0C | 4 | `fw_size` | 负载字节数 |
| 0x10 | 4 | `fw_crc32` | 负载 CRC32（标准多项式 `0xEDB88320`） |
| 0x14 | 4 | `hw_id` | 板型标识 |
| 0x18 | 4 | `build_time` | Unix 时间戳 |
| 0x1C | 4 | `flags` | 保留 |
| 0x20 | 4 | `min_boot_ver` | 最低 Bootloader 版本（Phase 3） |
| 0x24 | 28 | `reserved` | 预留签名（64 B）时扩展 |
| 0x40 | N | `payload` | App `.bin` 原文 |
| 0x40+N | 4 | `pkg_crc32` | 整个包（含头）CRC32，可选 |

> CRC32 统一用**标准反射算法**（与 PC 端 `zlib.crc32` 一致），不要用 STM32 硬件 CRC（多项式与反射方式不同，PC 端难对齐）。

### 8.2 打包脚本

`Documents/Tools/ota_pack.py`：

```python
#!/usr/bin/env python3
import argparse, struct, zlib, time

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bin")
    ap.add_argument("out")
    ap.add_argument("--version", required=True, help="major.minor.patch")
    ap.add_argument("--hw-id", type=lambda x: int(x, 0), default=0x0407VE00)
    a = ap.parse_args()

    fw = open(a.bin, "rb").read()
    major, minor, patch = (int(x) for x in a.version.split("."))
    ver = (major << 16) | (minor << 8) | patch
    fw_crc = zlib.crc32(fw) & 0xFFFFFFFF

    hdr = struct.pack("<IHHIIIIIII28s",
        0x4F544131, 64, 0, ver, len(fw), fw_crc, a.hw_id,
        int(time.time()), 0, 0, b"\x00" * 28)
    hdr = hdr[:4] + struct.pack("<H", zlib.crc32(hdr[:60]) & 0xFFFF) + hdr[6:]

    body = hdr + fw
    body += struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)
    open(a.out, "wb").write(body)
    print(f"{a.out}: {len(fw)} B, crc32=0x{fw_crc:08X}, ver={a.version}")

if __name__ == "__main__":
    main()
```

用法：

```bat
python Documents\Tools\ota_pack.py MDK\Output\STM32F407.bin firmware.ota --version 1.1.0
```

### 8.3 服务器

- 静态文件服务（Nginx / Caddy / 对象存储均可）；
- 必须返回 `Content-Length`，静态文件默认满足；
- 不要开 `Transfer-Encoding: chunked`（App 端不解析 chunked）；
- Phase 3 断点续传需要 `Accept-Ranges: bytes`（Nginx 默认支持）；
- 目录结构示例：

```text
/ota/
  firmware.ota          # 最新固件
  version.json          # Phase 3 自动检查
  firmware_1.1.0.ota    # 历史版本
```

### 8.4 版本规则

- `major.minor.patch`，打包脚本与 `APP_VERSION` 宏必须同步；
- App 拒绝安装 `fw_version <= 当前版本` 的包（可配置允许降级）；
- `hw_id` 不匹配直接拒绝；
- `min_boot_ver` 大于当前 Bootloader 版本时拒绝（Phase 3）。

`User/Application/Inc/Version.h`：

```c
#define APP_VERSION_MAJOR   1
#define APP_VERSION_MINOR   1
#define APP_VERSION_PATCH   0
#define APP_VERSION         ((APP_VERSION_MAJOR << 16) | \
                             (APP_VERSION_MINOR << 8)  | APP_VERSION_PATCH)
#define APP_VERSION_STR     "1.1.0"
```

---

## 9. 可靠性与安全

| 威胁/故障 | 现有措施 | 后续可加 |
| --- | --- | --- |
| 传输误码 | 包 CRC32 + 长度校验 | — |
| 下载中断 | 参数不写，旧 App 不动 | Phase 3 断点续传 |
| 搬运掉电 | OTA_WRITING 状态重入 | — |
| 新固件崩溃 | confirmed + try 计数 + slot B 回滚 | — |
| Flash 位翻转 | 每次升级完整 CRC | Phase 3 启动完整 CRC |
| 参数区损坏 | 32 条日志 + 单条 CRC + seq | — |
| 刷错板型 | `hw_id` 校验 | — |
| 恶意固件 | 无（HTTP 明文） | Phase 3 HMAC-SHA256 或 HTTPS |
| 降级攻击 | 版本比较 | 强制最低版本 |
| Bootloader 自身损坏 | 无法自恢复 | SWD 重烧；预留双 Bootloader 不现实 |

**明确不做**：本方案不做 A/B 内部 Flash 双分区（512 KB 放不下两份 448 KB），回滚依赖 W25Q64 的 slot B；也不做 Bootloader 自升级（风险收益不匹配）。

---

## 10. 分阶段实施计划

### Phase 0：分区与跳转（1~2 天）

- [ ] 新建 `Boot/` 目录、`boot_layout.h`、`boot_param.h/.c`；
- [ ] 新建 Bootloader Keil 工程，实现参数读取、快速检查、跳转；
- [ ] App 改 IROM/VECT_TAB_OFFSET/APP_BUILD，生成 `.bin`；
- [ ] 修改 `LFS_BLOCK_COUNT = 1536`，重新格式化并烧 3 批资源；
- [ ] 验证：上电直接进 App，串口打印版本，资源自检 43 项通过。

**验收**：App 功能与改造前完全一致；Bootloader 启动到跳转 < 100 ms；`Asset_Init()` 无缺失。

### Phase 1：离线 OTA + 搬运（2~3 天）

- [ ] Bootloader 实现 `Boot_Do_Update()`、备份、CRC、进度显示；
- [ ] App 增加维护模式：USART2 接收 `.ota` → 写 slot A → 写参数 → 重启；
- [ ] 验证搬运、掉电重入、CRC 失败、slot B 回滚；
- [ ] 不接网络、不做 UI，用串口脚本驱动。

**验收**：串口发送 146 KB 包，30 s 内完成"下载→重启→搬运→新版本运行"；搬运中拔电 10 次均能恢复。

### Phase 2：网络 OTA + UI（3~5 天）

- [ ] USART1 环形缓冲 512→4096，AT 互斥量；
- [ ] `OTA_Transport.c`：CIPSTART/CIPSEND/CIPRECVDATA；
- [ ] `OTA.c`：包头解析、流式写 slot A、进度、失败重试；
- [ ] `OTA_Task.c` + 事件位；
- [ ] WiFi 页"检查更新"按钮 + 裸绘 OTA 页；
- [ ] 夜间禁止、低功耗互斥。

**验收**：真机 WiFi 下载 146 KB 约 20 s；断网/服务器 404/包损坏均有明确提示；升级后版本号更新。

### Phase 3：回滚加固与运维（3~5 天）

- [ ] 统一 IWDG（Bootloader + App 喂狗）；
- [ ] 回滚流程完整测试（人为让新 App 启动即复位）；
- [ ] 断点续传（OTA 元数据区 + HTTP Range）；
- [ ] 串口救援模式（XMODEM）；
- [ ] 可选：USART1 提速 460800、HTTPS/HMAC、自动检查更新。

**验收**：连续 20 次升级/回滚无变砖；看门狗不误触发；断点续传能从 50% 继续。

---

## 11. 改动文件清单

| 文件 | 动作 | 说明 |
| --- | --- | --- |
| `Boot/Inc/boot_layout.h` | 新增 | 地址宏，双方共用 |
| `Boot/Inc/boot_param.h` | 新增 | 参数结构、读写接口 |
| `Boot/Src/boot_param.c` | 新增 | 日志式参数读写、CRC |
| `Boot/Src/boot_main.c` | 新增 | Bootloader 入口与状态机 |
| `Boot/Src/boot_update.c` | 新增 | 搬运、备份、回滚 |
| `Boot/Src/boot_lcd.c` | 新增 | 精简 ST7789 驱动 + 8×16 字模 |
| `Boot/Src/boot_crc32.c` | 新增 | 标准 CRC32（查表） |
| `Bootloader/STM32F407_Boot.uvprojx` | 新增 | Bootloader 工程 |
| `Core/system_stm32f4xx.c` | 修改 | `VECT_TAB_OFFSET` 条件编译 |
| `MDK/STM32F407.uvprojx` | 修改 | IROM、Define、fromelf |
| `Middlewares/LittleFS/Inc/lfs_config.h` | 修改 | `LFS_BLOCK_COUNT = 1536` |
| `BSP/Src/Usart.c` | 修改 | USART1 环形缓冲 4096 |
| `BSP/Src/AT.c` / `AT.h` | 修改 | AT 递归互斥 |
| `BSP/Inc/OTA_Transport.h` | 新增 | 传输层接口 |
| `BSP/Src/OTA_Transport.c` | 新增 | CIPSTART/CIPSEND/CIPRECV |
| `User/Application/Inc/OTA.h` | 新增 | OTA 状态与接口 |
| `User/Application/Src/OTA.c` | 新增 | 下载、写 W25Q64、参数 |
| `User/Application/Inc/Version.h` | 新增 | 版本宏 |
| `User/Task/Inc/OTA_Task.h` | 新增 | 任务接口 |
| `User/Task/Src/OTA_Task.c` | 新增 | OTA 任务 |
| `User/Task/Inc/Task_Common.h` | 修改 | 新增 OTA 事件位 |
| `User/Task/Src/Net_Task.c` | 修改 | OTA 期间暂停 |
| `User/UI/Src/screens/ui_WiFi.c` | 修改 | "检查更新"按钮 |
| `User/UI/Src/UI_Input.c` / `.h` | 修改 | OTA 入口与页面切换 |
| `User/UI/Src/UI_Mode.c` | 修改 | OTA 进度页 |
| `User/Task/Src/UI_Task.c` | 修改 | 启动确认、OTA 事件 |
| `Documents/Tools/ota_pack.py` | 新增 | 打包脚本 |
| `Documents/README.md` | 修改 | 补充启动链、OTA、烧录顺序 |

---

## 12. 风险与备选

| 风险 | 影响 | 应对 |
| --- | --- | --- |
| littlefs 缩容需重烧资源 | 一次停机约 30 min | 提供 `PROVISION_BATCH` 脚本化烧录；先备份 `/data` |
| ESP-AT 不支持 `CIPRECVMODE` | 网络 OTA 受阻 | 退回 `+IPD` 解析；或改用 ESP32 透传模式 |
| 115200 下载慢（20 s） | 体验一般 | Phase 3 提速 460800；资源与固件分离，固件只有 146 KB |
| App 擦参数区时停顿 250 ms | 丢 tick / 串口溢出 | 只在 OTA 完成时擦除；日志式设计让正常路径不擦除 |
| 回滚占用 512 KB | W25Q64 空间减少 | 6 MB 文件系统仍余 5.1 MB |
| Bootloader 与 App 共享 `system_stm32f4xx.c` | 改错导致向量表错位 | 用 `BOOT_BUILD`/`APP_BUILD` 条件编译 + 编译期断言 |
| 调试时擦 App 导致调试器断开 | 开发不便 | 调试用 ST-Link 直接烧 App；OTA 测试单独进行 |
| 用户随意断电 | 误触发回滚 | 确认提前到 3~5 s；try 上限 3 |

**备选方案（不推荐）**：把新固件放在 littlefs 的 `/data/ota.bin`，Bootloader 链接 littlefs 读取。好处是不用重烧资源；坏处是 Bootloader 增加 15 KB 代码 + 8 KB RAM，且与文件系统磨损/一致性强耦合。仅在"绝对不能重烧资源"时采用。

---

## 13. 待确认问题

在进入 Phase 0 之前，需要确认以下取舍：

1. **OTA 服务器**：用公网对象存储、自建 Nginx，还是局域网 PC？是否需要 HTTPS？
2. **回滚策略**：是否接受 slot B 备份（每次升级多 2 s + 占 512 KB），还是先不做回滚、只做"校验后擦写"？
3. **资源重烧**：能否接受一次性重新格式化 W25Q64 并重烧 3 批资源（约 30 min）？
4. **救援通道**：是否需要串口救援（XMODEM）？如果产品外壳没有引出 PA2/PA3，可省。
5. **看门狗**：是否允许给 App 全面加 IWDG（需要调整 AT 事务超时与喂狗点）？
6. **提速**：是否要把 USART1 从 115200 提到 460800（ESP32 需同步改并持久化）？
7. **版本显示**：主页面是否要显示当前版本号（建议显示，便于售后确认）？

> 建议先按推荐方案推进 Phase 0/1：回滚做、HTTPS 暂不做、串口救援留到 Phase 3、看门狗暂缓。这样最快验证核心链路，且不阻塞后续。
