# STM32F407 桌面天气时钟（Bootloader + OTA）

> 基于 **STM32F407VET6 + FreeRTOS** 的桌面天气时钟 / 室内环境监测终端。
> 白天用 2.4" SPI 彩屏显示时钟、日期、实况天气与室内温湿度；夜间自动切到 0.96" OLED 只显示时间，
> 同时冻结周期任务、让 ESP32-C3 进入 Modem-sleep。
> 时间基准是 **SNTP 校时 + TIM5 推算的软件时钟**，夜间改用 **DS1302 外部 RTC**，断网也能正常走时。
>
> 当前固件还带 **Bootloader + Gitee OTA**：App 从 Gitee Releases 检查新版本，下载 `.ota` 包、
> 校验后写入 W25Q64 并复位；Bootloader 负责搬运到内部 Flash，App 首次启动确认，
> 连续 3 次未确认会自动回滚到上一版。

| 项目 | 内容 |
| --- | --- |
| 主控 | STM32F407VETx（Cortex-M4F @168MHz，512KB Flash，128KB SRAM + 64KB CCM） |
| 操作系统 | FreeRTOS V10.4.3 LTS Patch 3：5 个常驻应用任务 + 可选 monitor + 启动确认临时任务；tick 1kHz、heap_4、42KB 堆 |
| 文件系统 | littlefs v2.11，挂载在 W25Q64（8MB SPI Flash） |
| 联网 | ESP32-C3-MINI-1（AT 固件 v4.1.1.0），USART1 @115200 |
| 显示 | ST7789V 240×320 彩屏（SPI3 + DMA）/ SSD1306 128×64 OLED（软件 I2C） |
| 图形库 | LVGL 8.3.11，只用于 WiFi/定位输入界面和 OTA 浮层 |
| 工具链 | Keil MDK + ARMCC V5.06 update 5（AC5）/ C99 |
| 外置资源 | 11 个字库 + 32 张图片 = 858778 B（≈839KB），全部在 W25Q64 |
| 当前固件 | App 159828 B（156.08KB）；Bootloader 13432 B；App 版本 `1.0.0` |
| 调试输出 | 默认 `DEBUG_SUPPORT=0` 关闭 App 串口日志；Bootloader 始终从 USART2 输出 |

---

## 1. 硬件平台

### 1.1 主控与时钟

| 项目 | 参数 |
| --- | --- |
| MCU | STM32F407VETx（Cortex-M4F，512KB Flash，128KB SRAM，另 64KB CCM） |
| 系统时钟 | 168MHz（HSE 25MHz，PLL_M=25 / PLL_N=336 / PLL_P=2） |
| 总线时钟 | AHB 168MHz · APB1 42MHz · APB2 84MHz |
| 中断分组 | `NVIC_PriorityGroup_4`（4 位抢占、0 位子优先级） |

> 参与时钟树计算的是 `Core/stm32f4xx.h` 里的 `HSE_VALUE`（当前 `25000000`）；Keil 工程中的
> `CLOCK(12000000)` 只影响调试器的 Xtal 显示。换晶振必须同步修改 `HSE_VALUE` 与 `PLL_M`，
> 否则串口波特率、TIM5 计时与 SPI 速率会整体偏移。

### 1.2 引脚分配

| 外设 | 信号 → 引脚 |
| --- | --- |
| ST7789 彩屏 | SCLK=PC10 · MISO=PC11 · MOSI=PC12（SPI3/AF6）· CS=PE2 · RESET=PE3 · DC=PE4 · BLK=PE5 |
| XPT2046 触摸 | 与 LCD 共用 SPI3，CS=PE0 · PENIRQ=PE1 |
| W25Q64 | CS=PA4 · CLK=PA5 · MISO=PA6 · MOSI=PA7（SPI1/AF5） |
| ESP32-C3 | TX=PA9 · RX=PA10（USART1/AF7） |
| 调试串口 | TX=PA2 · RX=PA3（USART2；Bootloader 固定使用，App 仅在 `DEBUG_SUPPORT=1` 时启用） |
| ATGM336H 定位 | TX=PB10 · RX=PB11（USART3/AF7，当前未启用） |
| SSD1306 OLED | SCL=PB6 · SDA=PB7（软件 I2C，开漏 + 内部上拉） |
| DHT22 | DATA=PE6（单总线） |
| DS1302 | RST=PE7 · IO=PE8 · CLK=PE9 |
| 光敏电阻 | DO=PC1（EXTI1 双沿）· AO=PC0（ADC123_IN10） |
| 用户按键 | PA0（下拉输入 + EXTI0 双沿） |
| 测试 LED | PC5（RTOS）/ PB2（裸机），相关代码当前已注释，未使用 |

### 1.3 外设配置

| 外设 | 关键参数 |
| --- | --- |
| USART1 | 115200-8N1，RXNE 中断 + 512B 环形缓冲，NVIC 抢占 5 |
| USART2 | 115200-8N1，Bootloader 日志固定输出；App 调试模式下 TX 走 DMA1_Stream6/Ch4 |
| USART3 | 9600-8N1，预留；当前代码整段注释，ATGM336H 未接入 |
| SPI1 | 主机 · 8 位 · Mode 0 · 预分频 4 → **21MHz** |
| SPI3 | 主机 · Mode 0 · 预分频 2 → **21MHz**，TX 走 DMA1_Stream5/Ch0 |
| SPI3（触摸） | 事务期间临时切到预分频 64（约 **656kHz**），结束恢复 21MHz |
| TIM5 | PSC=83（1MHz 计数）· ARR=999（1ms 中断）· NVIC 抢占 **2** |
| ADC1 | 仅光敏 AO 模式使用，当前 `AO_DO_SWITCH = 0` 不参与编译 |
| CCM | 使能 CCMDATARAMEN，LVGL 24KB 内存池固定在 `0x10000000` |

> SPI1 与 SPI3 同为 21MHz 是图片乒乓双缓冲能把 W25Q64 读取时间隐藏在 SPI3 发送之后的前提，
> 改动任一侧分频前需一并评估。

---

## 2. 代码架构

### 2.1 分层

```text
应用层      User/Application/      main / Board / App / app_task / Provision / bare_test /
                                  OTA / OTA_Update / stm32f4xx_it
界面层      User/UI/               UI_Mode / UI_Day / UI_Night / UI_Input / OTA_UI /
                                  Boot_Page / Main_Page / LVGL 屏幕
任务层      User/Task/             UI / Net / DHT22 / LightSensor / Key / Monitor +
                                  Task_Common / Task_Config
资源层      BSP/Asset.c           littlefs 挂载与自检、字库/图片只读通道、运行数据(/data)键值存取
板级驱动    BSP/                  LCD · OLED · Touch · AT · OTA_Net · OTA_Store · DHT22 ·
                                  External_RTC · Light_Sensor · Key · Timer · Usart · W25Q64 ·
                                  I2C · Profiling
Bootloader  Boot/                 boot_main / boot_param / boot_update / boot_lfs / boot_uart /
                                  boot_crc32 / boot_jump / ota_pkg
操作系统    Middlewares/FreeRTOS/  内核 + portable/FreeRTOSConfig.h
文件系统    Middlewares/LittleFS/ littlefs v2.11 + lfs_port + lfs_operation
图形库      Middlewares/Lvgl/     LVGL 8.3.11 + lv_conf.h + porting
资源数据    User/Resource/        字库与图片的源码内备份（正式固件不编译）
硬件抽象    Core/ + SPL           CMSIS + STM32F4 标准外设库
```

依赖方向**自上而下单向**：应用层 → 界面/任务层 → 资源层/板级驱动 → FreeRTOS/littlefs → 硬件抽象。
Bootloader 独立编译，只依赖 `Boot/`、SPL、W25Q64 和只读 littlefs，不链接 App 代码。

### 2.2 目录结构

```text
Project4/
├── MDK/                         App Keil 工程（STM32F407.uvprojx）
├── Bootloader/                  Bootloader Keil 工程 + 输出 bin
├── Boot/                        Bootloader 源码（App 也复用 boot_param/boot_crc32/ota_pkg）
│   ├── Inc/                     boot_layout / boot_param / boot_update / boot_lfs /
│   │                            boot_uart / boot_crc32 / boot_jump / ota_pkg
│   └── Src/                     对应 .c
├── Core/                        CMSIS + 启动文件 + 系统时钟
├── STM32F4xx_StdPeriph_Driver/  STM32F4 标准外设库
├── Middlewares/
│   ├── FreeRTOS/                内核 + include/ + portable/
│   ├── LittleFS/                littlefs v2.11 + lfs_port + lfs_operation
│   └── Lvgl/                    LVGL 8.3.11 + lv_conf.h + porting/
├── BSP/
│   ├── Inc/                     Asset · LCD · Touch · OLED · AT · OTA_Net · OTA_Store ·
│   │                            DHT22 · External_RTC · Light_Sensor · Key · Timer ·
│   │                            Usart · W25Q64 · I2C · Profiling
│   └── Src/                     对应 .c
├── User/
│   ├── Application/             main / Board / App / app_task / Provision / bare_test /
│   │                            OTA / OTA_Update / Version.h / BuildConfig.h
│   ├── Task/                    UI_Task / Net_Task / DHT22_Task / LightSensor_Task /
│   │                            Key_Task / Task_Monitor + Task_Common / Task_Config
│   ├── UI/                      UI_Mode / UI_Day / UI_Night / UI_Input / OTA_UI /
│   │                            Boot_Page / Main_Page / ui.c / ui_events.c / screens/
│   └── Resource/                Font.h / Image.h + 字库/图片源码
├── Documents/                   本说明、器件手册、Bootloader/OTA 文档、工具
├── ota_pack.py                  App .bin → .ota 打包工具
└── KeilClear.bat                清理构建产物
```

> 任务层在 `User/Task/`，不是项目根的 `Task/`；Bootloader 在 `Boot/`，Keil 工程在 `Bootloader/`。

### 2.3 模块职责

**应用层（`User/Application`）**

| 文件 | 职责 |
| --- | --- |
| `main.c` | 三种构建模式的唯一入口；断言与栈溢出钩子 |
| `Board.c` | 板级初始化：外设时钟、TIM5、USART2（调试模式）、触摸 CS park、LCD、Asset、OLED |
| `App.c` | 服务层：WiFi / SNTP / 天气 / 温湿度 / 软件时钟、凭据与定位存储 |
| `app_task.c` | 启动前修正 VTOR、创建 5 秒确认任务，再创建 `ui` |
| `OTA.c` | App 侧提交/备份/自检：`OTA_CommitFirmware()`、`Backup_Current()`、`OTA_CalcAppInfo()` |
| `OTA_Update.c` | 网络升级主流程：Gitee 版本发现、下载、包头/CRC 校验、提交 |
| `Provision.c` | 资源烧录：把源码备份的字库/图片写入 W25Q64 |
| `bare_test.c` | 裸机模块测试：OLED / 光敏 / DS1302 / W25Q64 / littlefs / TOUCH |
| `stm32f4xx_it.c` | 异常与中断桩 |

**Bootloader（`Boot/`）**

| 文件 | 职责 |
| --- | --- |
| `boot_main.c` | 入口、参数状态机、App 快速校验、跳转 |
| `boot_param.c` | 参数区日志式存储：32 条 × 64B、seq/CRC、确认与版本信息 |
| `boot_update.c` | 从 `firmware.bin` 搬运：校验包、写 `OTA_WRITING`、擦 Sector 4~7、4KB 写入、回读 CRC |
| `boot_lfs.c` | 只读挂载 W25Q64 上的 littlefs，打开 OTA 文件 |
| `boot_uart.c` | USART2 轮询日志 |
| `boot_crc32.c` | 标准 CRC32（与 PC 端 zlib 一致） |
| `boot_layout.h` | Flash 分区、`/ota` 文件路径 |
| `ota_pkg.h` | `.ota` 包头格式 |

**界面层（`User/UI`）**

| 文件 | 职责 |
| --- | --- |
| `UI_Mode.c/.h` | 昼夜模式门面：切换时序、主循环等待节拍、事件分派 |
| `UI_Day.c/.h` | 白天 LCD 主页面、顶部状态条、时钟/天气/温湿度局部刷新 |
| `UI_Night.c/.h` | 夜间 OLED、RTC 同步与分钟级刷新 |
| `UI_Input.c/.h` | 触摸校准、左上角图标热区、LVGL 配网/定位界面 |
| `OTA_UI.c/.h` | OTA 进度浮层：Checking / Downloading / Verifying / 错误提示 |
| `Boot_Page.c` / `Main_Page.c` | 开机页与主页面裸绘；`Page.h` 保存布局常量 |
| `ui.c` / `ui_events.c` / `ui_helpers.c` / `screens/` | SquareLine 生成的 LVGL 界面；`ui_WiFi.c` 里有 Check Update 按钮和版本标签 |

**任务层（`User/Task`）**

| 文件 | 职责 |
| --- | --- |
| `Task_Common.c/.h` | 事件组 `Task_Events()`、低功耗标志 `g_lowpower`、16 个事件位 |
| `Task_Config.h` | 优先级、栈大小、周期、去抖、按键时序、monitor 参数 |
| `UI_Task.c/.h` | `ui` 任务：开机流程、事件循环、LCD 唯一写者、OTA 浮层刷新 |
| `Net_Task.c/.h` | `net` 任务：AT/WiFi/SNTP/天气/OTA，对外提供开机结果 |
| `DHT22_Task.c/.h` | `dht22` 任务：单总线温湿度周期采集 |
| `LightSensor_Task.c/.h` | `light_sensor` 任务：光照去抖、昼夜状态机 |
| `Key_Task.c/.h` | `key` 任务：PA0 手势状态机 |
| `Task_Monitor.c/.h` | 可选内存监测；`BuildConfig.h` 的 `MONITOR_TASK_ENABLE=1` 时创建 |

**板级驱动（`BSP/Src`）**

| 文件 | 职责 |
| --- | --- |
| `Asset.c` | 资源层：挂载 littlefs、自检、字库/图片只读、运行数据键值存取 |
| `LCD.c` | ST7789：SPI3 + DMA、字模渲染、图片流式读取与乒乓双缓冲、总线锁 |
| `Touch.c` | XPT2046：滤波坐标、五点校准、校准参数存取 |
| `OLED.c` | SSD1306：命令/数据、按页清屏、ASCII 夜钟 |
| `AT.c` | ESP32-C3 AT 指令、WiFi/SNTP/HTTP、天气/IP 解析 |
| `OTA_Net.c` | 基于 ESP-AT 的 SSL/TCP 流式 HTTP 客户端，支持 302 跟跳和 chunked |
| `OTA_Store.c` | `/ota` 文件读写、rename、删除、剩余空间 |
| `DHT22.c` / `External_RTC.c` | 单总线温湿度；DS1302 读写与双读比对 |
| `Light_Sensor.c` / `Key.c` | 光敏 DO/AO；PA0 双边沿上报 |
| `Timer.c` / `Usart.c` | TIM5 1ms 基准；USART1 环形缓冲、USART2 调试输出 |
| `W25Q64.c` / `I2C.c` / `Profiling.c` | SPI Flash、软件 I2C、DWT 计时 |

### 2.4 关键约定

1. **LCD 只由 `ui` 任务写**（`UI_Day` / `UI_Input` / `OTA_UI` 都在该任务内）。其他任务只更新数据并置事件位。
2. **USART1（AT）只由 `Net_Task` 使用**。AT 是“请求—等待”模式，第二个使用者必然导致串扰；OTA 下载期间同样独占。
3. **共享数据整块替换**：`wifi_info`、`weather_info`、`room_info`、软件时钟都在临界区内整体赋值。
4. **中断只做通知**：TIM5 只累加毫秒；USART1 只入环形缓冲；EXTI/ADC 只发任务通知。
5. **外部 Flash 只由 `Asset.c` / `OTA_Store.c` 访问**：资源只读，`/data` 存配置，`/ota` 存固件；所有访问共用 `LittleFS_Lock()`。
6. **SPI3 和 littlefs 都有递归互斥量**：LCD/触摸共用 SPI3；W25Q64 整棵文件树共用 littlefs 锁。
7. **App 启动先修正 VTOR**：`SystemInit()` 会把 VTOR 设回 `0x08000000`，`app_task.c` 必须重新指向 `APP_BASE`。
8. **App 必须确认启动**：新固件启动 5 秒后写 `CONFIRMED`/自身 size/crc/version，否则 Bootloader 累加 `boot_try`，3 次未确认回滚。
9. **OTA 文件先写 tmp 再 rename**：Bootloader 永远看不到写了一半的固件；资源烧录批次 1 会格式化 littlefs，也会清掉 `/ota`。

---

## 3. 任务、事件与中断

### 3.1 任务一览

| 任务 | 优先级 | 栈 | 职责 | 循环节拍 |
| --- | --- | --- | --- | --- |
| `UI_Task` / `"ui"` | 3 | 2KB | 开机流程、**LCD 唯一写者**；昼夜切换经 `UI_Mode`，LVGL 与 OTA 浮层由 `UI_Input` / `OTA_UI` 承载 | 白天 50ms / 夜间 2000ms / 键盘界面 10ms |
| `DHT22_Task` / `"dht22"` | 4 | 1KB | 单总线温湿度采集，成功后置 `EV_DHT22` | 5min |
| `Net_Task` / `"net"` | 2 | **3KB** | **独占 USART1/AT**：WiFi、SNTP、天气；处理 `EV_OTA_CHECK` 时阻塞执行 OTA | 1s |
| `LightSensor_Task` / `"light_sensor"` | 2 | 512B | 光照判定、2s 去抖、发起昼夜切换并等 ACK | 事件驱动 + 1s 兜底 |
| `Key_Task` / `"key"` | 2 | 512B | PA0 手势状态机（单击/双击/三击/长按） | 事件驱动 |
| `Monitor_Task` / `"monitor"` | 1 | 512B | 可选内存监测（`MONITOR_TASK_ENABLE=1`）：堆用量与任务栈峰值，夜间静默 | 5s / 夜间 60s |
| `Boot_Confirm_Task` / `"boot_cfm"` | 1 | 512B | 启动 5 秒后写 `CONFIRMED` 和自身 size/crc/version，完成后删除自己 | 一次性 |

`net` 栈从 1KB 提升到 3KB，是因为 OTA 调用链（`OTA_Update_Run` + `OTA_Net` 请求缓冲 + printf）
实测需要约 2KB；原 1KB 会溢出并踩坏相邻内存。

**创建时机**：`main()` 只创建 `ui`；`app_task.c` 先创建 `boot_cfm`，再启动 `ui`。
`ui` 在开机流程中创建 `dht22`/`net`，等 `EV_NET_READY`（≤60s）并显示结果页后，
才创建 `light_sensor`/`key`，最后按开关创建 `monitor`。这样昼夜切换只可能发生在开机流程之后。

**优先级**：`DHT22`（4）高于 `UI`（3），因为单总线对微秒级时序敏感；`net`（2）低于 `UI`，
因为一次 AT/OTA 事务可能持续数十秒，优先级过高会把显示饿死；`net`/`light_sensor`/`key`
同为 2 且 `configUSE_TIME_SLICING=0`，所以三者都必须主动阻塞。

### 3.2 事件位

统一定义在 `User/Task/Inc/Task_Common.h`，共 16 位：

| 位 | 名称 | 置位者 → 消费者 | 含义 |
| --- | --- | --- | --- |
| bit0 | `EV_WEATHER` | `Net_Task` → `UI_Task` | 天气已更新 |
| bit1 | `EV_DHT22` | `DHT22_Task` → `UI_Task` | 室内温湿度已更新 |
| bit2 | `EV_NET_READY` | `Net_Task` → `UI_Task` | 开机网络阶段结束（无论成败） |
| bit3 | `EV_WIFI` | `Net_Task` → `UI_Task` | WiFi 状态变化 |
| bit4 | `EV_LOWERPOWER` | `LightSensor_Task` → `UI_Task` | 请求进入夜间 |
| bit5 | `EV_LOWPOWER_ACK` | `UI_Task` → `LightSensor_Task` | 昼夜切换执行完毕 |
| bit6 | `EV_WAKEUP` | `LightSensor_Task` → `UI_Task` | 请求回到白天 |
| bit7 | `EV_NET_UPDATE_NOW` | `UI_Mode` → `Net_Task` | 退出夜间后立即补网络更新 |
| bit8 | `EV_DHT22_UPDATE_NOW` | `UI_Mode` → `DHT22_Task` | 退出夜间后立即补温湿度采集 |
| bit9 | `EV_WIFI_KB` | `UI_Input` → `UI_Input` | 点了 WiFi 图标，调出配网界面 |
| bit10 | `EV_MAIN_SCREEN` | 输入界面回调 → `UI_Input` | 确认/取消，退回主界面 |
| bit11 | `EV_WIFI_CRED` | `UI_Input` → `Net_Task` | 提交了新 WiFi 凭据，请重连 |
| bit12 | `EV_LOCATION_KB` | `UI_Input` → `UI_Input` | 点了定位图标，调出定位输入界面 |
| bit13 | `EV_LOCATION_CRED` | `UI_Input` → `Net_Task` | 提交了新定位，立即按新定位取天气 |
| bit14 | `EV_OTA_CHECK` | `UI_Input`/`ui_WiFi` → `Net_Task` | 用户点了 Check Update |
| bit15 | `EV_OTA_DONE` | `Net_Task` → 预留 | OTA 流程结束；UI 通过 `g_ota_prog` 轮询结果 |

> 旧代码里的 `EV_KEYBOARD` 已改名为 `EV_WIFI_KB`。
> `EV_NET_UPDATE_NOW` 和 `EV_DHT22_UPDATE_NOW` 必须分开：`xEventGroupWaitBits` 是清除式等待，
> 共用一个位时先被唤醒的会把位清掉。

### 3.3 同步手段

| 机制 | 用途 |
| --- | --- |
| 事件组 `Task_Events()` | 16 个事件位，任务间主要通信方式 |
| 任务通知（计数语义） | ISR → 任务的光敏/按键边沿；按键“切换昼夜”请求 |
| 临界区 `taskENTER_CRITICAL()` | 软件时钟与共享结构体的整块赋值（`App.c`） |
| 互斥量 | 调试串口整行输出、LCD/触摸共用的 SPI3 递归锁、littlefs 文件树递归锁 |

未使用队列、信号量做业务数据传递，也未使用 `vTaskSuspend` 做同步；`configUSE_TIMERS=0`。

### 3.4 中断与 FreeRTOS 配置

| 中断源 | 抢占优先级 | 可否调用 FreeRTOS API | ISR 内动作 |
| --- | --- | --- | --- |
| PendSV / SysTick | 15 | — | 内核上下文切换 |
| **TIM5**（1ms 时基） | **2** | ❌ 禁止 | 仅 `TIM5_ms++` |
| USART1（AT 接收） | 5 | ✅ | 逐字节入 512B 环形缓冲 |
| EXTI0 / EXTI1 / ADC1 AWD | 5 | ✅ | `vTaskNotifyGiveFromISR` + `portYIELD_FROM_ISR` |
| USART2 / DMA1_Stream5,6 | — | — | 未配置 NVIC，靠轮询完成标志 |

`configMAX_SYSCALL_INTERRUPT_PRIORITY = 5 << 4`；TIM5 优先级 2，ISR 内禁止任何 RTOS 调用。

| 配置 | 值 |
| --- | --- |
| `configTICK_RATE_HZ` | 1000 |
| `configMAX_PRIORITIES` | 5（可用 0~4） |
| `configUSE_TIME_SLICING` | 0 |
| `configTOTAL_HEAP_SIZE` | `1024 * 42` = **43008 B**（heap_4） |
| `configMINIMAL_STACK_SIZE` | 128 word（IDLE 512B） |
| `configCHECK_FOR_STACK_OVERFLOW` | 2 |
| `configUSE_TRACE_FACILITY` | 1 |
| `configUSE_TIMERS` / `MUTEXES` / `RECURSIVE_MUTEXES` | 0 / 1 / 1 |
| `configUSE_EVENT_GROUPS` | 1 |
| `configTASK_NOTIFICATION_ARRAY_ENTRIES` | 3 |
| `configSUPPORT_DYNAMIC_ALLOCATION` / `STATIC` | 1 / 0 |

### 3.5 数据流

```text
ESP32-C3 ──USART1──> AT.c ──> wifi_info ──> 顶部状态条 / 开机结果页
                       │
                       ├─> AT_SNTP_Get_Time ─> Clock_Sync ─> 软件时钟（TIM5 推算）
                       │                                      └─> 主界面 / OLED 时钟
                       ├─> AT_Get_IP ─> 公网 IP ─> 天气 URL 的 location=<IP>
                       ├─> AT_Get_HTTP ─> Parse_Http_Response ─> weather_info ─> 天气卡片
                       └─> OTA_Update_Run ─> OTA_Net ─> Gitee/附件 CDN ─> /ota/firmware.tmp

天气定位：
  用户输入（省+市，优先）──取不到/没输入──> 公网 IP ──查不到──> 字面量 ip

DHT22 ──> DHT22_ReadData ──> room_info ──> 室内温湿度卡片
光敏 EXTI1 ──> LightSensor_Task（2s 去抖）──> 事件组 ──> UI_Mode 切屏
按键 EXTI0 ──> Key_Task（手势）──> 长按 ──> LightSensor_Task（统一裁决）

资源：
  W25Q64 ──SPI1 21MHz──> Asset 层 ──┬─> 字模：区位码直接算偏移，O(1)
                                    └─> 图片：分块流式读取 + DMA1_Stream5 乒乓 ──SPI3──> ST7789
```

---

## 4. Flash 分区与 Bootloader

### 4.1 内部 Flash 分区

STM32F407VET6 的 512KB Flash 按扇区划分，Bootloader 与参数区不能共用扇区：

| 区域 | 地址 | 大小 | 说明 |
| --- | --- | --- | --- |
| Bootloader | `0x08000000` | 48KB（Sector 0~2） | 上电第一段代码 |
| 参数区 | `0x0800C000` | 16KB（Sector 3） | 32 条 × 64B 日志记录 |
| App | `0x08010000` | 448KB（Sector 4~7） | 正式应用 |

对应 Keil 配置：

- App 工程：IROM `0x08010000`，大小 `0x70000`；宏 `APP_BUILD`；
- Bootloader 工程：IROM `0x08000000`，大小 `0xC000`；宏 `BOOT_BUILD,LFS_READONLY`。

### 4.2 参数区：日志式追加

参数区不是“覆盖写一个结构体”，而是 32 条 64B 记录循环追加：

- 每条包含 `magic='BOOT'`、`seq`、`layout`、`state`、`app_size/crc/version`、
  `ota_size/crc/version`、`boot_try`、`flags`、`crc32`；
- 写入找第一条全 `0xFF` 的空槽，`seq++`；读取扫全部 32 条，取 CRC 有效且 `seq` 最大者；
- 写满 32 条后整扇区擦除，从槽 0 重新开始；
- 掉电最坏只会丢“最新一条未写完的记录”，读回上一条完整记录，状态最多倒退一步。

状态机：

```text
IDLE -> OTA_PENDING -> OTA_WRITING -> APP_UNCONFIRMED -> IDLE
                                      └── 3 次未确认 -> ROLLBACK -> IDLE
无可用固件 -> RECOVERY（当前未实现串口救援，halt）
```

### 4.3 上电启动流程

```text
上电
  └─ Bootloader @0x08000000
       ├─ 初始化 USART2，打印 [BOOT] 日志
       ├─ Boot_Param_Load() 找最新有效记录
       ├─ 按 state 分支：
       │    IDLE / RECOVERY      App_QuickValid() 通过则跳转
       │    OTA_PENDING/WRITING  从 /ota/firmware.bin 搬运
       │    ROLLBACK             从 /ota/backup.bin 恢复
       │    APP_UNCONFIRMED      校验 App、boot_try++，达 3 次回滚
       └─ Boot_JumpToApp()：关中断 → 关 SysTick → 清 NVIC → 关 USART2
                             → VTOR=APP_BASE → MSP=App 栈顶 → 跳转

App 启动
  ├─ SystemInit() 把 VTOR 改回 0x08000000
  ├─ App_Task_Init()：Boot_Apply_VectorTable() 重新指向 APP_BASE
  ├─ 创建 boot_cfm（优先级 1，5 秒后写 CONFIRMED/size/crc/version）
  └─ 创建 ui 任务，进入正常业务
```

> `Boot_JumpToApp()` 必须按顺序设置 VTOR、MSP 并最后跳转；App 侧还要再设一次 VTOR，
> 因为 `SystemInit()` 会把它改回 `0x08000000`。两边各设一次，谁都不能省。

### 4.4 升级搬运与回滚

Bootloader 收到 `OTA_PENDING/WRITING` 后执行 `Update_Run()`：

1. 只读挂载 littlefs，打开 `/ota/firmware.bin`；
2. 读 64B 包头，校验 `magic/header_size/hw_id`，`fw_size <= APP_SIZE`；
3. 整包 payload CRC32 校验；
4. 写参数 `state=OTA_WRITING`（掉电重入标记）；
5. 擦除 Sector 4~7；
6. 跳过包头，4KB 一块写入 App 区；
7. 直接回读内部 Flash 再算 CRC，确认写入没有位错误；
8. 写参数 `APP_UNCONFIRMED + boot_try=1 + app_size/crc/version`；
9. 跳转新 App。

App 5 秒内未写 `CONFIRMED`，Bootloader 下次启动会 `boot_try++`；累计 3 次回滚到
`backup.bin`。回滚成功后直接 `IDLE + CONFIRMED`，不再要求重新确认。

**当前失败策略**：任何 `halt` 都只能靠 ST-Link 重新烧录；串口救援（`RECOVERY`）和
IWDG 看门狗尚未实现，见 [待办.md](待办.md)。

---

## 5. OTA 网络升级

### 5.1 入口与界面

WiFi 配网界面 `ui_WiFi` 里手动追加了：

- **Check Update** 按钮：点击置 `EV_OTA_CHECK`；
- **FW v1.0.0** 版本标签：来自 `Version.h`，随固件变化。

`Net_Task` 收到 `EV_OTA_CHECK` 后：

- WiFi 未连接：直接把 `g_ota_prog.state` 置 `OTA_UPD_ERR_NET`；
- 已连接：阻塞执行 `OTA_Update_Run()`（下载期间独占 AT 模组），结束后置 `EV_OTA_DONE`。

UI 侧不等待事件，而是在 `ui` 主循环每拍调用 `OTA_UI_Tick()` 读取 `g_ota_prog`：

| 状态 | 显示 |
| --- | --- |
| `CHECKING` | `Checking update...` |
| `DOWNLOADING` | 进度条 + `Downloading xx%`；当前下载长度未知，实际显示已下载 KB 数 |
| `APPLYING` | `Verifying & backup...` |
| `REBOOTING` | `Rebooting...`（预留状态；当前提交后延时 1s 直接复位，通常看不到） |
| `UP_TO_DATE` / 错误 | 结果文本，3 秒后自动回 `IDLE` 并退回 WiFi 页 |

### 5.2 版本发现与发布约定

设备不把下载地址写死，而是问 Gitee “当前最新 tag 是哪个”：

```text
GET https://gitee.com/ssh_zfy/WiFi_Clock/releases/latest
  -> 302 Location: https://gitee.com/ssh_zfy/WiFi_Clock/releases/tag/v1.0.7
  -> 从 Location 抠出 tag v1.0.7
  -> 拼出 https://gitee.com/ssh_zfy/WiFi_Clock/releases/download/v1.0.7/fw_1.0.7.ota
```

发布约定（必须严格对应）：

1. tag 形如 `v1.0.7`：三段十进制、不带前导零、版本单调递增；
2. 附件名 `fw_1.0.7.ota`：`"fw_" + tag 去掉 'v' + ".ota"`；
3. 两项不一致会 404；先发高版本再补低版本，`releases/latest` 可能仍指向高版本。

`Version.h` 是唯一版本源；打包时 `--version` 必须与 tag 对应：

```bash
py ota_pack.py MDK\Output\STM32F407.bin fw_1.0.7.ota --version 1.0.7
```

### 5.3 下载与校验

`OTA_Net.c` 不使用 `AT+HTTPCLIENT`（它会把整个响应缓存在模组里，150KB 固件放不下），
而是用 `AT+CIPSTART`（SSL/TCP）+ `AT+CIPSEND` 手写 HTTP/1.1 GET：

- 流式解析 `+IPD` 数据，边收边写 `/ota/firmware.tmp`、边算 CRC32；
- 支持 `Content-Length` 和 `chunked`；
- 跟随 301/302/307/308，最多 3 跳；
- 会话总超时 60s，两次数据间超时 10s；连接失败会打印 `AT+GMR`、`AT+PING`、
  `TCP:80` 诊断日志（`DEBUG_SUPPORT=1` 时可见）。

校验项：

- 64B 包头：`magic='OTA1'`、`header_size=64`、`hw_id=0x04074001`；
- `fw_size` 与 tag/下载长度一致；
- 包内 `fw_version` 必须等于最新 tag 解析出的版本；
- payload CRC32（zlib 标准反射）与包头一致。

### 5.4 提交、搬运与回滚

```text
App（网络下载完成，校验通过）
  1. tmp -> /ota/firmware.bin（littlefs rename，原子）
  2. 备份当前 App -> /ota/backup.bin（.ota 格式）
  3. 写参数 state=OTA_PENDING，记录 ota_size/crc/version，置 BACKUP_VALID
  4. 延时 1s，NVIC_SystemReset()

Bootloader（下次上电）
  5. 读包校验 -> state=OTA_WRITING -> 擦 App 区 -> 4KB 搬运 -> 回读 CRC
  6. 写参数 APP_UNCONFIRMED + boot_try=1 -> 跳转新 App

App（新固件启动）
  7. 5s 后写 CONFIRMED + app_size/crc/version
     失败/崩溃：boot_try 累加，3 次未确认 -> ROLLBACK 到 backup.bin
```

设计要点：

- **先校验、后擦除**：包不合法就不动 App 区，避免“旧固件没了、新固件也不能用”；
- **先写参数、后动手**：`OTA_WRITING` 先落盘，搬运中途掉电后重启会从擦除开始重做；
- **tmp + rename**：Bootloader 不会读到半截文件；
- **回读校验**：写完内部 Flash 后再算一遍 CRC，排除写入位错误；
- **确认窗口 5 秒**：过滤“启动即崩溃”的固件；3 次仍未确认就回滚。

---

## 6. 显示、输入与资源

### 6.1 显示与触摸

- 白天主页面由 `UI_Day` 调用 `Main_Page.c` 裸绘；LCD 为 240×320 RGB565，SPI3 + DMA。
- 夜间 `UI_Night` 使用 SSD1306 128×64 OLED，只显示 ASCII 时间和日期。
- LVGL 使用 24KB 内存池（CCM `0x10000000`），绘制缓冲借用 LCD 的 240×48 渲染缓冲；
  `LV_COLOR_16_SWAP=0`，flush 直接交给 `LCD_Draw_Pixels()`。
- 触摸为 XPT2046，五点校准后把 `kx/ky/bx/by` 写入 `/data/touch_cal.bin`；未校准时按原始坐标
  比例折算。`Board_Init()` 在 LCD 初始化前先调用一次 `Touch_Init()` 拉高 CS，防止 XPT2046
  误收 LCD 初始化命令；这次调用因 SPI3 尚未使能会返回 false，属于预期的 park 行为，
  LCD 就绪后再由 `UI_Input_Calibrate()` 成功初始化。

### 6.2 输入界面

主页面仍是裸绘，LVGL 只接管“点了左上角图标之后”的输入界面：

| 主页图标 | 屏幕 | 输入 | 事件位 | 保存到 |
| --- | --- | --- | --- | --- |
| WiFi `(5,5)` 20×20 | `ui_WiFi` | SSID + 密码 | `EV_WIFI_KB` | `/data/wifi_cfg.bin` |
| 定位 `(26,5)` 20×20 | `ui_location` | 省+市 | `EV_LOCATION_KB` | `/data/location.bin` |

- 白天主循环 50ms 轮询触摸热区；输入界面激活后改为 10ms 心跳，喂 `lv_tick_inc()` 并调
  `lv_timer_handler()`；
- 确认时只改 RAM，由 `UI_Input_HandleEvent()` 在收到 `EV_MAIN_SCREEN` 时落盘；
- `EV_WIFI_CRED` 让 `Net_Task` 立即用新凭据重连，`EV_LOCATION_CRED` 让天气立即按新定位更新；
- 中文定位仍受 UTF-8/GB2312 不匹配影响，详见已知限制。

### 6.3 W25Q64 资源

字模与图片不放在 MCU Flash 里，而是烧到 W25Q64 上由 littlefs 管理，MCU 内只保留描述表。

| 内容 | 大小/数量 |
| --- | --- |
| 字库 | 11 个，420050 B；`cn16/cn22` 覆盖 GB2312 一级汉字 3755 字 |
| 图片 | 32 个，438728 B；2 张全屏图 + 30 张图标 |
| 资源合计 | **858778 B**（占 8MB 的 10.2%），开机自检 43 项 |
| `/data` | WiFi 凭据、定位、触摸校准等运行配置 |
| `/ota` | `firmware.bin`、`backup.bin`、`.tmp` 下载/备份临时文件 |

图片格式为 4B 宽高头 + RGB565 小端；字模按 GB2312 区位码/ASCII 码直接算偏移，O(1) 读取。
`Asset.c` 挂载失败时不会自动格式化，避免清空已烧录资源。

---

## 7. 构建、烧录与发布

### 7.1 两个 Keil 工程

| 工程 | 路径 | 起始地址/大小 | 宏 | 输出 |
| --- | --- | --- | --- | --- |
| Bootloader | `Bootloader/STM32F407_Boot.uvprojx` | `0x08000000` / 48KB | `BOOT_BUILD,LFS_READONLY` | `Bootloader/Output/STM32F407_Boot.bin` |
| App | `MDK/STM32F407.uvprojx` | `0x08010000` / 448KB | `APP_BUILD` | `MDK/Output/STM32F407.bin` |

App 工程的 `BOOT` 组包含 `boot_param.c`、`boot_crc32.c`，用于参数区读写和 CRC 自检；
Bootloader 工程只包含 `Boot/`、SPL、W25Q64 和只读 littlefs。

当前构建的体积参考（`MDK/Output/STM32F407.map`）：App 为 Code 139616 + RO-data 19936、
RW-data 1100 + ZI-data 95044，即 Flash bin **159828 B**、主 SRAM **96144 B（93.89KB，不含 CCM 的
LVGL 24KB 池）**，主 SRAM 还剩约 **34.1KB**；Bootloader bin 为 **13432 B**，48KB 分区余量充足。

### 7.2 烧录顺序

```text
1. 烧 Bootloader 到 0x08000000（Bootloader 工程）
2. 烧 App 到 0x08010000（App 工程）
3. 首次使用：用 App 工程切到资源烧录模式，按批次 1→2→3 写 W25Q64
4. 改回正式固件，重新烧 App
```

注意事项：

- 不要整片擦除后只烧 App：Bootloader 和参数区会一起丢失；
- 参数区在 `0x0800C000`，正常下载 App 不会碰它；
- 资源烧录批次 1 会格式化 littlefs，**会清掉 `/ota` 下的固件/备份**，建议先烧资源再做 OTA；
- App 版本、Gitee tag、`.ota` 包版本三者必须一致。

### 7.3 构建开关

`User/Application/Inc/BuildConfig.h`：

| 开关 | 默认 | 作用 |
| --- | --- | --- |
| `USE_FREERTOS` | 1 | 1=RTOS，0=裸机测试 |
| `DEBUG_SUPPORT` | 0 | 0=无 App 串口日志；1=USART2 DMA 日志 |
| `MONITOR_TASK_ENABLE` | 0 | 1=创建内存监测任务 |
| `RESOURCE_PROVISION` | 0 | 1=资源烧录固件 |
| `PROVISION_BATCH` | 1 | 资源烧录批次 1~3 |
| `RESOURCE_DATA_IN_ROM` | 0 | 1=把资源数据编译进固件（烧录时自动置 1） |
| `APP_FW_SIZE_FALLBACK` | 160KB | 参数区无 app_size 时的备份长度兜底 |
| `OTA_GITEE_OWNER/REPO` | `ssh_zfy/WiFi_Clock` | OTA 更新源 |
| `TOUCH_FORCE_CALIBRATE` | 0 | 1=每次上电强制重新校准触摸 |

### 7.4 资源烧录

资源总量约 839KB，超过 512KB Flash，必须分三批：

| 批次 | 内容 | 字节数 |
| --- | --- | --- |
| 1 | 汉字库 16 + 22 | 367990 |
| 2 | 开机全屏图 + 主页全屏图 | 307208 |
| 3 | 9 张 ASCII 表 + 30 张图标 | 183580 |

每批改 `PROVISION_BATCH`、保持 `RESOURCE_PROVISION=1`，编译烧录到 App 区；批次 1 会格式化
文件系统，必须按 1→2→3 烧录。完成后把 `RESOURCE_PROVISION` 改回 0，重烧正式 App。

### 7.5 发布 OTA 包

```bash
# 1. 改 Version.h 的 APP_VERSION_*
# 2. 编译 App，得到 MDK/Output/STM32F407.bin
py ota_pack.py MDK\Output\STM32F407.bin fw_1.0.7.ota --version 1.0.7

# 3. 在 Gitee 建 Release：
#    tag: v1.0.7
#    附件: fw_1.0.7.ota
```

设备端会自动访问 `/releases/latest` 发现该 tag，不需要改代码。

### 7.6 调试

- Bootloader 始终从 USART2（PA2/PA3，115200-8N1）输出 `[BOOT]` 日志；
- App 的 `[NET]`/`[OTA]`/`[UI]` 日志需要 `DEBUG_SUPPORT=1`；
- OTA 失败时先看 `[OTA]` 日志：`probe`、`latest tag`、`asset`、`dl err`、`bad hdr`、`crc mismatch`；
- 下载页面显示 `HTTP error`/`No latest tag`/`Bad asset url` 时，分别检查 Release 状态、
  `releases/latest` 的 302、tag 与附件名。

---

## 8. 已知限制与待办

| 类别 | 问题 |
| --- | --- |
| 启动校验 | `App_QuickValid()` 的栈顶判断条件写反：未 8 字节对齐的 SP 反而能通过，削弱启动校验；正常 App 不受影响，但建议修成“对齐且在 SRAM/CCM 范围内”。 |
| 恢复 | `RECOVERY` 串口救援未实现；所有 `halt` 只能靠 ST-Link 重新烧录。`BOOT_FLAG_FORCE_RECOVERY` 已定义但未使用。 |
| 看门狗 | IWDG 尚未接入 Bootloader 擦写循环和 App UI 任务，见 [待办.md](待办.md)。 |
| OTA 安全 | 只有 CRC32 + `hw_id` 校验，没有签名/证书固定；HTTPS 可防传输损坏，但不能防仓库/服务器被篡改。 |
| 固件大小 | `APP_FW_SIZE_FALLBACK=160KB` 需要手动维护；当前 App 159828 B，只剩约 4KB 余量，App 再增大会导致备份截断。 |
| 资源烧录 | 批次 1 会格式化 littlefs，清掉 `/ota/firmware.bin` 和 `backup.bin`；升级后重烧资源会丢失回滚能力。 |
| 参数区掉电 | 32 条写满后整扇区擦除约 200~400ms，极端掉电可能丢 `boot_try`（见待办）。 |
| 调试 | 默认 `DEBUG_SUPPORT=0`，App 没有串口日志；排查 OTA 问题需临时打开。 |
| 显示 | 中文定位仍显示空白：LVGL 输入是 UTF-8，主页面字库按 GB2312 查表；`Utf8_Gb2312.c` 尚未加入工程。 |
| 显示 | 顶部条可能残留第二行：`Main_Page_Net_Update()` 在 y=28 画定位文本，`UI_TopBar_Draw()` 只清 y=5~24。 |
| 光敏 | AO 模式使用 `ADC_Channel_0`（应为 `ADC_Channel_10`）；当前 DO 模式不受影响。 |
| 裸机 | `main.c` 的裸机分支仍调用已注释的 `Test()`，`USE_FREERTOS=0` 会链接失败。 |
| 健壮性 | LCD SPI/DMA、W25Q64 字节读写是无超时忙等；`UI_Input_Calibrate()` 中 `Touch_Init()` 失败会 `for(;;);`。 |
| 资源注释 | `BuildConfig.h` / `Provision.c` 仍写 822KB、183460、33 张图标；实际 858778 B、183580、32 张图。 |

---

## 9. 文档与资料

### 9.1 项目文档

| 文件 | 内容 |
| --- | --- |
| `Documents/Bootloader_OTA_Flow.md` | Bootloader/OTA 入门版全流程：分区、状态机、网络升级、回滚 |
| `Documents/Bootloader_OTA_Guide.md` | 深入版：Flash 约束、参数区设计、搬运/回滚、踩坑记录 |
| `Documents/OTA_Net_Update_详解.md` | `OTA_Net.c` / `OTA_Update.c` 逐段解析 |
| `Documents/OTA_Test_Record.md` | Phase 0/1/2 测试记录与修复的 bug |
| `待办.md` | 串口救援、IWDG、失败诊断等后续项 |
| `MDK/Output/STM32F407.map` | App 链接 map |
| `Bootloader/Output/STM32F407_Boot.bin` | 当前 Bootloader 镜像 |

### 9.2 工具

| 文件 | 用途 |
| --- | --- |
| `ota_pack.py` | App `.bin` → `.ota` 打包（magic/64B 头/CRC16/CRC32） |
| `Documents/Tools/ota_send.py` | 串口发送工具，串口离线 OTA 已移除，保留作 Phase 3 救援协议基础 |
| `Documents/Tools/test_tag_parse.py` | 离线回归：tag 解析/版本比较/URL 拼接 |
| `Documents/Tools/gen_gb2312_table.py` | 生成 UTF-8→GB2312 转码表 |
| `Documents/Tools/verify_utf8_gb2312.py` | 校验转码表 |

### 9.3 器件手册

`Documents/` 下按类别存放：STM32F4、W25Q64、DS1302、ESP32-C3、屏幕、温湿度计、
ATGM336H、中文字库。
