# Bootloader 与 OTA 学习指南

> 基于本项目（STM32F407VET6 桌面天气时钟）的实战讲解
> 面向第一次接触 Bootloader / OTA 的开发者
> 配套代码：`Boot/`、`Bootloader/`、`User/Application/Src/app_task.c`

---

## 0. 这份文档怎么用

`Documents/` 下有三份互补的文档，按"想解决什么问题"来选：

| 文档 | 定位 | 什么时候看 |
| --- | --- | --- |
| **Bootloader_OTA_Flow.md** | 现状流程（12 章 + 7 张流程图） | 想快速搞清"现在到底怎么跑的" |
| **Bootloader_OTA_Guide.md**（本文） | 原理、设计取舍、踩坑历史 | 想理解"为什么要这么设计" |
| **OTA_Test_Record.md** | Phase 0/1/2 的测试方法与实测记录 | 想复现测试，或查某个 bug 的来龙去脉 |

代码里只保留必要的短注释，完整的原理、设计取舍和踩坑点集中在这里。本文建议按下面的顺序读：

1. 先读第 1~3 章，建立“Bootloader 到底在干什么”的整体图景；
2. 再读第 4~7 章，理解 Flash、向量表、跳转、参数区这些底层机制；
3. 然后读第 8~11 章，看 OTA、回滚、救援是怎么设计出来的；
4. 最后读第 12~15 章，对照本项目代码、踩坑清单和动手实验。

不需要一次读完。第一次读不懂“向量表”“MSP”很正常，先把第 1 章和生活类比记住，后面会反复回到这些概念。

---

## 1. 先用一句话说清楚

**Bootloader 是芯片上电后先运行的一小段程序，它不负责业务，只负责决定“接下来运行哪份应用固件”，并在需要时把新固件写进 Flash。**

**OTA（Over-The-Air）是不拆机、不接调试器，通过网络把新固件送到设备上并完成升级的过程。**

两者关系：

```text
OTA 负责“把新固件弄到设备里”
Bootloader 负责“把新固件真正写进运行区，并保证写坏了还能恢复”
```

生活里的类比：

| 概念 | 类比 |
| --- | --- |
| Bootloader | 电脑的 BIOS / 手机的 bootloader |
| App | Windows / Android 系统 |
| 参数区 | BIOS 里保存的启动项设置 |
| OTA | 手机的系统更新 |
| 回滚 | 系统更新失败后自动恢复上一个版本 |
| 串口救援 | 电脑开不了机时用 U 盘重装 |

一个常见的误解：**Bootloader 不是 App 里的一个函数，也不是一个开机画面。** 它必须是独立编译、独立烧录、独立运行的一段固件，位于 App 之前。

---

## 2. 没有 Bootloader 时，STM32 是怎么启动的

要理解 Bootloader，先要理解“正常启动”。以本项目的 App 为例（假设它还烧在 `0x08000000`）：

### 2.1 上电后的前几条指令

Cortex-M4 复位后，硬件会自动做两件事：

1. 从地址 `0x08000000` 读第一个 32 位字，作为**初始栈顶 MSP**；
2. 从地址 `0x08000004` 读第二个 32 位字，作为**复位处理函数地址**，然后跳过去执行。

这两个字所在的表，就叫**中断向量表**（vector table）。

```text
0x08000000: 0x20020000   ← 栈顶（MSP 初值）
0x08000004: 0x0800019D   ← Reset_Handler 地址（bit0=1 表示 Thumb）
0x08000008: 0x0800xxxx   ← NMI_Handler
0x0800000C: 0x0800xxxx   ← HardFault_Handler
...
```

所以“向量表”不是一张普通的函数表，它是**硬件在异常/中断发生时用来找入口的地址表**。CPU 不认函数名，只认这张表里的地址。

### 2.2 Reset_Handler → SystemInit → __main → main

`Reset_Handler` 是启动文件（`startup_stm32f40xx.s`）里的汇编入口，它大致做这些事：

1. 调用 `SystemInit()`：配置时钟（本项目 168 MHz）、Flash 等待周期、向量表基址；
2. 调用 `__main`：C 库的初始化，把 RW 数据从 Flash 拷到 RAM、把 ZI 段清零；
3. 跳到 `main()`。

`__main` 这一步很关键：**全局变量和静态变量的初值原本存在 Flash 里，必须搬到 RAM 才能使用。** 这也是为什么一个 `.bin` 文件不只包含代码，还包含 RW 初值。

### 2.3 `SystemInit()` 里的一个坑

本项目 `Core/system_stm32f4xx.c` 里有这样一行：

```c
#define VECT_TAB_OFFSET  0x00
...
SCB->VTOR = FLASH_BASE | VECT_TAB_OFFSET;
```

它的意思是：**把向量表基址设成 `0x08000000`**。

当 App 被链接到 `0x08010000` 后，这一行会把 Bootloader 刚设好的 `VTOR` 又改回 `0x08000000`。所以 App 必须在 `main()` 早期再改一次，这就是 `app_task.c` 里 `Boot_Apply_VectorTable()` 存在的原因。

> 更规范的做法是用 `APP_BUILD` 条件编译改 `VECT_TAB_OFFSET`，但那个文件是 GBK 编码，本项目暂时用 `app_task.c` 修正，避免破坏编码。

### 2.4 三个必须记住的寄存器/概念

| 名字 | 作用 | 一句话理解 |
| --- | --- | --- |
| `MSP` | 主栈指针 | 当前用的是哪块栈内存 |
| `VTOR` | 向量表基址 | 中断发生时去哪里找入口 |
| `SCB->VTOR` | 同上，寄存器全名 | 改它 = 换一张中断入口表 |

**跳转固件时，这两个都要换。** 只换 PC 不换 MSP/VTOR，是初学者最常见的错误。

---

## 3. 为什么单固件不能自己升级自己

假设只有一个 App，烧在 `0x08000000`，现在想通过网络把它自己更新掉。会遇到什么问题？

1. **擦除 App 区 = 擦掉正在运行的代码。** CPU 正在从 `0x08000000` 取指，你把它擦了，下一条指令就没了。
2. **写新固件时不能覆盖自己。** 新固件还没写完，旧固件已经被破坏，中间任何一步失败都会变砖。
3. **没有“失败后还能运行”的代码。** 一旦升级失败，设备连恢复的能力都没有。

所以必须有一段**不在被擦区域内运行**的代码，它负责：

- 判断有没有新固件；
- 校验新固件是否完整；
- 擦除并写入 App 区；
- 写失败时恢复旧版本；
- 最后跳转到 App。

这段代码就是 Bootloader。它自己占据 Flash 最前面的区域，升级时永远不动它，所以叫“刷不死的底座”。

---

## 4. Flash 的物理约束

很多 Bootloader 设计问题，根源不是代码，而是 Flash 的物理特性。

### 4.1 STM32F407 的扇区不是等大的

| 扇区 | 起始地址 | 大小 |
| --- | --- | --- |
| Sector 0 | `0x08000000` | 16 KB |
| Sector 1 | `0x08004000` | 16 KB |
| Sector 2 | `0x08008000` | 16 KB |
| Sector 3 | `0x0800C000` | 16 KB |
| Sector 4 | `0x08010000` | 64 KB |
| Sector 5 | `0x08020000` | 128 KB |
| Sector 6 | `0x08040000` | 128 KB |
| Sector 7 | `0x08060000` | 128 KB |

合计 512 KB。注意 Sector 4 突然变成 64 KB，Sector 5 又变成 128 KB，这就是为什么分区不能随便按“整数 MB”切。

### 4.2 三条硬规则

1. **只能把 1 写成 0，不能把 0 写回 1。** 所以想重新写一个地址，必须先擦除。
2. **擦除的最小单位是扇区**，不是字节。想改 4 个字节，也可能要擦 16 KB。
3. **擦除后是 0xFF，不是 0x00。** 判断“空槽”要用 `0xFFFFFFFF`，不是 `0`。

### 4.3 写入寿命

W25Q64 的擦写寿命约 10 万次，STM32 内部 Flash 约 1 万次。听起来很多，但如果每次开机都写一次参数，几年也会写坏。所以：

- 参数区用**日志式追加**，写满 32 条才擦一次；
- App 确认只在“新固件第一次启动”时写一次，已确认就不再写；
- 不要在主循环里周期性写 Flash。

### 4.4 单 Bank 擦写时 CPU 会停顿

F407 是单 Bank Flash。擦除/编程期间，CPU 从 Flash 取指会被总线暂停。表现为：

- 擦 16 KB 约 200~300 ms；
- 这期间中断响应会被延迟，串口可能丢数据；
- 所以参数写入要关中断，且尽量放在“没有实时任务”的时机。

这也是为什么 OTA 的“擦 App 区”放在 Bootloader 里做，而不是在 App 运行时做——Bootloader 没有实时任务，停顿无所谓。

---

## 5. 本项目为什么这样分区

```text
0x08000000 +  48 KB  Bootloader   (Sector 0-2)
0x0800C000 +  16 KB  参数区       (Sector 3)
0x08010000 + 448 KB  App          (Sector 4-7)
```

### 5.1 为什么 Bootloader 是 48 KB

- 必须从 Sector 0 开始；
- 不能和参数区共用扇区（擦参数区不能把 Bootloader 擦了）；
- Sector 0-3 各 16 KB，所以 Bootloader 取 3 个扇区 = 48 KB，参数区取第 4 个 = 16 KB；
- 实测当前 Bootloader 只有 2.5 KB，48 KB 给后续加只读 littlefs、LCD、救援留足余量。

### 5.2 为什么 App 是 448 KB

从 Sector 4 开始到 Sector 7 结束：

```text
64 KB + 128 KB + 128 KB + 128 KB = 448 KB
```

当前 App 约 147 KB，余量约 300 KB，足够放 OTA 模块和后续功能。

### 5.3 为什么参数区放内部 Flash，不放 W25Q64

参数区要回答一个生死问题：**“App 还能不能跳？”**

如果参数区放在 W25Q64，而 W25Q64 虚焊、损坏、SPI 接触不良，Bootloader 就不知道 App 是否有效，可能拒绝启动。放在内部 Flash，即使外部 Flash 完全坏掉，设备仍能正常启动已有 App。

### 5.4 地址宏与 Keil 配置

`Boot/Inc/boot_layout.h` 里定义了所有地址。App 工程的 Keil 配置必须同步：

| 配置项 | 值 |
| --- | --- |
| IROM1 Start | `0x08010000` |
| IROM1 Size | `0x70000`（448 KB） |
| Define | 追加 `APP_BUILD` |
| After Build | `fromelf --bin --output=...` |

Bootloader 工程则是 `0x08000000 / 0xC000`。

**`.bin` 是怎么来的？** Keil 默认只生成 `.axf`（带调试信息的 ELF）。`.bin` 是纯二进制，OTA 下载和烧录都需要它，所以要加 fromelf 命令。本项目两个工程都已经配好。

---

## 6. Bootloader 的启动流程

当前 MVP 版 `Boot/Src/boot_main.c` 的流程：

```text
main()
  ├─ Boot_Uart_Init()          串口日志
  ├─ Boot_Param_Load()         读参数区
  ├─ App_QuickValid()          检查 0x08010000 是否像合法固件
  │    ├─ 无效 → 打印后死循环（将来进救援模式）
  │    └─ 有效 → JumpToApp()
  └─ JumpToApp()
```

### 6.1 快速校验检查了什么

`App_QuickValid()` 只检查三件事：

1. **栈顶**：`*(uint32_t*)0x08010000` 必须落在 SRAM（`0x20000000~0x2001FFFF`）或 CCM（`0x10000000~0x1000FFFF`）范围内；
2. **复位向量**：`*(uint32_t*)0x08010004` 必须落在 App 区内，且 bit0 = 1（Thumb）；
3. **前 8 个字不能全是 `0xFFFFFFFF`**：全 FF 说明这块从没烧过。

为什么不做整包 CRC？因为正常启动要在几十毫秒内完成，扫 147 KB 做 CRC 会明显变慢。整包 CRC 留给“升级搬运之后”和“参数里记录了 CRC”时再做。

### 6.2 跳转序列为什么是这个顺序

`JumpToApp()` 的每一步都有原因，顺序不能乱：

```c
__disable_irq();              // 1. 关全局中断
SysTick->CTRL = 0;            // 2. 关 SysTick
NVIC->ICER[i] = 0xFFFFFFFF;   // 3. 关所有外部中断
NVIC->ICPR[i] = 0xFFFFFFFF;   // 4. 清所有挂起
USART_Cmd(USART2, DISABLE);   // 5. 关 Bootloader 用过的外设
SCB->VTOR = APP_BASE;         // 6. 向量表指向 App
__DSB(); __ISB();             // 7. 等写入生效、清流水线
__set_MSP(sp);                // 8. 切换到 App 的栈
__enable_irq();               // 9. 必须重新开中断
jump();                       // 10. 跳到 App 的 Reset_Handler
```

逐条解释：

- **1~4**：如果不清，跳过去后某个外设中断可能打到 Bootloader 的旧向量表，或者 App 还没初始化完就被中断打断。
- **5**：Bootloader 用过的 USART2 如果不关，App 重新初始化前可能残留状态。
- **6**：不改 VTOR，App 的中断会去 `0x08000000` 找入口——那是 Bootloader 的向量表，函数对不上。
- **7**：`DSB` 确保 VTOR 写入真正完成，`ISB` 让流水线重新取指。
- **8**：`__set_MSP` 之后，Bootloader 的局部变量和返回地址都不再有效。**这一条之后不能再依赖栈上的任何东西。**
- **9**：最容易忘！Bootloader 关了 PRIMASK，不恢复的话，App 启动后所有中断被屏蔽，FreeRTOS 的 SysTick 起不来，系统看起来像死机。
- **10**：`jump()` 是函数指针调用，跳到 App 向量表的第二个字（Reset_Handler）。

### 6.3 一个常见疑问：为什么不直接 `NVIC_SystemReset()`

`NVIC_SystemReset()` 会让芯片重新复位，又回到 `0x08000000` 的 Bootloader，形成循环。跳转是“不复位、直接换执行流”，两者完全不同。

---

## 7. 参数区：掉电安全的艺术

### 7.1 为什么不能“直接写一个变量”

升级过程中随时可能掉电。假设参数区只有一个结构体，每次原地覆盖：

- 写到一半掉电 → 结构体一半新、一半旧 → 下次启动读到垃圾；
- 无法判断“这条记录是否完整”。

所以需要一种**任何时刻掉电都能恢复到某个已知状态**的存储方式。

### 7.2 日志式追加 + seq + CRC

本项目把 Sector 3（16 KB）切成 32 个槽，每槽 512 B，每条记录 64 B：

```text
槽0: [magic|seq=1|state|...|crc]   ← 有效
槽1: [magic|seq=2|state|...|crc]   ← 有效
槽2: [0xFF ...]                    ← 空
...
```

每次保存：

1. 找第一个首字为 `0xFFFFFFFF` 的空槽；
2. 写入新记录，`seq = 上一条 seq + 1`；
3. 计算 CRC 并写入；
4. 回读比较。

读取时：

1. 扫描全部 32 条；
2. 过滤 magic、layout、CRC；
3. 取 `seq` 最大的一条。

这样即使最后一条写到一半掉电，它的 CRC 对不上，读取时会自动回退到上一条完整记录。

### 7.3 为什么读取要扫全部 32 条

因为掉电可能留下“magic 对、CRC 错”的残记录。如果“找到第一条就返回”，可能拿到一条旧的、seq 更小的记录。扫全部、取最大 seq，才是可靠的。

### 7.4 写满 32 条怎么办

擦除整个 Sector 3，从槽 0 重新开始。擦除前会关中断，耗时 200~300 ms。

正常使用下，这个分支很少走到：

- 每次升级写 2~3 条；
- 新固件首次启动确认写 1 条；
- 32 条够升级很多次。

### 7.5 掉电矩阵

| 掉电时机 | 参数区状态 | 下次启动 |
| --- | --- | --- |
| 写记录前 | 旧记录完好 | 读旧记录，正常 |
| 写记录中 | 新记录 CRC 错 | 回退到上一条 |
| 擦除中 | 全部 0xFF | 参数区为空，按默认启动 |
| 擦除后、写新记录前 | 全部 0xFF | 参数区为空，按默认启动 |

**关键点**：擦除是“先擦后写”，擦除后到写新记录之间掉电，参数区会变成空。所以设计上要保证“参数区为空”也是一个合法状态，Bootloader 能按默认逻辑启动（检查 App 是否有效）。

---

## 8. OTA 的完整链路（规划）

当前 MVP 只做了“跳转”，真正的 OTA 还在规划中。完整链路如下：

### 8.1 固件包格式

不能直接下载 `.bin`，因为需要携带版本、大小、CRC 等信息。设计一个 64 B 头：

```text
+0x00  magic 'OTA1'
+0x04  header_size
+0x06  header_crc16
+0x08  fw_version
+0x0C  fw_size
+0x10  fw_crc32
+0x14  hw_id
+0x18  build_time
+0x1C  flags
+0x20  min_boot_ver
+0x24  reserved[28]
+0x40  payload（App 的 .bin）
+...   整包 CRC32
```

### 8.2 从 GitHub Releases 下载

本项目的仓库是 `sshzfy/WiFi_Clock`，Release 资产命名为 `firmware.ota`。

直接请求：

```http
GET /sshzfy/WiFi_Clock/releases/latest/download/firmware.ota HTTP/1.1
Host: github.com
User-Agent: STM32F407-OTA/1.0
```

会经历两次 302：

```text
github.com/releases/latest/download/firmware.ota
  → 302 github.com/releases/download/v1.1.0/firmware.ota
  → 302 objects.githubusercontent.com/...（签名 URL）
  → 200 + Content-Length
```

ESP-AT 的 `AT+CIPSTART` **不会自动跟随重定向**，所以 App 必须自己解析 `Location` 并重新连接。

几个必须注意的点：

- **必须带 `User-Agent`**，否则 GitHub 返回 403；
- **`Location` 可能长达 700~900 B**，缓冲要 2 KB；里面的 `%3B`、`%2F` 要原样保留，不要 URL 解码；
- **不要发 `Accept-Encoding: gzip`**，否则 body 被压缩；
- 最多跟随 3 跳；
- 最后用 `Content-Length` 校验包大小。

### 8.3 为什么用 HTTPS（`AT+CIPSTART="SSL"`）

GitHub 只支持 HTTPS。ESP-AT 的 SSL 连接：

```text
AT+CIPMUX=0
AT+CIPSTART="SSL","github.com",443
```

证书验证默认关闭，安全性靠包内 CRC 兜底。如果以后要防篡改，可以加 HMAC 或签名。

### 8.4 写入 littlefs 用 tmp + rename

下载过程中，文件可能写一半。如果 Bootloader 此时读到它，会误判。

所以：

```text
下载 → /ota/firmware.tmp
校验通过 → lfs_rename 为 /ota/firmware.bin
写参数 state=OTA_PENDING
重启
```

`rename` 在 littlefs 里是原子的，要么是旧文件，要么是新文件，不会出现“半个固件”。

### 8.5 备份与回滚

升级前，App 把当前固件备份成 `/ota/backup.bin`（同样带 64 B 头）。备份完成后再写参数。

为什么由 App 做备份，而不是 Bootloader？因为本方案 Bootloader 只读挂载 littlefs（`LFS_READONLY`），不能写文件。App 有完整读写权限。

### 8.6 完整时序

```text
App：检查更新 → 下载 → 校验 → 备份 → 写参数 → 重启
Bootloader：读参数 → 校验 firmware.bin → 擦 App → 搬运 → 校验
          → state=APP_UNCONFIRMED → 跳转
App：启动 5 秒 → 写 confirmed
下一次启动：Bootloader 看到 confirmed，不再回滚
```

---

## 9. littlefs 与 W25Q64

### 9.1 为什么不能随便改 block_count

littlefs 的 superblock 里记录了 `block_count`。如果只改 `lfs_config.h` 的 `LFS_BLOCK_COUNT` 而不重新格式化，挂载时会发现“配置和磁盘不一致”，直接失败或要求格式化。

而格式化会清空所有资源（858 KB 字库/图片），需要重新烧录。所以本项目当前**不改 littlefs 布局**，OTA 文件作为普通文件放进去。

### 9.2 逻辑分区

不能做物理裸分区，但可以在 littlefs 里用目录隔离：

```text
/font    只读字库
/img     只读图片
/data    运行配置
/ota     固件缓存与备份
```

路径宏统一放在 `Boot/Inc/boot_layout.h`。启动自检时检查 `/ota` 目录、剩余空间，并清理 `*.tmp`。

### 9.3 磨损均衡与掉电一致性

littlefs 是 log-structured 文件系统：

- 写文件不是原地覆盖，而是分配新块、写新数据、更新元数据；
- 元数据用 copy-on-write，掉电时要么旧状态、要么新状态；
- 全片参与磨损均衡，所以物理地址不固定。

这带来两个好处：掉电不容易把文件系统写坏；坏块/磨损分散。代价是**不能假设某个文件固定在某个地址**。

### 9.4 写入性能

W25Q64 一页 256 B，编程约 0.7 ms；一个 4 KB 扇区擦除约 45 ms。146 KB 固件约 36 个扇区，写入总耗时约 2~3 s。和 115200 串口下载的 20 s 相比，不是瓶颈。

---

## 10. 回滚设计

### 10.1 为什么需要回滚

新固件可能：

- CRC 正确，但代码有 bug，启动就崩溃；
- 能启动，但某个外设初始化失败，反复复位；
- 屏幕不亮、WiFi 连不上，用户没法用。

这些情况 CRC 都发现不了，只能靠“运行起来后确认”。

### 10.2 confirmed + boot_try

```text
Bootloader 写入新 App
  → state = APP_UNCONFIRMED
  → 每次跳转前 boot_try++
App 启动 5 秒
  → 写 CONFIRMED
下次启动
  → 看到 CONFIRMED，不再回滚
```

如果 App 启动就崩溃：

```text
第 1 次启动：boot_try=1，崩溃
第 2 次启动：boot_try=2，崩溃
第 3 次启动：boot_try=3，仍未确认
  → Bootloader 判定失败
  → 从 /ota/backup.bin 恢复旧版本
```

### 10.3 为什么是 3 次

- 1 次太激进：偶发的上电时序问题可能误判；
- 次数太多：用户要反复看着设备重启；
- 3 次是常见折中。

### 10.4 回滚失败怎么办

如果 `backup.bin` 也损坏，或者 littlefs 挂载失败，就进入 `RECOVERY`：

- 等待串口救援；
- 或者保持停止，避免反复重启。

**永远不要在没有可用固件的情况下盲目擦除。**

---

## 11. 看门狗与救援

### 11.1 IWDG 的特点

STM32 的独立看门狗（IWDG）由内部 LSI 驱动，**一旦启动就不能关闭**，只能复位或继续喂狗。

这意味着：

- 如果 Bootloader 启动 IWDG，App 必须也喂狗，否则 8 秒后复位；
- 不能“Bootloader 开、App 不开”；
- 所以 IWDG 要么全系统统一加，要么先不加。

当前 MVP 没有启用 IWDG。Phase 3 会统一加，并在 Bootloader 的擦写循环、App 的 UI 任务里喂狗。

### 11.2 串口救援

当 App 和 backup 都不可用时，最后的恢复手段是串口。本项目的 USART2（PA2/PA3）已经引出，115200。

自定义帧协议：

```text
0x55 0xAA | seq(2) | len(2) | payload(len) | CRC16(2)
```

流程：

1. PC 发送元数据帧（size、crc32、version、hw_id）；
2. Bootloader 擦除 App 区；
3. PC 逐 4 KB 发送数据帧；
4. Bootloader 边收边写；
5. 最后校验 CRC，写参数，跳转。

为什么不用 XMODEM？XMODEM 是通用协议，但和 `.ota` 包头拼接麻烦，也无法携带版本/硬件 ID。自定义协议配一个 Python 脚本更可控。

---

## 12. 本项目的代码地图

### 12.1 文件职责

| 文件 | 职责 |
| --- | --- |
| `Boot/Inc/boot_layout.h` | 内部分区、OTA 逻辑路径 |
| `Boot/Inc/boot_param.h` | 参数结构、状态、标志 |
| `Boot/Src/boot_param.c` | 日志式参数读写、CRC |
| `Boot/Src/boot_crc32.c` | 标准 CRC32 |
| `Boot/Src/boot_uart.c` | Bootloader 轮询串口 |
| `Boot/Src/boot_main.c` | 启动、校验、跳转 |
| `Bootloader/STM32F407_Boot.uvprojx` | Bootloader 工程 |
| `User/Application/Src/app_task.c` | App 侧 VTOR 修正、确认任务 |

### 12.2 两个工程的分工

| | Bootloader | App |
| --- | --- | --- |
| 地址 | `0x08000000`，48 KB | `0x08010000`，448 KB |
| 依赖 | 启动文件 + SPL | FreeRTOS + littlefs + LVGL |
| 产物 | `STM32F407_Boot.bin`（2.5 KB） | `STM32F407.bin`（约 147 KB） |
| 烧录顺序 | 先 | 后 |

两个工程都编译 `boot_param.c` 和 `boot_crc32.c`，因为它们要读写同一套参数格式。

### 12.3 当前实现与规划

| 功能 | 状态 |
| --- | --- |
| 内部分区、Bootloader 跳转 | 已实现，编译通过 |
| 参数区日志读写 | 已实现 |
| App 启动确认 | 已实现（5 秒后，并写入 size/crc/version） |
| 只读 littlefs（`LFS_READONLY`） | 已实现 |
| 搬运状态机（升级/回滚/重入） | 已实现 |
| 串口离线 OTA（USART2 + 自定义协议） | 已实现，待上机验证 |
| 备份 / 回滚（`backup.bin` + confirmed + try） | 已实现，待上机验证 |
| Gitee 自动发现下载（`releases/latest` 推导 tag） | 已实现，实测通过 |
| 看门狗 / 串口救援 | 规划中（Phase 3） |

---

## 13. 常见坑清单

| # | 现象 | 原因 | 解决 |
| --- | --- | --- | --- |
| 1 | 跳转后 App 不运行 | App 的 IROM 没改到 `0x08010000` | 检查 Keil Target → IROM1 |
| 2 | App 能跑但中断异常 | VTOR 没指向 App | `app_task.c` 的 `Boot_Apply_VectorTable()` |
| 3 | 跳转后死机 | 没设 MSP / 没开中断 | 按 `JumpToApp()` 顺序检查 |
| 4 | HardFault | 跳到了偶地址（bit0=0） | 检查复位向量 bit0 |
| 5 | 参数区读不到 | 没擦除，或 magic 不对 | 全片擦除后重试 |
| 6 | 参数区写一次就坏 | 没按 4 字节对齐 / 没回读 | 检查 `FLASH_ProgramWord` 地址 |
| 7 | 升级后变砖 | 擦 App 前没校验新固件 | 先校验再擦 |
| 8 | 升级后反复重启 | 没有 confirmed/try 机制 | 加回滚 |
| 9 | CRC 对不上 | PC 用 zlib，MCU 用硬件 CRC | 统一标准反射 CRC32 |
| 10 | littlefs 挂载失败 | 改了 block_count 没格式化 | 恢复原值，或重新格式化 |
| 11 | OTA 文件读到一半 | 没做 tmp + rename | 下载完再改名 |
| 12 | 升级时断电变砖 | 没有备份/重入状态 | `OTA_WRITING` 状态 + backup |
| 13 | GitHub 返回 403 | 没带 `User-Agent` | 加请求头 |
| 14 | 下载到一半断开 | 没处理 302 / `Content-Length` | 解析 Location，校验长度 |
| 15 | ESP-AT 收不到数据 | 用了逐行解析 | 用 `CIPRECV` 原始二进制 |
| 16 | 串口接收溢出 | USART1 环形缓冲只有 512 B | 扩到 4 KB |
| 17 | 看门狗误复位 | 只给 Bootloader 开了 IWDG | App 也要喂狗 |
| 18 | 版本号对不上 | Release tag、打包参数、`Version.h` 三处不同步 | 打包脚本统一读取 |
| 19 | 烧录后旧 App 还在跑 | 只烧了 Bootloader 没烧 App | 按顺序烧录 |
| 20 | 调试时程序跑飞 | 调试器从 `0x08000000` 复位，先进 Bootloader | 在 App 的 `main` 打断点，或关闭“Run to main” |

---

## 14. 术语表

| 术语 | 解释 |
| --- | --- |
| Bootloader | 上电先运行的引导程序，负责校验/搬运/跳转 |
| App | 业务应用固件 |
| 向量表 | 存放栈顶和中断入口地址的表，位于固件起始处 |
| MSP | 主栈指针 |
| VTOR | 向量表基址寄存器 |
| 分区 | 把 Flash 切成 Bootloader/参数/App 等区域 |
| 参数区 | 记录升级状态、版本、CRC 的掉电保持区域 |
| 日志式 | 顺序追加记录，写满再擦，掉电可回退 |
| CRC32 | 循环冗余校验，用来判断数据是否完整 |
| OTA | 通过无线/网络升级固件 |
| A/B 分区 | 两套 App 区交替升级，失败可切回 |
| 回滚 | 新固件失败后恢复旧版本 |
| confirmed | App 标记“我已正常启动”的标志 |
| boot_try | 新 App 尝试启动的次数 |
| 救援模式 | 没有可用固件时，通过串口恢复 |
| IWDG | 独立看门狗 |
| littlefs | 掉电安全的嵌入式文件系统 |
| 磨损均衡 | 把擦写分散到不同块，延长 Flash 寿命 |
| 302 | HTTP 重定向状态码 |
| SNI | TLS 握手时告诉服务器要访问哪个域名 |

---

## 15. 学习路径与动手实验

### 15.1 建议的阅读顺序

1. `boot_layout.h` → 先看地址，建立空间感；
2. `boot_main.c` → 看启动和跳转；
3. `boot_param.c` → 看掉电安全；
4. `app_task.c` → 看 App 侧配合；
5. 再回来看本文第 4~7 章。

### 15.2 实验 1：观察跳转

1. 烧录 Bootloader 和 App；
2. 串口看 `[BOOT] ... jump`；
3. 用 Keil 在 App 的 `main` 打断点，确认能停下来；
4. 观察 `SCB->VTOR` 在 `App_Task_Init` 前后的值。

### 15.3 实验 2：观察参数区

1. 第一次启动后等 5 秒；
2. 用 ST-Link 查看 `0x0800C000` 的 64 B；
3. 找到 `magic='BOOT'`、`seq=1`、`flags=1`；
4. 再复位一次，确认 `seq` 不变。

### 15.4 实验 3：制造 App 无效

1. 只烧 Bootloader，不烧 App；
2. 复位，串口应打印 `app invalid, halt`；
3. 再烧 App，确认恢复。

### 15.5 实验 4：模拟掉电

1. 在 `Boot_Param_Save()` 的 `FLASH_ProgramWord` 循环里加延时；
2. 写入过程中复位；
3. 确认下次启动能回退到上一条记录。

### 15.6 进阶主题

- Cortex-M 异常模型与中断优先级；
- 分散加载文件（scatter file）与链接脚本；
- littlefs 的 CTZ 链表与掉电一致性；
- TLS 握手与证书验证；
- 安全启动（签名、HMAC、AES）；
- A/B 双分区与差分升级。

---

## 16. 下一步

Phase 0（跳转 + 参数区）与 Phase 1（离线 OTA + 回滚 + 掉电恢复）已全部完成，
并经过硬件实测（A/B1/B2/B3/B4 全过），记录见《OTA_Test_Record.md》第 2 节。

Phase 2（Gitee Releases 网络升级）已完成并硬件实测通过，详见第 18、19 节。
其中"更新源写死在固件里、导致设备只能升级一次"的缺陷，修复记录见 19.5。

下一阶段（Phase 3）待办，按"能不能把设备救回来"排序：

1. 看门狗：IWDG + 各任务心跳监控，而不是单纯喂狗；
2. 串口救援：`boot_main.c` 三处 `halt` 改为 YMODEM 接收
   （`BOOT_FLAG_FORCE_RECOVERY` 已预留但尚未使用）；
3. 失败原因落盘：参数区 `reserved[0]` 改为 `last_error`，UI 显示；
4. 可选：固件签名（Ed25519），公钥烧在 Bootloader。

---

## 17. 实测修正（2026-10-24/25，务必对照正文修正认知）

### 17.1 扇区宏是位掩码，不是序号

`stm32f4xx_flash.h` 里 `FLASH_Sector_4 = 0x0020`（SNB 编码），不是数字 4。
`for (s = FLASH_Sector_4; s <= FLASH_Sector_7; s++)` 会从 32 数到 56 共
25 次，其中 `0x24` 的 bit2 是 MER（Mass Erase）位——实测把整片 512 KB
Flash 抹掉（含 Bootloader 自身），设备变砖。
正确写法：显式列出扇区表，逐个擦。

### 17.2 boot_try 递增必须同时递增 seq

参数区按"seq 最大"取最新记录。Bootloader 给未确认 App 计数
`p.boot_try++` 后若不 `p.seq++`，新记录与旧记录 seq 相同，下次加载
读回的仍是旧值，boot_try 卡在 1→2 永远到不了 3，回滚永不触发。

### 17.3 固件大小不要依赖执行域符号

`Image$$ER_IROM1$$Limit` 是执行域末尾，不含 `.data` 初值的加载区
（`RW_IRAM1` 的 Load 区）。用它做备份大小，回滚后全局变量初值全 0xFF：
App 能跑、能确认，但 UI 黑屏——极具迷惑性。
本项目终版方案（零链接符号）：
- OTA 路径：Bootloader 把 `.ota` 头的 `fw_size` 写入参数区 `app_size`，
  App 备份/自检直接继承，精确等于 bin 长度；
- ST-Link 烧录（参数区为空）：用 `BuildConfig.h` 的
  `APP_FW_SIZE_FALLBACK`（160 KB）兜底，人工保证 >= 实际固件大小；
- 实验记录：AC5 自动分散加载下 `Load$$LR_IROM1$$Limit` 与
  `Image$$LR_IROM1$$Limit` 均不存在（Undefined Reference）；
  想"一个符号拿总长"必须手写 scatter 文件。

---

## 18. Phase 2：网络升级（设计记录，最终落地为 Gitee 直连）

### 18.1 总体结构

```
UI(WiFi页 Check Update 按钮) --EV_OTA_CHECK--> netTask
  OTA_Update_Run()（阻塞，下载期间独占 AT 模组）
    1. OTA_Net_HttpGet(api.github.com/releases/latest)  → JSON(16KB 缓冲)
       解析 tag_name（v1.0.6 → 0x010006）与第一个 .ota 资产 URL/size
    2. 版本比较：latest > APP_VERSION 才继续，否则 UI 显示已最新
    3. OTA_Net_HttpGetRedirect(github.com → 302 → objects.githubusercontent.com)
       两阶段：先不带回调探测跟跳拿到最终 URL，再正式下载
       流式回调 Dl_Sink：写 littlefs tmp + payload CRC + 进度 g_ota_prog
    4. 包头静态校验（magic/hw_id/version/size）+ payload CRC
    5. OTA_CommitFirmware()：rename → 备份 → 写参数 OTA_PENDING → 复位
       （与串口离线升级共用同一函数）
UI: LVGL 全屏浮层 + 进度条，uiTask 每拍 OTA_UI_Tick() 按 g_ota_prog 刷新，
    结果态显示 3 秒后自动收起。
```

### 18.2 关键实现点

- **为什么不用 AT+HTTPCLIENT**：它把整响应缓存在模组内一次性吐出，
  固件 150KB+ 远超缓冲；改用 CIPSTART(SSL) + CIPSEND 手写请求，
  +IPD 流式接收。
- **USART1 环形缓冲加大到 4KB**：littlefs 偶发块擦除（数十 ms）期间
  不能读串口，4KB ≈ 356ms 容量（115200bps）覆盖擦除延迟。
- **连接前强制 AT+CIPMUX=0**：保证 +IPD 是 "+IPD,<len>:" 单连接格式。
- **302 两阶段**：探测跳转不带数据回调（302 响应体不会污染下载文件），
  拿到最终 URL 后再带回调正式下载。
- **收满 Content-Length 必须置 finished**：否则服务器随后的 CLOSED 会
  被误判为 ERR_CLOSED（自查修掉的 bug）。
- **TLS 校验**：ESP-AT SSL 连接只加密不校验证书，Phase 3 用固件签名补强。
- **app_size 继承限制**：只有 APP_UNCONFIRMED 状态（Bootloader 刚按
  .ota 头写入）才精确；ST-Link 烧录后参数区是旧值，必须用
  APP_FW_SIZE_FALLBACK 兜底，否则备份截断（Phase 1 黑屏事故变体）。

### 18.3 实测步骤

1. 设备烧 1.0.5 测试版（DEBUG_SUPPORT=1，串口可看 [OTA] 日志）；
2. GitHub 创建 Release：tag `v1.0.6`，上传 `fw_106.ota`；
3. 主界面 → WiFi 图标 → 点 "Check Update"；
4. 浮层显示 Checking → Downloading xx% → Verifying → 复位；
5. Bootloader 搬运 → 1.0.6 启动 → 复位确认 ver=0x010006。

### 18.4 发布约定

- tag 形如 `v1.0.6`（版本号从 tag 解析）；
- Release 里放任意一个 `.ota` 资产即可（设备自动找第一个 .ota 后缀文件）；
- 发布说明（body）不要写太长：JSON 缓冲 16KB，body 过长会把
  assets 字段挤到缓冲外导致解析失败。

---

## 19. Phase 2 实测与踩坑记录（2026-10-25）

### 19.1 最终方案：Gitee 直连下载

校园网环境访问 GitHub 需要 VPN，而设备的 ESP8266 走校园网直连、
无法享受 PC 的 VPN，因此 GitHub API 与下载均不可达（CIPSTART 15 秒后 ERROR）。
Gitee 的 `releases/latest` API 可访问，但匿名调用很快触发限流（403）。

最终采用**直连 URL 模式**（`OTA_SOURCE_MODE=0`）：
不做 API 查询，直接下载固定地址的 `.ota`，版本从包头读取。

```
https://gitee.com/ssh_zfy/WiFi_Clock/releases/download/v1.0.6/fw_106_gitee.ota
```

实测链路（22:43）：
```
[OTA] direct download: https://gitee.com/.../fw_106_gitee.ota
[OTA] net: dial AT+CIPSTART="SSL","gitee.com",443
[OTA] net: http hdr status=302  → 跟随到 foruda.gitee.com（CDN）
[OTA] net: http hdr status=200 len=160248
[OTA] direct err=0 got=160248 clen=160248 http=200
[OTA] direct hdr ver=0x010006 size=160184
[BOOT] param: state=1 → start update → fw_size=160184 ver=0x00010006
[BOOT] erase 4/5/6/7 → write → verify → done, jump
```

耗时：TLS 握手 ×2 + 下载 ×2（探测一次、正式一次）约 45 秒。

### 19.2 本轮修掉的 8 个 bug（按发现顺序）

1. **netTask 栈 1KB 溢出**：OTA 路径局部变量约 1.7KB。修复：栈加到 3KB +
   大缓冲改 static（.bss）。
2. **所有忙等缺超时**：Net_SendRaw 的 TXE 轮询、Dbg_Flush 的 DMA/TC 等待、
   Net_WaitStr 的超时检查写在"无数据"分支里（持续数据流时永不触发）。
   修复：超时检查前置 + 每个忙等加 1~2 秒保护。
3. **EV_OTA_CHECK 不在等待掩码**：`xEventGroupWaitBits` 超时返回的 bits
   包含该位，而 `xClearOnExit` 清不掉掩码外的位 → 每秒重复触发 OTA。
   修复：把 EV_OTA_CHECK 加入等待掩码。
4. **Http_ParseHeader 索引死循环**：`i = (eol - line) + 2` 用相对偏移，
   而 line 已变成 `s_hdr + i`，i 在两个值间来回跳。**用 Keil 调试器
   Halt 看到调用栈停在 strstr → Http_ParseHeader 才定位**。
   修复：改成 `(eol - s_hdr) + 2`（绝对偏移）。
5. **s_hdr 无 `'\0'` 结尾**：`strstr` 会越界扫描。修复：每收一字节补结尾。
6. **302 响应头超过 1KB**：Gitee 的 302 头很长，原 1024 缓冲撑满后
   `\r\n\r\n` 检测失效。修复：头缓冲 4KB，Location 192→512 字节。
7. **CRC32 初值漏初始化**：`memset(&ctx,0,...)` 后 crc=0，而 CRC32 必须
   从 `0xFFFFFFFF` 开始。修复：下载前 `ctx.crc = 0xFFFFFFFFUL`。
8. **逐块 +IPD 日志过多**：160 块刷屏。修复：删除逐块打印，保留
   异常路径与最终结果。

### 19.3 调试方法论收获

- **printf 日志定位不了死循环时，用调试器 Halt 看调用栈**：
  一眼看到 `strstr ← Http_ParseHeader ← Net_RecvLoop`，
  比猜十轮快得多。
- **所有"等待"必须有超时**，且超时检查要放在读数据之前。
- **FreeRTOS 事件组**：等待掩码外的位置位后不会被自动清除，
  超时返回的 bits 里也可能带着它——用它做触发条件必须放进掩码。
- **流式协议解析**（HTTP 头/JSON）要防御三类问题：
  缓冲边界、字符串结尾、长字段（Location）溢出。

### 19.4 发布约定（2026-10-26 更新）

1. 仓库：`https://gitee.com/ssh_zfy/WiFi_Clock`
2. 发版：发行版页 → 新建/编辑 → tag `v1.0.7` → 上传 `fw_1.0.7.ota`
3. **附件名必须能从 tag 推出来**：`"fw_" + tag去掉'v' + ".ota"`。
   设备端直接按这条规则拼地址，不再读 `BuildConfig.h`，**发版不需要改代码**。
4. `.ota` 用 `ota_pack.py` 打包，`--version` 与 `Version.h` 一致。

旧约定（"改 `OTA_DIRECT_URL` 再重新发布"）已废弃，原因见 19.5。

### 19.5 更新源改为自动发现（Phase 2 bug 修复）

#### 问题：设备只能升级一次

Phase 2 把下载地址写死在 `BuildConfig.h`：

```c
#define OTA_DIRECT_URL "https://gitee.com/ssh_zfy/WiFi_Clock/releases/download/v1.0.6/fw_106_gitee.ota"
```

这个 URL 存在于**正在运行的固件**里，于是形成死循环：

1. 发 1.0.7 时改了源码里的 URL → 编译出的是 1.0.7 固件；
2. 但设备上跑的还是 1.0.6，它内部的 URL 仍指向 `v1.0.6`；
3. 1.0.6 设备去拉 `v1.0.6/fw_106_gitee.ota`，拿到的版本等于自己，
   判定"已是最新"。

**结果：设备只能升级一次（1.0.5 → 1.0.6），之后永远卡住。**
改源码对已经出厂的设备完全无效——这就是引导式自举悖论。

#### 为什么不用 manifest.json

考虑过在仓库里维护一个版本清单文件，但实测 Gitee 的 raw 通道会 302 到
`raw.giteeusercontent.com`，`Location` 长达 **400 字节**（带 `metadata` +
`signature` 签名参数），逼近设备 512 字节的 `location` 缓冲，而且路径越长越长。
放弃。

#### 方案：用 `releases/latest` 的 302 推导

实测 Gitee 三个关键行为：

| 请求 | 响应 | `Location` 长度 |
| --- | --- | --- |
| `/releases/latest` | 302 → `.../releases/tag/v1.0.6` | **56 B** |
| `/releases/download/v1.0.6/fw_106_gitee.ota` | 302 → `.../attach_files/<id>/download/...` | 83 B |
| `/raw/main/README.md` | 302 → `raw.giteeusercontent.com/...` | 400 B |

于是设备端三步走：

```text
① GET /releases/latest      -> 302，Location 尾部就是最新 tag
② 抠出 tag，拼附件地址       -> releases/download/<tag>/fw_<ver>.ota
③ 版本 > 当前？是则下载      -> 复用原有 302 跟随 + 流式写 littlefs
```

一个容易写错的点：`OTA_Net_HttpGet` 在 3xx 时会返回 `OTA_NET_ERR_HTTP`，
但 `resp` 已经完整填充，所以调用方必须判断 `resp.status_code`，
不能拿 `err != OTA_NET_OK` 当失败。

#### 顺带的性能修复

旧实现是"先下完 160 KB，再从 .ota 包头里读版本比较"，
已是最新也要空跑约 45 秒。

新实现把版本比较提到下载之前，只读一个 302 头（56 字节），
**几秒钟就能判定"已是最新"**。

#### 代码清理

按"只保留一条验证通过的路径"的原则删除：

| 删除项 | 原因 |
| --- | --- |
| `OTA_SOURCE_MODE` 三选一 | 只留 Gitee 自动发现 |
| Gitee / GitHub API 分支 | 匿名 API 会 403 限流 |
| `OTA_DIRECT_URL` 硬编码地址 | 正是本轮要修的 bug |
| `Json_GetStr` / `Json_Sink` / `Json_FindOtaAsset` | 不再解析 JSON |
| `StrsEqNoCase` / `s_json[16 KB]` / `s_api_path` | 同上 |
| `Documents/Tools/test_json_parse.py` | 由 `test_tag_parse.py` 取代 |

结果：`OTA_Update.c` 从 518 行降到 347 行，固件从 171,600 B 降到 159,864 B
（省 11.7 KB）。

#### 新增校验

包头校验多一条 `hdr->fw_version != latest_ver`：**附件里的版本必须与 tag 一致**。
防止把 1.0.6 的包挂在 v1.0.7 的 tag 下，设备装了旧版本却以为升级成功。

#### 离线回归

`Documents/Tools/test_tag_parse.py` 用 Python 复刻了 `OTA_ParseTag` /
`Ver_Parse` / `OTA_BuildAssetUrl` 的边界行为，覆盖真实 302、带查询参数、
带锚点、缺 tag、版本段越界、带后缀 tag 等 8 个用例，无需硬件即可回归。

```bash
py Documents/Tools/test_tag_parse.py
```

#### 迁移

1.0.6 的固件内置的是错误地址，**无法通过 OTA 自愈**，必须用 Keil / ST-Link
烧一次 1.0.7。从 1.0.7 起才进入自动升级循环。

#### 本轮验收标准

- 编译 0 Error 0 Warning
- `fw_1.0.7.ota` 包头：`magic=0x4F544131`、`ver=0x010007`、
  `size=159864`、`crc32=0xDD07D266`、`hw_id=0x04074001`
- 设备烧入 1.0.7 后，点 Check Update 应打印
  `[OTA] probe http=302` → `[OTA] latest tag=v1.0.7` → `[OTA] up to date`
  （因为当前就是 1.0.7）
- 发布 1.0.8 后再次点击，应打印 `[OTA] latest tag=v1.0.8` 并完成升级
---

## 20. 附录：设计沿革（早期方案 vs 最终实现）

> 本节由 2026-10-22 的设计稿《Bootloader_Design.md》提炼而来（原文件已合并删除）。
> 记下当时怎么选、后来推翻了什么、为什么改。**与正文冲突时以正文为准。**

### 20.1 早期方案中被采纳的部分

| 设计项 | 当时的方案 | 现状 |
| --- | --- | --- |
| 三段式分区 | Bootloader 48 KB + 参数区 16 KB + App 448 KB | 完全按此实现 |
| Bootloader 职责边界 | 只做校验、搬运、跳转；不联网、不带 RTOS | 一直守住 |
| 日志式参数区 | 追加写 + seq + CRC32，不覆盖写 | 实现为 32 × 64 B |
| confirmed + boot_try 回滚 | 3 次未确认自动回滚 | 已实现并实测 |
| 先校验后擦除 | 任何擦除前必须整包 CRC 通过 | 已实现 |
| PC 端打包工具 | `ota_pack.py` | 已实现 |

### 20.2 被推翻的七个决策

| # | 早期方案 | 最终实现 | 为什么改 |
| --- | --- | --- | --- |
| 1 | W25Q64 划 1 MB **裸区双 slot**（A 放新固件、B 放备份） | littlefs 里的 `/ota/firmware.bin` + `/ota/backup.bin` | 裸区要缩容 littlefs 并**重新格式化 + 重烧 3 批资源**（约 30 分钟）。用文件系统反而简单：`rename` 天然原子，不用自己管块分配和磨损均衡 |
| 2 | littlefs 从 2048 block 缩到 1536（8 MB → 6 MB） | 保持 2048 block 不动 | 同上。固件放文件里只占约 300 KB，没必要为它腾裸区 |
| 3 | 参数记录 **32 × 512 B** | 32 × 64 B | 64 B 已能放下全部字段。512 B 会把 16 KB 参数区正好用满，以后加字段没余地 |
| 4 | Bootloader 带 **LCD 驱动**（`boot_lcd.c`，精简 ST7789 + 8×16 字模） | 只用串口日志 | 省下约 3 KB Flash 和整套 SPI 初始化。搬运进度串口看得到，屏幕留给 App |
| 5 | 独立传输层 `OTA_Transport.c` | `BSP/Src/OTA_Net.c` | 职责合并少一层抽象。HTTP 解析和 AT 收发本来就强耦合（要边收 `+IPD` 边解析头），硬拆反而绕 |
| 6 | 串口救援用 **XMODEM** | 计划改 **YMODEM**（Phase 3） | YMODEM 支持 1 KB 包和"文件名 + 大小"元信息，比 XMODEM 快一倍 |
| 7 | 共享 `system_stm32f4xx.c`，用 `VECT_TAB_OFFSET` 条件编译 | Bootloader 用独立启动文件；App 在 `app_task.c` 里显式写 `SCB->VTOR` | 该文件是 GBK 编码，改它容易破坏中文注释；显式设置 VTOR 也更直观，不依赖编译宏 |

### 20.3 至今仍生效的五条原则

1. **Bootloader 尽量小、尽量笨**——只做校验、搬运、跳转。网络、文件系统、界面全部留在 App。
2. **先校验后破坏**——任何擦除 App 的动作之前，必须确认新固件 CRC 正确且完整。
3. **任何一步掉电都可恢复**——状态机 + 日志式参数 + 备份。
4. **正常启动不加负担**——App 有效时只做几十毫秒的轻量检查，不初始化 W25Q64。
5. **不改现有业务分层**——OTA 作为新的应用层模块接入，BSP 只增加传输封装。

### 20.4 早期识别的约束，现状如何

| 约束 | 当时的判断 | 现状 |
| --- | --- | --- |
| `FLASH_Sector_x` 是位掩码不是序号 | 已识别 | 用显式扇区表；但仍为此踩了一次全片擦除（Phase 1 Bug 1） |
| USART1 环形缓冲只有 512 B | 已识别，计划扩到 4096 | 已扩 |
| AT 层逐行解析，扛不住二进制流 | 已识别 | 改用 `+IPD` 原始流 + `AT+CIPSEND` |
| LCD 由 uiTask 独占 | 已识别 | OTA 进度走 `g_ota_prog`，由 uiTask 绘制 |
| W25Q64 是 SPI1 单总线，需互斥 | 已识别 | 复用 littlefs 的锁 |
| App 擦参数区停顿 200~400 ms | 已识别 | 仍存在，靠"只在写满 32 条时才擦"缓解 |

### 20.5 明确不做的事

| 方案 | 不做的理由 |
| --- | --- |
| 内部 Flash A/B 双分区 | 512 KB 放不下两份 448 KB App |
| Bootloader 自升级 | 风险收益不匹配，写坏就是真砖 |
| 差分升级 | bspatch 需要同时持有两份固件，448 KB 放不下 |
| 固件加密 | 解密密钥必须存在设备上，等于没有 |

### 20.6 威胁模型现状

| 威胁 | 当前措施 | 缺口 |
| --- | --- | --- |
| 传输误码 | 包 CRC32 + 长度校验 | 无 |
| 下载中断 | 参数不写，旧 App 不动 | 无 |
| 搬运掉电 | `OTA_WRITING` 重入 | 无 |
| 新固件崩溃 | confirmed + boot_try + 回滚 | 无 |
| Flash 位翻转 | 搬运后回读校验 | 无 |
| 参数区损坏 | 32 条日志 + 单条 CRC + seq | 无 |
| 刷错板型 | `hw_id` 校验 | 无 |
| **恶意固件** | **无** | ESP-AT 的 TLS 不校验证书，应用层也没有签名 |
| **降级攻击** | 版本比较（拒绝不新） | 未限制"最低可接受版本" |
| **Bootloader 自身损坏** | 无法自恢复 | 只能 SWD 重烧 |

加粗的三项是 Phase 3 的候选范围，详见第 16 节。
