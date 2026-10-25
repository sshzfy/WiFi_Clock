# Phase 1 离线 OTA 测试说明

> 目标：串口把 `.ota` 发给设备，验证"下载 → 写文件 → 备份 → 重启 → Bootloader 搬运 → 新版本运行"，以及回滚和掉电恢复。
> 前提：Phase 0 已通过（Bootloader 跳转 + 参数区正常）。

---

## 1. 本阶段新增内容

| 侧 | 模块 | 职责 |
| --- | --- | --- |
| Bootloader | `Boot/Src/boot_lfs.c` | 只读挂载 littlefs（`LFS_READONLY`） |
| Bootloader | `Boot/Src/boot_update.c` | 校验 `.ota` 头/CRC → 擦 Sector 4-7 → 搬运 → 回读校验 → 写参数 → 跳转 |
| Bootloader | `Boot/Src/boot_main.c` | 升级状态机：`OTA_PENDING/WRITING/ROLLBACK/APP_UNCONFIRMED` |
| App | `BSP/Src/OTA_Store.c` | littlefs 文件写入（open/write/close/rename/remove/stat） |
| App | `BSP/Src/OTA_Serial.c` | USART2 RX 中断 + 4 KB 环形缓冲（`USART2_IRQHandler` 定义在此） |
| App | `User/Application/Src/OTA.c` | 协议状态机、写 `firmware.tmp`、CRC 校验、备份、写参数、复位 |
| App | `User/Task/Src/OTA_Task.c` | `ota` 任务（优先级 2，2 KB 栈） |
| PC | `Documents/Tools/ota_pack.py` | `.bin` → `.ota` 打包 |
| PC | `Documents/Tools/ota_send.py` | 串口发送协议（依赖 pyserial） |

协议（双方一致）：

```text
PC → MCU: 55 AA 'OTA'           握手
MCU → PC: AA 55 'OK'
PC → MCU: 元数据 20B <HHIIII: 0x55AA, 20, total_len, crc32, version, hw_id>
MCU → PC: AA 55 'OK'
PC → MCU: 裸 .ota 字节流
MCU → PC: AA 55 'DONE'（成功，随后复位） / 'FAIL'（失败）
```

---

## 2. 编译与烧录

1. 打开 `Bootloader/STM32F407_Boot.uvprojx`，Build（本机实测 ROM 约 11 KB / 48 KB）；
2. 打开 `MDK/STM32F407.uvprojx`，确认 `BuildConfig.h` 中 `OTA_SERIAL_TEST=1`、`DEBUG_SUPPORT=0`，Build；
3. 整片擦除后：先烧 Bootloader，再烧 App；
4. 串口 PA2/PA3 应看到原有 `[BOOT]` 日志，App 正常启动；等 5 秒后复位，参数区显示 `ver=0x010000`（App 1.0.0）。

---

## 3. 打包新固件

升级测试需要"比当前版本新"的固件，以 1.0.1 为例：

1. 修改 `User/Application/Inc/Version.h`：`APP_VERSION_PATCH` 0 → 1（`APP_VERSION_STR "1.0.1"`）；
2. 重新 Build App，得到 `MDK/Output/STM32F407.bin`；
3. 打包：

```bat
py Documents\Tools\ota_pack.py MDK\Output\STM32F407.bin firmware_1.0.1.ota --version 1.0.1
```

> 版本必须大于当前运行版本，否则 App 直接拒绝（`OTA_ERR_VERSION`）。

---

## 4. 发送与升级

```bat
py -m pip install pyserial
py Documents\Tools\ota_send.py firmware_1.0.1.ota COM3 --version 1.0.1
```

（COM3 换成实际串口号。注意：**先断开串口助手**，串口只能一个程序占用。）

设备侧预期（`[BOOT]` 日志在复位后由 Bootloader 打印）：

```text
发送完成 → App 校验/备份（约 4~5 秒无输出，属正常）
→ 收到 DONE → 设备复位
→ [BOOT] param: state=2 seq=N ver=0x00000000      ← OTA_PENDING
→ [BOOT] start update
→ [BOOT] mount littlefs ... / open /ota/firmware.bin
→ [BOOT] fw_size=... ver=0x010101
→ [BOOT] check crc ... / erase sector 4 5 6 7
→ [BOOT] write app ... 25% 50% 75% 100%
→ [BOOT] verify ... / done, jump
→ App 以 1.0.1 启动
```

等 5 秒后复位，参数区显示 `state=0 ver=0x010101`（已确认新版本）。

---

## 5. 验证项

### 5.1 正常升级

- 升级后 App 运行 1.0.1；
- 复位后 Bootloader 显示 `ver=0x010101`；
- W25Q64 上出现 `/ota/firmware.bin`（约 150 KB）与 `/ota/backup.bin`。

### 5.2 CRC 校验拒绝

把 `firmware_1.0.1.ota` 中间改几个字节（不重新打包）再发送：设备回复 `FAIL`，App 区原封不动，旧版本继续运行。

### 5.3 版本不新

用 `--version 1.0.0` 发送（等于当前版本）：`metadata rejected`，设备不动作。

### 5.4 搬运中掉电

升级过程中（看到 `erase sector` / `write app` 阶段）拔电再上电：Bootloader 检测 `state=OTA_WRITING` 自动重新搬运，最终完成升级。

### 5.5 回滚

1. 打包一个"坏固件"1.0.2：在 `App_Task_Init()` 的 `Boot_Apply_VectorTable()` 之后插入 `while(1);`（确认任务永远不会创建），编译打包发送；
2. 升级完成、设备跳转到坏固件后死循环（屏幕停在开机前状态）；
3. 手动复位，观察 Bootloader 日志中 `state=3`（APP_UNCONFIRMED）且每次 `try` 递增；
4. 第 4 次复位时 Bootloader 自动回滚：

```text
[BOOT] app unconfirmed, rollback
[BOOT] ... backup.bin ... write app ... done, jump
```

5. 设备恢复为 1.0.1，参数区 `state=0`、`flags` 含 CONFIRMED。

### 5.6 备份空间检查

W25Q64 剩余空间不足时（可临时塞大文件模拟），App 回复 `FAIL`，不进入升级。

---

## 6. 常见问题

| 现象 | 原因 / 处理 |
| --- | --- |
| `handshake failed` | 串口没接 / 被串口助手占用 / App 未烧最新版（`OTA_SERIAL_TEST=1` 未生效） |
| `metadata rejected` | 版本不新、hw_id 不匹配（`ota_pack.py` 与 `ota_pkg.h` 的 `HW_ID` 不一致） |
| `no valid response` | 设备在备份阶段被复位 / 波特率错 / 数据被串口线损 |
| Bootloader 打印 `open failed` | `/ota/firmware.bin` 不存在：App 侧 rename 前失败，或 littlefs 损坏 |
| 搬运后 App 死机 | 确认 `APP_VERSION` 与打包版本一致；回滚机制会兜底 |
| 升级后 `try` 一直涨 | App 确认任务没执行（坏固件），3 次后自动回滚 |

---

## 7. 回退

不需要 OTA 串口任务时：`BuildConfig.h` 把 `OTA_SERIAL_TEST` 改为 0，重新编译即可（USART2 交还给调试串口）。
---

## 8. 实测记录与修复（2026-10-24/25 硬件实测）

### 8.1 测试矩阵结果

| 用例 | 结果 |
| --- | --- |
| A 正常升级 1.0.0 → 1.0.1 | 通过（SUCCESS → 搬运 → 确认 state=0 ver=0x010001） |
| B1 CRC 损坏包拒绝 | 通过（metadata ok → 收完 → FAIL，旧固件不动） |
| B2 版本不新拒绝 | 通过（metadata rejected，一个字节都不发） |
| B3 搬运中掉电重入 | 通过（重启 state=1/2 → start update → 完整搬运） |
| B4 坏镜像 3 次失败自动回滚 | 通过（第 3 次复位 rollback → 恢复 1.0.3） |
| 新方案回归（参数区继承 + 回滚完整性） | 通过（回滚 fw_size=153652 与包一致，屏幕正常） |

### 8.2 修复的三个 bug

**Bug 1（严重）：扇区宏当序号用，触发全片擦除**
- `FLASH_Sector_4~7` 是 FLASH_CR 位掩码（0x20/0x28/0x30/0x38），不是连续序号；
- `for (s = FLASH_Sector_4; s <= FLASH_Sector_7; s++)` 实际从 32 数到 56；
- `s=36=0x24` 的 bit2 是 MER（Mass Erase）位，触发整片擦除，Bootloader 自身被抹掉；
- 修复：`Boot/Src/boot_update.c` 改为显式扇区表，只擦 4/5/6/7 四个扇区。

**Bug 2：boot_try 递增不涨 seq，回滚永不触发**
- `Boot_Param_Load` 取 seq 最大的记录；`p.boot_try++` 后保存时 seq 不变，
  下次加载读回的仍是旧记录（严格大于才覆盖），boot_try 卡在 1→2；
- 修复：`Boot/Src/boot_main.c` 中 `p.boot_try++; p.seq++;` 一起保存。

**Bug 3：备份丢失 .data 初值，回滚后黑屏**
- `OTA_AppSelfSize()` 原来只算到 `Image$$ER_IROM1$$Limit`（执行域末尾），
  漏掉 RW_IRAM1 的加载数据（.data 初值，压缩后 276 B）；
- 回滚时该段没写回，启动后全局变量初值全 0xFF，UI 初始化异常黑屏
  （Bootloader 确认任务不依赖这些变量，所以 state 照常变 0，具迷惑性）；
- 修复（终版）：**彻底不用链接符号**，改为参数区继承：
  - OTA 路径：Bootloader 按 .ota 头 `fw_size` 写入参数区 `app_size`，
    App 备份/自检直接读它，精确等于 bin 长度；
  - ST-Link 路径：参数区为空时用 `BuildConfig.h` 的
    `APP_FW_SIZE_FALLBACK`（160 KB）兜底，必须 >= 实际固件大小；
  - `--datacompressor=off` 与三个 `extern Image$$...` 全部撤销。
- 注意：`Load$$LR_IROM1$$Limit` / `Image$$LR_IROM1$$Limit` 两个
  加载域符号在 AC5 自动分散加载下实测均不可用（Undefined Reference），
  故不采用"一个符号拿总长"的方案。

### 8.3 环境坑

- 本机 `python` 是 Microsoft Store 假程序（静默无输出），必须用 `py`；
- pyserial 安装：`py -m pip install pyserial -i https://pypi.org/simple`；
- 串口助手必须关闭后再跑 `ota_send.py`（串口独占）；
- Keil IDE 打开工程时命令行 `UV4 -b` 会被 IDE 抢占（可能弹"外部修改"
  对话框），命令行构建前先关闭 IDE。