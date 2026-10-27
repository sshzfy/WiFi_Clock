# OTA 测试记录（Phase 0 / 1 / 2）

> 本文汇总 Bootloader 与 OTA 三个阶段的**测试方法、实测结果与修复记录**。
> 原理与设计见《Bootloader_OTA_Guide.md》，当前实现流程见《Bootloader_OTA_Flow.md》。

## 0. 总览

| 阶段 | 范围 | 验证方式 | 结果 |
| --- | --- | --- | --- |
| Phase 0 | Bootloader 跳转 + App 分区 + 参数区 | 串口日志 + ST-Link 查 Flash | 通过 |
| Phase 1 | 串口离线 OTA + 备份回滚 + 掉电恢复 | 6 个用例（A / B1~B4 + 回归） | 通过 |
| Phase 2 | Gitee 网络升级 + 版本自动发现 | 端到端升级 + Gitee 302 实测 | 通过 |

测试硬件：STM32F407VET6 + W25Q64 + ESP8266(ESP-AT) + 2.4" SPI 触摸屏。
测试串口：USART2（PA2/PA3），115200-8N1。

---

## 1. Phase 0：最小闭环

范围：Bootloader 跳转 + App 分区 + 参数区读写。**不含** OTA 下载、littlefs、备份回滚。

### 1.1 改动

| 类型 | 文件 |
| --- | --- |
| 新增 | `Boot/Inc/boot_layout.h`、`boot_param.h`、`boot_crc32.h`、`boot_uart.h` |
| 新增 | `Boot/Src/boot_main.c`、`boot_param.c`、`boot_crc32.c`、`boot_uart.c` |
| 新增 | `Bootloader/STM32F407_Boot.uvprojx` |
| 修改 | `MDK/STM32F407.uvprojx`：IROM 改 `0x8010000/0x70000`，加 `APP_BUILD` 宏与 `..\Boot\Inc` 头文件路径，启用 fromelf 生成 `.bin` |
| 修改 | `User/Application/Src/app_task.c`：恢复 `SCB->VTOR`，创建 5 秒后写 `confirmed` 的确认任务 |

### 1.2 烧录顺序

**不能反**：先 Bootloader（`0x08000000`，48 KB），再 App（`0x08010000`，448 KB）。

首次烧录建议整片擦除（Keil → Flash → Erase），避免旧固件残留在参数区。

### 1.3 验证

首次上电（参数区为空）：

```text
[BOOT] WiFi_Clock bootloader
[BOOT] param: empty
[BOOT] app valid, jump
```

等 5 秒（确认任务写入参数区）后再复位：

```text
[BOOT] param: state=0 seq=1 ver=0x00000000
[BOOT] app valid, jump
```

再复位一次 `seq` 仍为 1 —— 说明已确认且不会重复写。

App 侧检查：屏幕正常显示、资源自检 43 项通过、WiFi/SNTP/天气/DHT22 与原固件一致。

### 1.4 失败排查

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 串口无 `[BOOT]` 输出 | Bootloader 未烧 / USART2 未接 / 波特率错 | 检查烧录地址与 PA2/PA3 |
| `app invalid, halt` | App 未烧到 `0x08010000` | 确认 App IROM 配置并重烧 |
| App 启动后死机 | VTOR 未恢复 | 确认 `Boot_Apply_VectorTable()` 已编译 |
| `seq` 一直增加 | 确认任务未执行 | 检查 `Boot_Param_Confirm()` 返回值 |
| 链接报 `L6406E` | IROM 仍是 `0x80000` | 改为 `0x8010000/0x70000` |

### 1.5 当时的已知限制

- 只做快速检查（栈顶/复位向量），不做整包 CRC；
- 参数区 `app_size/app_crc32` 仍为 0，等 Phase 1 填充；
- 不读 littlefs、不联网、不做回滚；
- 确认时机固定在启动后 5 秒。

---

## 2. Phase 1：串口离线 OTA

目标：串口把 `.ota` 发给设备，验证"下载 → 写文件 → 备份 → 重启 → 搬运 → 新版本运行"，以及回滚与掉电恢复。

### 2.1 新增模块

| 侧 | 模块 | 职责 |
| --- | --- | --- |
| Bootloader | `boot_lfs.c` | 只读挂载 littlefs |
| Bootloader | `boot_update.c` | 校验包头/CRC → 擦 Sector 4-7 → 搬运 → 回读校验 → 写参数 → 跳转 |
| Bootloader | `boot_main.c` | 升级状态机 |
| App | `OTA_Store.c` | littlefs 写入封装 |
| App | `OTA_Serial.c` | USART2 RX 中断 + 4 KB 环形缓冲 |
| App | `OTA.c` | 协议状态机、写 tmp、CRC、备份、写参数、复位 |
| App | `OTA_Task.c` | `ota` 任务（优先级 2，2 KB 栈） |
| PC | `ota_pack.py` | `.bin` → `.ota` |
| PC | `ota_send.py` | 串口发送（依赖 pyserial） |

### 2.2 串口协议

```text
PC → MCU: 55 AA 'OTA'                                 握手
MCU → PC: AA 55 'OK'
PC → MCU: 元数据 20B <HHIIII: 0x55AA, 20, total_len, crc32, version, hw_id>
MCU → PC: AA 55 'OK'
PC → MCU: 裸 .ota 字节流
MCU → PC: AA 55 'DONE'（成功，随后复位） / 'FAIL'
```

### 2.3 测试步骤

1. 编译 Bootloader（ROM 约 11 KB / 48 KB）；
2. App 侧 `BuildConfig.h`：`OTA_SERIAL_TEST=1`、`DEBUG_SUPPORT=0`（两者**互斥**，都占 USART2）；
3. 整片擦除 → 先烧 Bootloader → 再烧 App；
4. 改 `Version.h` 的 `PATCH` 生成新版本 → 编译 → 打包：

```bat
py Documents\Tools\ota_pack.py MDK\Output\STM32F407.bin firmware_1.0.1.ota --version 1.0.1
```

5. **先断开串口助手**（串口独占），再发送：

```bat
py Documents\Tools\ota_send.py firmware_1.0.1.ota COM3 --version 1.0.1
```

### 2.4 设备侧预期日志

```text
发送完成 → App 校验/备份（约 4~5 秒无输出，正常）
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

等 5 秒后再复位，参数区变为 `state=0 ver=0x010101`（已确认）。

### 2.5 验证项

| 项 | 做法 | 预期 |
| --- | --- | --- |
| 正常升级 | 见 2.3 | 新版本运行，`/ota/firmware.bin` 与 `backup.bin` 出现 |
| CRC 拒绝 | 改包中间几个字节不重新打包 | 设备回 `FAIL`，App 区不动 |
| 版本不新 | `--version 1.0.0`（等于当前） | `metadata rejected`，一个字节都不发 |
| 搬运中掉电 | 看到 `erase sector`/`write app` 时拔电 | 重启后 `state=OTA_WRITING` 自动重新搬运 |
| 坏镜像回滚 | 在 `Boot_Apply_VectorTable()` 后插 `while(1);` | 每次复位 `try` 递增，第 4 次自动回滚 |
| 空间不足 | 塞大文件占用 littlefs | App 回 `FAIL`，不进入升级 |

### 2.6 常见问题

| 现象 | 原因 / 处理 |
| --- | --- |
| `handshake failed` | 串口未接 / 被串口助手占用 / `OTA_SERIAL_TEST` 未生效 |
| `metadata rejected` | 版本不新、`HW_ID` 与 `ota_pkg.h` 不一致 |
| `no valid response` | 备份阶段被复位 / 波特率错 / 线损 |
| `open failed` | `/ota/firmware.bin` 不存在（rename 前失败或 littlefs 损坏） |
| 搬运后 App 死机 | 确认 `APP_VERSION` 与打包版本一致；回滚会兜底 |
| `try` 一直涨 | 确认任务没执行（坏固件），3 次后自动回滚 |

### 2.7 实测结果（2026-10-24/25）

| 用例 | 结果 |
| --- | --- |
| A 正常升级 1.0.0 → 1.0.1 | 通过（SUCCESS → 搬运 → 确认 `state=0 ver=0x010001`） |
| B1 CRC 损坏包拒绝 | 通过（metadata ok → 收完 → FAIL，旧固件不动） |
| B2 版本不新拒绝 | 通过（metadata rejected） |
| B3 搬运中掉电重入 | 通过（重启 → start update → 完整搬运） |
| B4 坏镜像 3 次失败自动回滚 | 通过（第 3 次复位 rollback → 恢复 1.0.3） |
| 回归：参数区继承 + 回滚完整性 | 通过（回滚 `fw_size=153652` 与包一致，屏幕正常） |

### 2.8 Phase 1 修掉的三个 bug

**Bug 1（严重）：扇区宏当序号用，触发全片擦除**

`FLASH_Sector_4~7` 是 FLASH_CR 的**位掩码**（0x20/0x28/0x30/0x38），不是连续序号。
`for (s = FLASH_Sector_4; s <= FLASH_Sector_7; s++)` 实际从 32 数到 56，
其中 `s=36=0x24` 的 bit2 是 **MER（全片擦除）位**，直接把 Bootloader 自己抹掉了。

修复：改为显式扇区表 `{FLASH_Sector_4, 5, 6, 7}`。设备靠 ST-Link 恢复。

**Bug 2：`boot_try` 递增不涨 `seq`，回滚永不触发**

`Boot_Param_Load()` 取 `seq` 最大的记录。`p.boot_try++` 后保存时 `seq` 没变，
下次加载读回的仍是旧记录（严格大于才覆盖），`boot_try` 卡在 1→2 循环。

修复：`boot_main.c` 中改为 `p.boot_try++; p.seq++;`。

**Bug 3：备份丢失 `.data` 初值，回滚后黑屏**

`OTA_AppSelfSize()` 原来只算到 `Image$$ER_IROM1$$Limit`（执行域末尾），
漏掉了 RW_IRAM1 的加载数据（`.data` 初值，压缩后 276 B）。回滚时这段没写回，
启动后全局变量初值全是 `0xFF`，UI 初始化异常黑屏——而 Bootloader 的确认任务
不依赖这些变量，所以 `state` 照常变 0，现象极具迷惑性。

修复（终版）：**彻底不用链接符号**，改为参数区继承：

- OTA 路径：Bootloader 按 `.ota` 头 `fw_size` 写入参数区 `app_size`，App 备份直接读它；
- ST-Link 路径：参数区为空时用 `APP_FW_SIZE_FALLBACK`（160 KB）兜底；
- `--datacompressor=off` 与三个 `extern Image$$...` 全部撤销。

> 顺带记录：`Load$$LR_IROM1$$Limit` / `Image$$LR_IROM1$$Limit` 两个加载域符号
> 在 AC5 自动分散加载下实测均不可用（Undefined Reference），所以"一个符号拿总长"走不通。

---

## 3. Phase 2：Gitee 网络升级

详细设计记录见《Bootloader_OTA_Guide.md》第 18、19 节，此处只列结果。

### 3.1 端到端实测（2026-10-25 22:43）

```text
[OTA] direct err=0 got=160248 clen=160248 http=200
[OTA] direct hdr ver=0x010006 size=160184
[BOOT] param: state=1 seq=59 ver=0x00010005 → start update
[BOOT] fw_size=160184 ver=0x00010006
→ erase 4/5/6/7 → write 100% → verify → done, jump
```

### 3.2 修复的 8 个 bug

| # | 现象 | 根因 | 修复 |
| --- | --- | --- | --- |
| 1 | `Stack overflow in task net` | netTask 栈 1 KB 不够 OTA 调用链（约 1.7 KB） | 栈改 3 KB + 大缓冲移 `.bss` |
| 2 | 下载卡死 | `Net_WaitStr` 只在无数据分支检查超时 | 超时检查提到循环开头 |
| 3 | OTA 每秒重复触发 | `EV_OTA_CHECK` 不在等待掩码里，超时返回带着它 | 加入 `xEventGroupWaitBits` 掩码 |
| 4 | 解析 HTTP 头死循环 | `i = (eol - line) + 2` 而 `line = s_hdr + i`，索引来回振荡 | 改为 `(eol - s_hdr) + 2` |
| 5 | `strstr` 越界 | 头缓冲没写 `'\0'` | 每收一字节补终止符 |
| 6 | 302 头撑爆缓冲 | Gitee 302 头超 1 KB | 头缓冲 1 KB → 4 KB，`location` 192 → 512 |
| 7 | CRC 校验失败 | `memset` 后 `crc=0`，而 CRC32 必须从 `0xFFFFFFFF` 起 | 下载前显式初始化 |
| 8 | 日志刷屏 | 逐块打印 `+IPD` | 只保留异常路径与最终结果 |

### 3.3 更新源缺陷（2026-10-26 修复）

Phase 2 最初把下载地址写死在 `BuildConfig.h`，导致**设备只能升级一次**：
URL 存在于正在运行的固件里，1.0.6 的设备永远只知道 1.0.6 的地址。

修复：改为读 `releases/latest` 的 302 头拿最新 tag，再按约定拼附件地址。

| 请求 | 响应 | `Location` 长度 |
| --- | --- | --- |
| `/releases/latest` | 302 → `/releases/tag/v1.0.6` | 56 B |
| `releases/download/v1.0.6/fw_106_gitee.ota` | 302 → `attach_files/<id>/download/` | 83 B |
| `/raw/main/README.md` | 302 → `raw.giteeusercontent.com/...?signature=` | 400 B（逼近 512 缓冲，弃用） |

附带收益：版本比较提前到下载之前，已是最新时不再空跑 160 KB（45 秒 → 几秒）。

离线回归：`Documents/Tools/test_tag_parse.py` 覆盖 8 个边界用例，全部通过。

---

## 4. 环境与工具坑（跨阶段汇总）

| 坑 | 说明 |
| --- | --- |
| `python` 是 Microsoft Store 假程序 | 静默无输出，必须用 `py` |
| pyserial 安装源 | `py -m pip install pyserial -i https://pypi.org/simple` |
| 串口独占 | 跑 `ota_send.py` 前必须关闭串口助手 |
| Keil 命令行构建 | IDE 打开时 `UV4 -b` 会被 IDE 抢占，构建前先关 IDE |
| 编码 | 源码是 GB2312，`Documents/*.md` 是 UTF-8；用错编码保存会把中文变乱码 |
| 换行符 | 项目内不统一（部分文件 LF、部分 CRLF），编译警告 `#1-D` 就是它引起的 |
| 设备访问 Gitee | 校园网下设备直连可用，PC 走 VPN 反而解析到 fake-ip |
| 发布顺序 | `releases/latest` 按发布顺序取，**必须单调递增**，不能补发旧版本号 |

---

## 5. 结论

三个阶段全部通过硬件实测。当前系统的已验证能力：

- 正常升级（串口 / 网络两条路径）
- 坏包拒绝（CRC 双保险：传输中 + 搬运前）
- 版本降级拒绝
- 搬运中途掉电可重入
- 坏固件 3 次启动失败自动回滚
- 备份完整性（含 `.data` 段）

**尚未覆盖**：看门狗、串口救援（`halt` 后只能靠 ST-Link）、固件签名。这三项属于 Phase 3。