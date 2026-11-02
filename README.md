# WiFi 时钟

基于 **STM32F407VET6 + FreeRTOS** 的桌面天气时钟与室内环境监测终端。设备白天使用 2.4 英寸彩屏显示时间、日期、天气和室内温湿度，夜间切换到 OLED 只显示时间，并降低系统活动以节省功耗。

项目包含一套完整的 **Bootloader + OTA** 方案：应用可以从 Gitee Releases 检查更新、下载固件并写入外部 Flash，重启后由 Bootloader 搬运到内部 Flash；新固件启动后需要主动确认，连续 3 次启动失败会自动回滚到旧版本。

---

## 总览

| 项目     | 说明                                                                  |
| -------- | --------------------------------------------------------------------- |
| 主控     | STM32F407VET6，Cortex-M4F，168MHz，512KB Flash，128KB SRAM + 64KB CCM |
| 操作系统 | FreeRTOS V10.4.3 LTS Patch 3，1kHz tick，heap_4                       |
| 显示     | ST7789 240×320 SPI 彩屏；SSD1306 128×64 OLED                          |
| 交互     | 2046 电阻触摸、单用户按键、光敏电阻                                   |
| 联网     | ESP32-C3-MINI-1，AT 指令，USART1                                      |
| 传感器   | DHT22 温湿度、DS1302 外部 RTC                                         |
| 存储     | W25Q64 8MB SPI Flash，littlefs 文件系统                               |
| 图形库   | LVGL 8.3.11，用于配网、定位输入和 OTA 界面                            |
| 开发环境 | Keil MDK 5 + ARMCC V5.06                                              |
| 当前版本 | App `1.0.0`                                                           |

### 主要功能

- **时钟与日历**：SNTP 网络校时，TIM5 维护软件时钟；夜间可切换到 DS1302，断网后仍能走时。
- **天气信息**：通过 ESP32-C3 获取公网 IP 和天气数据，支持手动输入省/市作为定位。
- **室内环境**：DHT22 周期采集温湿度，显示在主页卡片中。
- **昼夜双屏**：光敏电阻或按键触发切换；白天刷新彩屏，夜间只点亮 OLED，并冻结不必要的周期任务。
- **触摸配网**：LVGL 输入界面修改 WiFi 凭据和定位信息，配置保存在外部 Flash。
- **外部资源**：字库和图片存放在 W25Q64，通过 littlefs 按需读取，减少内部 Flash 占用。
- **OTA 升级**：Gitee Releases 版本发现、流式下载、CRC 校验、Bootloader 搬运、启动确认与失败回滚。

---

## 系统架构

### 分层结构

```text
┌──────────────────────────────────────────────────────┐
│ 应用层   User/Application                             │
│          main / Board / App / app_task / Provision    │
│          OTA / OTA_Update                             │
├──────────────────────────────────────────────────────┤
│ 界面层   User/UI                                      │
│          UI_Mode / UI_Day / UI_Night / UI_Input       │
│          OTA_UI / Boot_Page / Main_Page / LVGL 页面   │
├──────────────────────────────────────────────────────┤
│ 任务层   User/Task                                    │
│          ui / net / dht22 / light_sensor / key        │
│          monitor（可选）/ boot_cfm（启动确认）        │
├──────────────────────────────────────────────────────┤
│ 板级层   BSP                                          │
│          LCD / OLED / Touch / AT / OTA_Net / OTA_Store│
│          DHT22 / DS1302 / W25Q64 / Asset / Usart      │
├──────────────────────────────────────────────────────┤
│ 系统层   FreeRTOS / littlefs / LVGL / STM32 标准库    │
├──────────────────────────────────────────────────────┤
│ 硬件层   STM32F407 / ST7789 / SSD1306 / W25Q64 /      │
│          ESP32-C3 / DHT22 / DS1302 / XPT2046          │
└──────────────────────────────────────────────────────┘
```

各层职责：

| 层 | 职责 |
| --- | --- |
| 应用层 | 启动流程、板级初始化、WiFi/时间/天气服务、OTA 提交与备份 |
| 界面层 | 昼夜页面切换、主界面局部刷新、配网/定位输入、OTA 进度浮层 |
| 任务层 | 任务调度、事件分发、传感器采集、网络事务和低功耗模式协作 |
| 板级层 | 屏幕、触摸、Flash、传感器、AT 模组等硬件驱动与资源访问 |
| 系统层 | RTOS 内核、文件系统、图形库和芯片外设库 |
| Bootloader | 上电校验、固件搬运、启动确认和回滚 |

### 任务模型

| 任务 | 优先级 | 栈 | 主要职责 |
| --- | --- | --- | --- |
| `ui` | 3 | 2KB | 开机流程、LCD 唯一写者、昼夜页面切换、OTA 浮层刷新 |
| `dht22` | 4 | 1KB | 单总线温湿度采集，优先级高于 UI 以保证时序 |
| `net` | 2 | 3KB | 独占 USART1：WiFi、SNTP、天气、OTA 下载 |
| `light_sensor` | 2 | 512B | 光照采样、去抖、发起昼夜切换 |
| `key` | 2 | 512B | 按键手势识别 |
| `boot_cfm` | 1 | 512B | 新固件启动 5 秒后写入启动确认，然后自删除 |
| `monitor` | 1 | 512B | 可选内存和任务栈监测，默认关闭 |

任务创建顺序：`App_Task_Init()` 在启动调度器前创建 `boot_cfm` 和 `ui`；`ui` 在开机阶段创建 `dht22` 和 `net`，等网络阶段结束后再创建 `light_sensor`、`key`，最后按配置创建 `monitor`。

### 关键设计约束

- **LCD 只由 `ui` 任务写**。其他任务只更新共享数据并置事件位，避免 SPI3 和显示状态竞争。
- **USART1 只由 `net` 任务使用**。AT 指令是请求—等待模式，OTA 下载期间同样独占串口。
- **共享数据整块替换**。WiFi、天气、室内温湿度和软件时钟在临界区内整体赋值。
- **中断只做通知**。TIM5 累加毫秒，USART1 写入环形缓冲，EXTI 通过任务通知唤醒任务。
- **外部 Flash 统一访问**。资源、配置和 OTA 文件分别位于 littlefs 的不同目录，共用文件系统锁。

### 数据流

```text
ESP32-C3 ──USART1──> net 任务 ──> wifi_info / weather_info ──> ui 任务
                         │
                         ├─ SNTP ──> 软件时钟 ──> 彩屏 / OLED
                         ├─ 公网 IP / 定位 ──> 天气请求
                         └─ OTA_Update ──> Gitee ──> /ota/firmware.tmp

DHT22 ──> dht22 任务 ──> room_info ──> ui 任务

光敏 / 按键 ──> light_sensor / key ──> 事件组 ──> ui 任务切换昼夜模式

W25Q64 ──SPI1──> Asset 层 ──> 字库 / 图片 ──SPI3+DMA──> ST7789
```

---

## Flash 分区与升级

### 内部 Flash 布局

| 区域 | 起始地址 | 大小 | 用途 |
| --- | --- | --- | --- |
| Bootloader | `0x08000000` | 48KB | 上电启动、固件搬运、回滚 |
| 参数区 | `0x0800C000` | 16KB | 启动状态、固件信息、重试次数 |
| App | `0x08010000` | 448KB | 正式应用 |

对应的 Keil 配置：

- Bootloader 工程：IROM 起始 `0x08000000`，大小 `0xC000`。
- App 工程：IROM 起始 `0x08010000`，大小 `0x70000`。

### 启动流程

```text
上电
  └─ Bootloader
       ├─ 读取参数区最新有效记录
       ├─ 普通启动：检查 App 向量表，跳转到 App
       ├─ 有待升级固件：校验 .ota 包，擦除并写入 App 区
       ├─ 有回滚请求：从备份恢复旧固件
       └─ 新固件未确认：重试计数 +1，达到 3 次则回滚

App
  ├─ 修正 VTOR 到 0x08010000
  ├─ 创建 boot_cfm，5 秒后写入启动确认
  └─ 启动 ui 任务，进入正常业务
```

参数区采用日志式追加：32 条 64 字节记录循环使用，每条带序号和 CRC。掉电最多丢失最新一条未写完的记录，不会破坏上一条完整状态。

### OTA 流程

```text
App 检查更新
  └─ GET gitee.com/ssh_zfy/WiFi_Clock/releases/latest
       └─ 解析最新 tag：vX.Y.Z
            └─ 下载 fw_X.Y.Z.ota

下载与校验
  ├─ 通过 ESP-AT SSL/TCP 流式接收
  ├─ 写入 /ota/firmware.tmp，同时计算 CRC32
  ├─ 校验 64B 包头、硬件 ID、版本号和 payload CRC
  └─ 通过后重命名为 /ota/firmware.bin

提交升级
  ├─ 备份当前 App 到 /ota/backup.bin
  ├─ 参数区写入 OTA_PENDING
  └─ 延时后复位

Bootloader 搬运
  ├─ 校验固件包
  ├─ 擦除 App 区并分块写入
  ├─ 回读内部 Flash 再次校验 CRC
  ├─ 写入 APP_UNCONFIRMED
  └─ 跳转到新 App

新 App 启动
  ├─ 5 秒内写入 CONFIRMED 和固件信息
  └─ 未确认累计 3 次：Bootloader 自动回滚到 backup.bin
```

## 目录结构

```text
Project4/
├── MDK/                         App Keil 工程
├── Bootloader/                  Bootloader Keil 工程
├── Boot/                        Bootloader 源码及 OTA 包定义
├── User/
│   ├── Application/             启动、业务服务、OTA、资源烧录
│   ├── Task/                    FreeRTOS 任务
│   ├── UI/                      昼夜界面、输入界面、LVGL 页面
│   └── Resource/                字库和图片的源码备份
├── BSP/                         屏幕、触摸、Flash、传感器、网络驱动
├── Middlewares/
│   ├── FreeRTOS/                RTOS 内核
│   ├── LittleFS/                littlefs 与移植层
│   └── Lvgl/                    LVGL 与显示移植
├── Core/                        CMSIS、启动文件、系统时钟
├── STM32F4xx_StdPeriph_Driver/  STM32F4 标准外设库
├── Documents/                   器件资料和工具脚本
├── ota_pack.py                  App bin → OTA 包打包工具
└── README.md
```

---

## 构建与烧录

1. 使用 Keil MDK 5 打开 `Bootloader/STM32F407_Boot.uvprojx`，编译并烧录到 `0x08000000`。
2. 打开 `MDK/STM32F407.uvprojx`，编译并烧录 App 到 `0x08010000`。
3. 首次使用需要在 `User/Application/Inc/BuildConfig.h` 中开启 `RESOURCE_PROVISION`，分 3 批把字库和图片写入 W25Q64；完成后恢复为 `0` 并重新编译正式固件。
4. 需要串口日志时，将 `DEBUG_SUPPORT` 设为 `1`；正式固件保持 `0`。

---

## 配置入口

| 文件 | 用途 |
| --- | --- |
| `User/Application/Inc/BuildConfig.h` | FreeRTOS/裸机、调试输出、资源烧录、OTA 兜底大小 |
| `User/Task/Inc/Task_Config.h` | 任务优先级、栈大小、周期、按键和低功耗参数 |
| `User/Application/Inc/Version.h` | App 版本号 |
| `Boot/Inc/boot_layout.h` | Flash 分区、OTA 文件路径 |
| `Boot/Inc/ota_pkg.h` | OTA 包头格式、硬件 ID |

---