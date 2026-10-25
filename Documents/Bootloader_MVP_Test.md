# Bootloader MVP 测试说明

> 范围：Bootloader 跳转 + App 分区 + 参数区
> 不包含：OTA 下载、littlefs 读写、备份/回滚、UI
> 对应阶段：Phase 0（最小闭环）

---

## 1. 本次改动

### 新增

| 文件 | 说明 |
| --- | --- |
| `Boot/Inc/boot_layout.h` | 内部 Flash 分区、OTA 逻辑路径宏 |
| `Boot/Inc/boot_param.h` | 参数记录结构、状态枚举、接口 |
| `Boot/Inc/boot_crc32.h` | 标准 CRC32 接口 |
| `Boot/Inc/boot_uart.h` | Bootloader 极简 USART2 驱动 |
| `Boot/Src/boot_main.c` | 启动、快速检查、跳转 |
| `Boot/Src/boot_param.c` | 日志式参数读写（32 × 512 B） |
| `Boot/Src/boot_crc32.c` | nibble 查表 CRC32 |
| `Boot/Src/boot_uart.c` | 轮询 USART2（PA2/PA3，115200） |
| `Bootloader/STM32F407_Boot.uvprojx` | Bootloader 独立工程 |

### 修改

| 文件 | 改动 |
| --- | --- |
| `MDK/STM32F407.uvprojx` | IROM `0x8010000/0x70000`；Define 加 `APP_BUILD`；IncludePath 加 `..\Boot\Inc`；启用 fromelf 生成 `.bin`；新增 `BOOT` 文件组 |
| `User/Application/Src/app_task.c` | 启动时恢复 `SCB->VTOR = APP_BASE`；创建 5 秒后写 `confirmed` 的确认任务 |

### 未改动

- `Core/system_stm32f4xx.c`（GBK，避免编码破坏）
- `BSP/`、`User/Task/`、`User/UI/` 的业务代码
- littlefs、W25Q64、资源文件

---

## 2. 编译与烧录

### 2.1 编译 Bootloader

1. 打开 `Bootloader/STM32F407_Boot.uvprojx`；
2. 确认 Device 为 `STM32F407VETx`、Pack 为 `Keil.STM32F4xx_DFP.2.15.0`；
3. Build，确认 `Bootloader/Output/STM32F407_Boot.axf` 与 `.bin` 生成；
4. 检查 `.map`：本机实测 `Code=2060`、`RO-data=508`，`Total ROM Size = 2584 B`（2.5 KB），48 KB 余量充足。

### 2.2 编译 App

1. 打开 `MDK/STM32F407.uvprojx`；
2. Build，确认 `MDK/Output/STM32F407.bin` 生成（fromelf 已启用）；
3. 检查 `.map` 的 `Total ROM Size`，应在 448 KB 以内（本机实测 150,668 B，约 147 KB）。

### 2.3 烧录

**顺序不能反**：

1. 先烧 Bootloader（起始 `0x08000000`，48 KB）；
2. 再烧 App（起始 `0x08010000`，448 KB）。

首次烧录建议先擦除整片（Keil → Flash → Erase），避免旧固件残留在参数区 `0x0800C000`。

---

## 3. 验证

### 3.1 串口输出

USART2（PA2=TX，PA3=RX），115200-8N1，复位后应看到：

```text
[BOOT] WiFi_Clock bootloader
[BOOT] param: empty
[BOOT] app valid, jump
```

随后 App 正常启动。

### 3.2 参数区确认

等待 5 秒以上（App 的确认任务），再复位，串口应变为：

```text
[BOOT] WiFi_Clock bootloader
[BOOT] param: state=0 seq=1 ver=0x00000000
[BOOT] app valid, jump
```

再复位一次，`seq` 仍为 `1`，说明已确认且不会重复写。

### 3.3 App 功能

- 屏幕正常显示；
- 资源自检 43 项通过（串口 `[ASSET]` 日志或界面）；
- WiFi、SNTP、天气、DHT22 与原固件一致。

### 3.4 参数区读写（可选）

用 ST-Link 查看 `0x0800C000` 起始的 64 B：`magic = 'BOOT'`、`state = 0`、`flags = 1`、`seq = 1`。

---

## 4. 失败排查

| 现象 | 可能原因 | 处理 |
| --- | --- | --- |
| 串口无 `[BOOT]` 输出 | Bootloader 未烧录 / USART2 未接 / 波特率错 | 检查烧录地址与 PA2/PA3 |
| `app invalid, halt` | App 未烧到 `0x08010000`，或烧录后未复位 | 确认 App IROM 配置并重烧 |
| App 启动后死机 | App 的 VTOR 未恢复 | 确认 `app_task.c` 的 `Boot_Apply_VectorTable()` 已编译 |
| 参数区 `seq` 一直增加 | 确认任务未执行或写入失败 | 检查 `Boot_Param_Confirm()` 返回值与 Sector 3 是否被写保护 |
| Keil 链接报 `L6406E` / 空间不足 | IROM 仍为 `0x80000` | 检查 Target → IROM1 是否为 `0x8010000/0x70000` |

---

## 5. 回退方法

如果这一版不能接受，回退只需要两步：

1. `MDK/STM32F407.uvprojx`：IROM 改回 `0x8000000/0x80000`，去掉 `APP_BUILD`；
2. `app_task.c`：删除 `SCB->VTOR` 与确认任务。

Bootloader 不烧录时不影响原 App。

---

## 6. 已知限制

- Bootloader 只做快速检查（栈顶/复位向量），不做完整 CRC；
- 参数记录里的 `app_size/app_crc32` 仍为 0，等 Phase 1 的 OTA 流程填充；
- 不读 littlefs、不联网、不做回滚；
- 确认时机固定在启动后 5 秒，后续应移到「开机流程完成」之后。

---

## 7. 下一步（Phase 1）

1. Bootloader 加入只读 littlefs，能打开 `/ota/firmware.bin`；
2. App 增加 `OTA_Store`，通过 USART2 接收离线 `.ota` 并写入文件；
3. 实现备份、参数 `OTA_PENDING`、重启搬运；
4. 验证搬运中断电、CRC 失败、`backup.bin` 回滚。
