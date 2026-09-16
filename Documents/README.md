# STM32F407 桌面天气时钟

> 基于 **STM32F407VET6 + FreeRTOS** 的桌面天气时钟 / 室内环境监测终端。
> 彩屏显示时钟与天气，OLED 负责夜间显示，光敏电阻自动切换昼夜模式，WiFi 走 ESP32-C3 AT 模组。

本文档对应的仓库快照：**2026-09-15**（字库与图片迁移到外部 W25Q64 之后；构建环境 MDK-ARM Plus 5.24.1 / ARMCC V5.06 update 5）。
文档与源码不一致时，**以源码为准**。

---

## 目录

- [1. 项目简介](#1-项目简介)
- [2. 功能特性](#2-功能特性)
- [3. 硬件平台](#3-硬件平台)
- [4. 目录结构](#4-目录结构)
- [5. 软件架构](#5-软件架构)
- [6. 外设与引脚分配](#6-外设与引脚分配)
- [7. 中断优先级与 FreeRTOS 约束](#7-中断优先级与-freertos-约束)
- [8. 关键流程说明](#8-关键流程说明)
- [9. 编译与烧录](#9-编译与烧录)
- [10. 编译开关与裸机测试](#10-编译开关与裸机测试)
- [11. 需要自行配置的参数](#11-需要自行配置的参数)
- [12. 串口调试日志](#12-串口调试日志)
- [13. 已知限制与待办](#13-已知限制与待办)
- [14. 参考资料索引（Documents/）](#14-参考资料索引documents)
- [15. 编码与维护约定](#15-编码与维护约定)

---

## 1. 项目简介

本项目是一个运行在 STM32F407VET6 上的桌面级天气时钟：

- 通过 **ESP32-C3-MINI-1（AT 固件 v4.1.1.0）** 连接 WiFi，用 SNTP 获取网络时间，用 HTTP 请求心知天气接口获取实况天气；
- 用 **ST7789 SPI 彩屏（240×320）** 显示主界面：时钟、日期星期、WiFi 状态、城市、天气图标与温度、室内温湿度；
- 用 **SSD1306 0.96" OLED（软 I2C）** 在夜间显示简洁的 `HH:MM` 与日期；
- 用 **光敏电阻模块** 感知环境亮度，自动在彩屏（白天）与 OLED（夜间）之间切换；
- 用 **DHT22/AM2302** 采集室内温度与湿度；
- 用 **一个轻触按键（PA0）** 支持单击 / 双击 / 三击 / 长按手势，目前单击用于手动切换昼夜；
- **FreeRTOS** 负责调度：显示、联网、传感器、光敏、按键各自独立任务，通过事件组通信；
- **夜晚进入低功耗模式**：关闭彩屏、只保留 OLED 显示时间，并暂停 WiFi / SNTP / 天气 / 室内温湿度的周期更新；退出时立即补更一次。

时间基准白天为 **SNTP 校时 + TIM5 1ms 计数** 推算的软件时钟（见 8.3）；夜间切换到 **DS1302 外部 RTC**（进入夜间前用网络时间回写并回读校验），因此夜间不依赖网络也能正常走时。

**字库与图片已全部迁移到外部 W25Q64**，由 littlefs 管理，MCU 内不再保存任何点阵与像素数据（详见 9.8、9.9 节）。

---

## 2. 功能特性

### 主界面（彩屏 240×320）

- 顶部状态条：WiFi 图标（连接/断开两种）/ SSID（`ssid_len + 2 > 10` 时截断为前 5 字符的 `[xxxxx...]`）。
- 大号时钟：`Font_48B` 绘制 `HH:MM`，冒号随秒奇偶闪烁；未校时显示 `--:--`。
- 日期行：`YYYY/MM/DD` + 英文星期（`Monday` … `Sunday`），`Font_16B`。
- 实况天气卡片：26 条天气码映射（复用 22 张图标）→ 图标 + 中文（`晴 / 多云 / 阴 / 阵雨 / 雷阵雨 / 小雨 … 霾`），未知天气码回落为晴天；下方显示温度 `xx.x` + `C`，`Font_22B`。
- 室内温湿度卡片：温度 `C` 与湿度 `%`，DHT22 读取失败时显示 `--`。

### 开机流程

1. 上电 → LED 自检 → `TIM5` → `ST7789_Init` → `USART2` 调试口。`ST7789_Init()` 在打开显示与背光**之前**先把 GRAM 刷黑——ST7789 复位不会清 GRAM，否则会先亮出复位前残留在屏上的旧画面（例如上次加载完的主页）。
2. 显示开机底图 `Boot_Page_Waitconnect`。
3. 创建 `dht22` / `net` 任务，等待联网阶段结束（最长 **30s** 兜底）。
4. 显示连接结果页：
   - 成功：`[SSID] Connect` / `Mac: xx:xx:...` / `Channel: n, RSSI: -nn`；
   - WiFi 失败：错误图标 + `[WiFi] Disconnect`；
   - 服务失败（SNTP 或天气）：错误图标 + `[Service] Init failed`。
5. 停留 **2.5s** → 绘制主界面 → 启动光敏任务与按键任务（保证昼夜切换只发生在开机完成之后）。

### 夜间低功耗模式与双屏切换

- **变暗**：用系统时间回写 DS1302（回读校验）→ 置 `s_lowpower = true` → 打开 OLED 并从 DS1302 读时间显示 → 关闭 ST7789 显示（`0x28` 显示睡眠）与背光。
- **夜间**：`Net_Task` / `DHT22_Task` **冻结周期计数器、不执行任何 update**；`UI_Task` 事件等待放宽到 `EV_LP_UI_TICK_MS`（2s），OLED 仅在时/分/日变化时重绘。
- **变亮**：关闭 OLED → 恢复 ST7789（`0x29` + 背光）→ 整屏重绘主界面 → 清 `s_lowpower` 并置 `EV_NET_UPDATE_NOW | EV_SENSOR_UPDATE_NOW`，两个任务各立即补一次 update。
- 切换由光敏任务发起，等待 `UI_Task` 回 `EV_LOWPOWER_ACK`（超时 2s），避免竞态。
- **PA0 按键单击可手动切换昼夜**（见 8.7）：切换后光敏不会立刻把它改回去，直到环境光真正发生反向变化才恢复自动跟随。
- 光敏任务**以中断为主、1000ms 超时轮询兜底**：即使 EXTI 未触发，也能在 1 秒内感知亮度变化并切换。

### 其他

- 串口调试日志：`USART2 @115200-8N1`，行缓冲 + DMA 发送，多任务整行原子输出。
- 网络异常自恢复：WiFi 掉线自动重连，重连成功后立即补一次 SNTP 与天气。
- **上电即初始化两块屏幕**：`Board_Init()` 会 `OLED_Init()` + `OLED_Display_Off()`，把 OLED 拉到"GDDRAM 全 0、显示关"的已知状态（见 6.5）。
- **按键手势**：PA0 单击 / 双击 / 三击 / 长按（800ms，按住即触发）由 `Key_Task` 识别；目前单击 = 手动切换昼夜，其余手势是留空的扩展点（见 8.7）。

---

## 3. 硬件平台

| 项目 | 参数 |
| --- | --- |
| MCU | STM32F407VETx（Cortex-M4F，512KB Flash，128KB SRAM = 112KB SRAM1 + 16KB SRAM2，另 64KB CCM） |
| 系统时钟 | 168MHz（`HSE = 25MHz`，`PLL_M=25 / PLL_N=336 / PLL_P=2`） |
| 彩屏 | ST7789V，SPI3，240×320，RGB565 |
| 夜间显示 | SSD1306 0.96" OLED，128×64，软 I2C，从地址 `0x3C` |
| 联网 | ESP32-C3-MINI-1，AT 固件 `v4.1.1.0`，USART1 |
| 室内传感器 | DHT22 / AM2302 单总线 |
| 环境光 | 光敏电阻模块，DO（EXTI）或 AO（ADC + 模拟看门狗）二选一 |
| 用户按键 | 轻触按键 1 个，PA0，下拉输入（空闲低、按下高），EXTI0 双沿 |
| 调试口 | USART2 @115200 |
| 外部 RTC | DS1302 模块（**PE7=RST / PE8=IO / PE9=CLK**），夜间低功耗模式的时间源 |
| 外部存储 | W25Q64 SPI Flash（8MB），运行 littlefs，存放全部字库与图片 |

> ⚠️ **DS1302 引脚在 GPIOE，不是 GPIOB**：`BSP/Inc/External_RTC.h` 定义为 `DS1302_PORT = GPIOE`、`RST = Pin_7`、`IO = Pin_8`、`CLK = Pin_9`。部分代码注释（`BuildConfig.h`、`bare_test.c` 的 printf）仍写着旧的 `PB0/PB1/PB2`，**以头文件为准**。

> ⚠️ **晶振注意**：Keil 工程 `Cpu` 串中的 `CLOCK(12000000)` 只影响调试器的 Xtal 设置，**不参与时钟树计算**。
> 实际参与计算的是 `Core/stm32f4xx.h` 的 `HSE_VALUE`（当前 `STM32F40_41xxx` 分支为 **25000000**）。
> 若你的核心板是 8MHz 晶振，必须同步修改 `HSE_VALUE` 与 `system_stm32f4xx.c` 的 `PLL_M`，否则串口波特率、TIM5 计时、SPI 速率全部偏移。

---

## 4. 目录结构

```
Project4/
├── .gitignore                     # 忽略 MDK/Output 与 Keil 中间文件（见 9.6）
├── KeilClear.bat                  # 递归清理 Keil 中间文件
├── 项目简历-STM32F407桌面天气时钟.md # 项目简介
├── User/                          # 应用层（Inc 10 个头 / Src 9 个源）
│   ├── Inc/
│   │   ├── main.h                 # 仅汇聚 C 标准库头
│   │   ├── Board.h                # 板级初始化声明
│   │   ├── BuildConfig.h          # 编译开关（USE_FREERTOS / 资源烧录）
│   │   ├── App.h                  # 联网/传感器服务与软件时钟接口
│   │   ├── app_task.h             # 任务、事件位、周期参数定义
│   │   ├── Provision.h            # 资源烧录固件接口
│   │   ├── bare_test.h            # 裸机模块测试接口
│   │   ├── Page.h                 # 页面绘制接口
│   │   ├── stm32f4xx_conf.h       # 标准外设库裁剪配置
│   │   └── stm32f4xx_it.h         # 中断向量服务声明
│   └── Src/
│       ├── main.c                 # 入口：资源烧录 / RTOS / 裸机 三种模式
│       ├── Board.c                # 时钟使能、LED、TIM5、USART2、LCD、资源层、OLED 初始化
│       ├── App.c                  # WiFi / SNTP / 天气 / DHT22 服务 + 软件时钟
│       ├── app_task.c             # UI_Task / Net_Task / DHT22_Task / LightSensor_Task / Key_Task
│       ├── Provision.c            # 资源烧录实现（仅 RESOURCE_PROVISION=1 时编译）
│       ├── bare_test.c            # 裸机模块测试用例
│       ├── stm32f4xx_it.c         # 中断向量服务（NMI/HardFault/... 桩函数）
│       ├── Boot_Page.c            # 开机页绘制
│       └── Main_Page.c            # 主界面绘制（状态条 / 时间 / 天气 / 温湿度）
├── Resource/                      # 屏幕资源数据（Inc 2 个 / Src 12 个）
│   ├── Inc/
│   │   ├── Font.h                 # 字库接口与 Font_16B 等字模描述符
│   │   └── Image.h                # 图片结构与绘制接口
│   └── Src/
│       ├── Font/                  # Font_12/16/22/32/48 + Chinese_Font16/22（资源备份）
│       └── Image/                 # Image.c / Image_Boot_Page.c / Image_Main_Page.c /
│                                  # Image_Weather.c / ImageTable.c（图片清单）
├── BSP/                           # 板级驱动（Inc 与 Src 各 13 个文件）
│   ├── Inc/ · Src/
│   │   ├── Asset.c                # 资源层：挂载 littlefs、开机自检、O(1) 字模读取
│   │   ├── Profiling.c            # 基于 DWT->CYCCNT 的微秒级计时
│   │   ├── AT.c                   # ESP32-C3 AT 指令、WiFi/CWSTATE/CWJAP、SNTP、HTTP、JSON 解析
│   │   ├── DHT22.c                # 单总线时序、校验和、错误码
│   │   ├── External_RTC.c         # DS1302 驱动（裸机测试 + 夜间低功耗时间源）
│   │   ├── I2C.c                  # 通用软件 I2C
│   │   ├── Key.c                  # PA0 按键：EXTI0 双边沿上报，手势识别在 app_task.c
│   │   ├── LCD.c                  # ST7789：SPI3 + DMA1_Stream5、字模渲染、图片双缓冲流式显示
│   │   ├── Light_Sensor.c         # DO(EXTI1) 或 AO(ADC1 + AWD) 两种模式
│   │   ├── OLED.c                 # SSD1306 命令/数据、取字模、OLED_ShowClock
│   │   ├── Timer.c                # TIM5 1ms 基准 + delay_us/delay_ms
│   │   ├── Usart.c                # USART1 RX 环形缓冲 + USART2 printf(DMA)
│   │   └── W25Q64.c               # SPI1 Flash 驱动（读 / 页编程 / 扇区擦除 / 忙等待超时）
├── Core/                          # CMSIS 内核 + 启动文件 + 系统时钟
│   ├── core_cm4.h / core_cmFunc.h / core_cmInstr.h / core_cmSimd.h
│   ├── stm32f4xx.h                # HSE_VALUE 等器件配置
│   ├── system_stm32f4xx.c/.h      # PLL 配置
│   ├── 文件说明.txt
│   └── Startup/
│       └── startup_stm32f40xx.s   # 工程实际使用
├── STM32F4xx_StdPeriph_Driver/    # STM32F4 标准外设库（工程引用 42 个 .c）
├── Third_Lib/
│   ├── FreeRTOS/
│   │   ├── croutine.c · event_groups.c · list.c · queue.c ·
│   │   │   stream_buffer.c · tasks.c · timers.c
│   │   ├── include/
│   │   └── portable/              # FreeRTOSConfig.h / heap_4.c / port.c / portmacro.h
│   └── LittleFS/                  # littlefs v2.11（LFS_VERSION 0x0002000b）
│       ├── Inc/                   # lfs.h / lfs_config.h / lfs_port.h / lfs_util.h /
│       │                          #   LFS_Operation.h
│       └── Src/                   # lfs.c / lfs_util.c / lfs_port.c / LFS_Operation.c
│                                  #   LFS_Operation 提供 SaveFont / LoadFont /
│                                  #   SaveImage / LoadImage 读写封装
├── MDK/                           # Keil MDK5 工程
│   ├── STM32F407.uvprojx          # 工程文件（入口）
│   ├── STM32F407.uvoptx           # 调试器与断点等用户选项
│   ├── STM32F407.uvguix.*         # 个人窗口布局（已被 .gitignore 忽略）
│   ├── build.log · EventRecorderStub.scvd  # 构建日志已被忽略
│   ├── DebugConfig/               # 调试器配置
│   ├── .vscode/                   # Keil Assistant 插件的配置(跟踪)与日志(忽略)
│   └── Output/                    # 构建产物 .axf/.map/.htm/.lnp/.sct/.o/...（已被忽略）
└── Documents/                     # 器件资料与本说明文档（无参考工程源码）
```

> **字库与图片已迁移到外部 W25Q64**：`Resource/Src` 下的 `Font_*.c`、`Chinese_Font*.c`、
> `Image*.c` 里的点阵/像素数据仍完整保留，但被复合宏 `#if (RESOURCE_DATA_IN_ROM == 1) && (PROVISION_BATCH == N)`
> 排除在正式固件之外（详见 9.8、9.9 节）。
> 注意 `ImageTable.c`、`Boot_Page.c`、`Main_Page.c` **没有**该守卫——它们只含指针表与绘制逻辑，正式固件照常编译。

---

## 5. 软件架构

### 分层

```
应用层      User/Src : main.c / Board.c / App.c / app_task.c / Provision.c / bare_test.c /
                       stm32f4xx_it.c（中断服务）/ Boot_Page.c / Main_Page.c
板级驱动    BSP/Src  : Asset / LCD / OLED / AT / I2C / DHT22 / External_RTC /
                       Light_Sensor / Key / Timer / Usart / W25Q64 / Profiling
操作系统    Third_Lib/FreeRTOS（portable/FreeRTOSConfig.h）
文件系统    Third_Lib/LittleFS（lfs_port.c 对接 W25Q64，LittleFS_Operation 提供读写封装）
硬件抽象    STM32F4xx_StdPeriph_Driver + Core（CMSIS / 启动 / system_stm32f4xx.c）
```

设计约定：

- **LCD 只由 UI_Task 写**；netTask/sensor 任务只更新数据并置事件位。
- **USART1（AT）只由 Net_Task 使用**，避免 AT 响应串扰。
- **共享数据用临界区整块替换**：`wifi_info`、`weather_info`、`room_info`、软件时钟的 `epoch/sync_ms` 都在 `taskENTER_CRITICAL()` 内整体赋值，UI_Task 不会读到半写状态。
- **字模与图片只从 W25Q64 读**：MCU 内不再保存任何点阵/像素数据。`Asset` 层在 `Board_Init()` 中挂载 littlefs 并逐项自检；资源缺失时该项渲染退化为纯色填充，不阻塞任务、不重启、不自动格式化。

### 任务一览（`User/Src/app_task.c`）

| 任务（函数名 / FreeRTOS 名） | 优先级 | 栈（字 / 字节） | 职责 | 周期 |
| --- | --- | --- | --- | --- |
| `UI_Task` / `"ui"` | 3 | 1024 / 4KB | 创建事件组、`Board_Init`、开机页 → 创建 `dht22`/`net` → 等 `EV_NET_READY`（30s 超时）→ 结果页 → 主界面 → 创建光敏与按键任务；事件驱动刷新，昼夜切换执行者 | 白天 500ms / 夜间 `EV_LP_UI_TICK_MS`(2s) |
| `DHT22_Task` / `"dht22"` | 4 | 512 / 2KB | 等待 10s 超时即为采集周期，同时响应 `EV_SENSOR_UPDATE_NOW` 立即补采；低功耗期间跳过；优先级高于 UI，避免抢占破坏 us 级时序 | 10s |
| `Net_Task` / `"net"` | 2 | 1024 / 4KB | 独占 USART1/AT；开机联网 + SNTP + 天气；1s 心跳推进三个周期计数器，响应 `EV_NET_UPDATE_NOW` 立即补更；低功耗期间冻结计数器 | 1s 心跳 |
| `LightSensor_Task` / `"light_sensor"` | 2 | 512 / 2KB | 注册中断回调、初始化光敏；等待中断（1000ms 超时轮询兜底）→ 2s 去抖 → 昼夜切换并等 ACK；**优先处理按键的手动切换请求**（见 8.7） | 事件驱动 + 1s 兜底 |
| `Key_Task` / `"key"` | 2 | 1024 / 4KB | 注册中断回调、初始化按键（PA0）；手势状态机产出单击/双击/三击/长按，目前仅单击接"切换昼夜" | 事件驱动，无轮询 |

`Net_Task` 周期参数（`User/Inc/app_task.h`）：

| 项 | 正常周期 | 失败重试 |
| --- | --- | --- |
| WiFi 检查 | `WIFI_PERIOD_S = 60s` | `RETRY_WIFI_S = 10s` |
| SNTP 校时 | `SNTP_PERIOD_S = 4h` | `RETRY_SNTP_S = 5s` |
| 天气更新 | `WEATHER_PERIOD_S = 1h` | `RETRY_WEATHER_S = 60s` |

其它常量：

- `DEBOUNCE_MS = 2000`（昼夜切换去抖 2s）
- `EV_LP_UI_TICK_MS = 2000`（夜间 `UI_Task` 事件等待超时；夜间只显示到分钟）
- 按键手势（`User/Inc/app_task.h`）：`KEY_LONG_PRESS_MS = 800`（长按阈值，按住即触发）、`KEY_MULTI_GAP_MS = 300`（连击窗口）、`KEY_CLICK_MAX = 3`（最多识别到三击）、`KEY_RELEASE_POLL_MS = 20`（长按后等松开的轮询步长）
- `RTC_LOWPOWER_ENABLE = 1`（夜间时间源：`1` = DS1302，`0` = 软件时钟）
- `configTICK_RATE_HZ = 1000`、`configMAX_PRIORITIES = 5`（优先级 0~4）
- `configUSE_TIME_SLICING = 0`、`configTOTAL_HEAP_SIZE = 88KB`（heap_4）
- `configCHECK_FOR_STACK_OVERFLOW = 2`；`configASSERT` 失败进入 `vAssertCalled`
- 未开启 tickless idle（`configUSE_TICKLESS_IDLE = 0`）

> `app_task.c` 里 `WIFI_PERIOD_S` 附近有一句"连接上 30min"的旧注释，与 `app_task.h` 的 `60UL` 不符，**以头文件为准**。

### 事件位（`User/Inc/app_task.h`）

| 位 | 名称 | 生产者 | 含义 |
| --- | --- | --- | --- |
| bit0 | `EV_WEATHER` | Net_Task | 天气已更新，刷新天气模块 |
| bit1 | `EV_ROOM` | DHT22_Task | 房间温湿度已更新 |
| bit2 | `EV_NET_READY` | Net_Task | 开机网络阶段结束（无论成败） |
| bit3 | `EV_WIFI` | Net_Task | WiFi 状态变化，刷新顶部状态条 |
| bit4 | `EV_LOWERPOWER` | LightSensor_Task | 进入夜间 |
| bit5 | `EV_LOWPOWER_ACK` | UI_Task | 昼夜切换完成确认 |
| bit6 | `EV_WAKEUP` | LightSensor_Task | 回到白天 |
| bit7 | `EV_NET_UPDATE_NOW` | UI_Task | 退出低功耗：`Net_Task` 立即补 WiFi+SNTP+天气（唯一消费者，清除式等待） |
| bit8 | `EV_SENSOR_UPDATE_NOW` | UI_Task | 退出低功耗：`DHT22_Task` 立即补采（唯一消费者，清除式等待） |

> ⚠️ 这两个补更事件位**必须分开**：`xEventGroupWaitBits` 使用清除式等待，若两个任务共用一个位，先到的任务会把位清掉，另一个任务就永远收不到通知（表现为"退出夜间后只有一半数据被刷新"）。

### 数据流

```
ESP32-C3 ──USART1──▶ AT.c ──▶ wifi_info   ──▶ Main_Page_Net_Update / Boot_Page_Show
                       │
                       ├──▶ AT_SNTP_Get_Time ──▶ Clock_Sync ──▶ 软件时钟（TIM5 推算）
                       │                                          └─▶ 主界面时钟 / OLED 时钟
                       └──▶ AT_Get_HTTP ──▶ Parse_Weather_Response ──▶ weather_info ──▶ 天气卡片

DHT22 ──▶ DHT22_ReadData ──▶ room_info ──▶ 室内温湿度卡片
光敏中断 ──▶ vTaskNotifyGiveFromISR ──▶ LightSensor_Task（2s 去抖）──▶ 事件组 ──▶ UI_Task 切屏
按键中断 ──▶ vTaskNotifyGiveFromISR ──▶ Key_Task（手势状态机）──▶ 单击请求 ──▶ LightSensor_Task（统一裁决昼夜）

夜间（低功耗）:
进入夜间前: 软件时钟 ──▶ DS1302_SetTime（回读校验）
夜间:       DS1302_ReadTime ──▶ 成功 ──▶ OLED 显示
                            └── 失败 ──▶ 软件时钟降级（打印 time_source=SoftClock）
退出夜间:   UI_Task ──▶ EV_NET_UPDATE_NOW ─────▶ Net_Task 立即 WiFi+SNTP+天气
                    └─▶ EV_SENSOR_UPDATE_NOW ──▶ DHT22_Task 立即采集

资源（字库/图片，全部来自 W25Q64）:
W25Q64 ──SPI1 21MHz──▶ Asset 层 ──▶ 字模: (区码-0xB0)*94+(位码-0xA1) 直接算偏移，O(1)
                                └─▶ 图片: 分块流式读取 + DMA1_Stream5 乒乓双缓冲 ──SPI3──▶ ST7789
```

---

## 6. 外设与引脚分配

### 6.1 USART1 — ESP32-C3 AT 模组

| 信号 | 引脚 | 说明 |
| --- | --- | --- |
| USART1_TX | PA9 | AF7，115200-8N1 |
| USART1_RX | PA10 | AF7，RXNE 中断 → 512 字节环形缓冲 |

接收路径：`USART1_IRQHandler`（`BSP/Src/Usart.c`）逐字节入环形缓冲，AT 层用 `Usart1_RX_Count()` / `Usart1_RX_Read()` 轮询取出，等待期间 `vTaskDelay(1)` 让出 CPU。缓冲满则丢弃。

### 6.2 USART2 — 调试输出

| 信号 | 引脚 | 说明 |
| --- | --- | --- |
| USART2_TX | PA2 | AF7，115200-8N1，DMA1_Stream6 / Channel4 |
| USART2_RX | PA3 | AF7（当前未使用） |

`printf` 重定向到 `fputc`：按行加锁（互斥量 + 行主记录），遇 `'\n'` 或缓冲满（256B）时以 DMA 整行发出；调度器未运行 / 中断上下文中退化为逐字节轮询。

### 6.3 SPI3 — ST7789V 彩屏

| 信号 | 引脚 | 说明 |
| --- | --- | --- |
| SCLK | PC10 | AF6 |
| MISO | PC11 | AF6 |
| MOSI | PC12 | AF6 |
| CS | PE2 | 软件片选 |
| RESET | PE3 | 复位 |
| DC | PE4 | 命令/数据选择 |
| BLK | PE5 | 背光 |

- `SPI_BaudRatePrescaler_2`（SPI3 = 42MHz → 21MHz），`CPOL=0 / CPHA=1`，MSB First。
- 像素搬运走 DMA1_Stream5 / Channel0；命令阶段 8 位、像素阶段 16 位，切换前先等 `BSY` 并关 `SPE`。
- 渲染缓冲 `s_scratch[240 × 48 × 2]`（约 23KB），供整行文字与图标共用。
- 图片绘制用**乒乓双缓冲**：一块由 DMA 发送时，CPU 同时从 SPI1 读下一块，两个 `7680` 字节的缓冲
  在 `ST7789_Init()` 里从 FreeRTOS 堆分配（共 15KB），分配失败自动退化为单缓冲。

### 6.4 SPI1 — W25Q64 Flash

| 信号 | 引脚 | 说明 |
| --- | --- | --- |
| CLK | PA5 | AF5 |
| MISO | PA6 | AF5 |
| MOSI | PA7 | AF5 |
| CS | PA4 | 软件片选 |

- `SPI_BaudRatePrescaler_4`（SPI1 = 84MHz → **21MHz**），`CPOL=0 / CPHA=1`，MSB First。
- 读取走 **CPU 轮询**；页编程 / 扇区擦除内部阻塞等待 BUSY，并带超时上限
  （`W25Q64_BUSY_TIMEOUT`），芯片未焊接时不会死等，超时次数可用 `W25Q64_GetBusyTimeoutCount()` 读。
- 芯片上运行 **littlefs v2.11**，覆盖整片 8MB（2048 × 4KB 块），由 `lfs_port.c` 提供 read/prog/erase 回调。
- 存放全部字库与图片（写入数据合计 858778 字节 ≈ 839KB；littlefs 实际占用约 247/2048 块）；文件布局见 9.9 节。
- **SPI1 与 SPI3 同为 21MHz**——这正是图片双缓冲能把 W25Q64 读取时间完全隐藏在 DMA 发送之后的前提。

### 6.5 软件 I2C — SSD1306 OLED

| 信号 | 引脚 | 说明 |
| --- | --- | --- |
| SCL | PB6 | 开漏 + 内部上拉 |
| SDA | PB7 | 开漏 + 内部上拉 |
| 从地址 | `0x3C` | 命令前缀 `0x00`，数据前缀 `0x40` |

**上电必须初始化 OLED**：SSD1306 只要不断电就一直保持 GDDRAM 内容和显示开关状态，所以裸机测试后切回 FreeRTOS 固件时，屏上会一直挂着上一次的测试画面。因此 `Board_Init()` 上电就执行 `OLED_Init()` + `OLED_Display_Off()`（白天由 LCD 显示）；并且 `OLED_Init()` 里是**先清屏、最后才 `0xAF` 开显示**，否则从开显示到清屏写完这段时间旧内容会被看到。

`OLED_Clear()` 采用**按页整块写**（每页 1 次 I2C 事务写 128 字节，共 8 次）；若逐列发送则需要 1024 次单字节事务、耗时约 240ms。

### 6.6 传感器 / ADC / 定时器 / 外部 RTC

| 功能 | 引脚 | 说明 |
| --- | --- | --- |
| DHT22 DATA | PE6 | 单总线，输出/输入切换，输入上拉 |
| 光敏 AO | PC0 | `GPIOC + GPIO_Pin_0`，对应 `ADC123_IN10`（驱动里的 ADC 通道仍是 `ADC_Channel_0`，见第 13 节） |
| 光敏 DO | PC1 | `GPIOC + GPIO_Pin_1` → `EXTI1`（端口源为 `EXTI_PortSourceGPIOC`），上升沿/下降沿双沿触发 |
| 按键 KEY | PA0 | `GPIOA + GPIO_Pin_0` → `EXTI0`（端口源 `EXTI_PortSourceGPIOA`）；**下拉输入**：空闲低、按下高；上升/下降沿双沿触发，手势识别见 8.7 |
| 测试 LED | PC5（FreeRTOS）/ PB2（裸机） | 推挽输出，低电平点亮；`Board.c` 按 `USE_FREERTOS` 选择引脚 |
| DS1302 外部 RTC | **PE7=RST / PE8=IO / PE9=CLK** | 裸机测试项 `BM_TEST_MODULE_DS1302` + 夜间低功耗时间源；`DS1302_Init()` 只置写保护，不清零秒寄存器 |

开关宏：`BSP/Inc/Light_Sensor.h` 中 `AO_DO_SWITCH`，**默认 `0`（DO 模式）**；改为 `1` 使用 ADC + 模拟看门狗。AO 模式的阈值常量：`LIGHT_SENSOR_AO_DARK_TH = 3000`、`LIGHT_SENSOR_AO_LIGHT_TH = 2000`（迟滞回防抖），极性由 `LIGHT_SENSOR_AO_DARK_HIGH` 决定。

DO 模式判定：DO 引脚为**高**（光照未达电位器阈值）→ 视为"暗"。

> ⚠️ 改光敏引脚时，`BSP/Inc/Light_Sensor.h` 里的 **GPIO 端口/引脚**与 **`LIGHT_SENSOR_DO_EXTI_PORT_SOURCE` / `..._PIN_SOURCE`** 必须成对修改。两者不一致时 EXTI 中断源仍挂在旧端口上，**电平变化不会产生任何中断**，表现为"遮挡后无反应、串口也无日志"。
> 注意：**PA0 已分配给按键**（见 6.6 与 8.7），光敏用的是 PC0/PC1，两者不冲突。

---

## 7. 中断优先级与 FreeRTOS 约束

NVIC 分组为 **`NVIC_PriorityGroup_4`**（4 位全为抢占优先级）。

| 中断 | 抢占优先级 | 能否调用 FreeRTOS API |
| --- | --- | --- |
| PendSV / SysTick | 15（`configKERNEL_INTERRUPT_PRIORITY`） | — |
| **TIM5** | **2** | ❌ **禁止**（优先级数值 < `configMAX_SYSCALL_INTERRUPT_PRIORITY`） |
| USART1 | 5 | ✅ 可调用 `...FromISR` |
| EXTI0（按键，PA0） | 5 | ✅ 可调用 `...FromISR` |
| EXTI1（光敏 DO，PC1） | 5 | ✅ 可调用 `...FromISR` |
| ADC（光敏 AO） | 5 | ✅ 可调用 `...FromISR` |

规则（`Third_Lib/FreeRTOS/portable/FreeRTOSConfig.h` 中有对应注释）：

- `configMAX_SYSCALL_INTERRUPT_PRIORITY = 5 << 4`；优先级数值 **≥ 5** 的中断才允许调用 FreeRTOS API。
- `TIM5_IRQHandler` 优先级为 2，**只做 `TIM5_ms++`**，不得加锁、不得调用任何 FreeRTOS API。
- 光敏 ISR 与按键 ISR 都只做 `vTaskNotifyGiveFromISR` + `portYIELD_FROM_ISR`；去抖、手势识别与显示逻辑都在任务里完成。

---

## 8. 关键流程说明

### 8.1 AT 初始化与联网时序

```
AT_Init():
    GPIO / USART1 / NVIC(优先级5) 初始化
    AT_Wait_Boot(3000)          # 反复发 "AT"，最多 30 次
    AT+RESTORE                  # 恢复出厂（2000ms）
    AT_Wait_Ready(5000)         # 等 "ready"

Wireless_Init():
    AT+CWMODE=1                 # Station 模式

Service_WiFi_Connect():
    AT+CWJAP="<ssid>","<password>"          # 超时 10000ms
    AT+CWSTATE?                 # 解析连接状态与 SSID（必需项）
    AT+CWJAP?                   # 解析 BSSID / channel / RSSI（失败不影响整体）

AT_SNTP_Init():
    AT+CIPSNTPCFG=1,8           # 使能 SNTP，东八区

Service_Time_Sync():
    AT+CIPSNTPTIME?             # 最多轮询 15 次，每次间隔 1s

Service_Weather_Update():
    AT+HTTPCLIENT=2,1,"<url>",,,2   # 直接返回 JSON 响应体
```

`AT_SNTP_Get_Time` 会把 `1970` 年等无效时间判为失败，等待下一轮重试。

### 8.2 天气 JSON 解析

`Parse_Weather_Response()`（`BSP/Src/AT.c`）在响应中定位 `"results":` → `"location":` → `"name"` / `"path"`，以及 `"now":` → `"text"` / `"code"` / `"temperature"`，分别填入：

| 字段 | 结构体成员 | 用途 |
| --- | --- | --- |
| `name` | `city[32]` | 解析保留，当前界面未使用（定位标签已从顶部状态条移除） |
| `path` | `location[128]` | 备用 |
| `text` | `weather[16]` | 英文天气描述（当前界面改用中文映射表显示） |
| `code` | `weather_code` | 查 26 项 `weather_map`（复用 22 张图标）得图标 + 中文 |
| `temperature` | `temperature` | 天气卡片温度 |

> **定位显示已移除**：`Main_Page_Net_Update()` 现在只绘制 WiFi 图标与 SSID。`weather_info.city` 仍在解析并保留在结构体中，但不再参与界面绘制；`Image_location` / `Image_no_location` 两张图标也不再被引用（资源文件仍留在 W25Q64 上，仍参与开机自检，也可后续清理）。

### 8.3 软件时钟

- `Clock_Sync(&date_info)`：校验 `year >= 2000` 与月份合法后，用 `Clock_DaysFromCivil()`（Howard Hinnant 民用历算法）把日期换算为 Unix 时间戳，与当前 `TIM5_Get_ms()` 一起在临界区内保存。
- `Clock_GetDateTime()`：临界区内一次性取出 `synced / epoch / sync_ms / now_ms`，再算 `epoch + elapsed_ms/1000`，最后用 `Clock_CivilFromDays()` 反算年月日与星期（`((days+3)%7+7)%7+1`，1=周一）。
- 未同步时返回全 0，界面据此绘制 `--:--` / 空日期，因此 **上电后到首次 SNTP 成功之间不会显示错误时间**。

### 8.4 昼夜切换与低功耗握手

```
LightSensor_Task                          UI_Task                    Net_Task / DHT22_Task
  中断通知(或1s轮询兜底) → 2s 去抖
  暗 & !requested_night:
     置 EV_LOWERPOWER  ───────────────▶  ① RTC_Sync_From_SystemClock()（回读校验）
                                        ② s_lowpower = true
                                        ③ OLED 开 → RTC_ReadDataTime() → 显示
                                        ④ ST7789 睡眠 + 关背光
     等 EV_LOWPOWER_ACK(2s) ◀──────────  ⑤ 置 EV_LOWPOWER_ACK
                                                                   → 下一拍起冻结计数器 / 跳过采集
  亮 & requested_night:
     置 EV_WAKEUP      ───────────────▶  ① OLED 关 → ST7789 恢复 → Main_Page_Display()
                                        ② s_lowpower = false
                                        ③ 置 EV_NET_UPDATE_NOW|EV_SENSOR_UPDATE_NOW ─▶ 立即补更/补采
     等 EV_LOWPOWER_ACK(2s) ◀──────────  ④ 置 EV_LOWPOWER_ACK
```

夜间状态由 UI_Task 的 `s_lcd_on / s_oled_on` 记录（`s_lowpower` 供各任务查询），事件刷新只在对应屏幕开启时执行；OLED 时钟仅在**时、分或日期变化**时重绘，时间源为 DS1302（失败回退软件时钟）。

夜间 `Net_Task` / `DHT22_Task` 的关键约定：

- **冻结周期计数器**：`if (s_lowpower) continue;` 放在递减之前。若边暂停边递减，退出夜间时三个计数器会同时归零，导致同一秒内连发多次 AT 事务。
- **补更走独立事件位**：`Net_Task` 等 `EV_NET_UPDATE_NOW`、`DHT22_Task` 等 `EV_SENSOR_UPDATE_NOW`，均为清除式等待，互不抢占。
- **正在执行的 AT 事务不被打断**：进入夜间时若 `Net_Task` 正在收发 AT（最长 10s），本轮跑完后下一拍才进入跳过状态；`UI_Task` 不等待 Net_Task，仍立即回 ACK。

### 8.5 DS1302 夜间时间源

- 三个内部静态辅助函数（`User/Src/app_task.c`）：`RTC_Ensure_Init()`（懒初始化）、`RTC_Sync_From_SystemClock()`（用网络时间回写并回读校验）、`RTC_ReadDataTime()`（优先读 DS1302，失败回退软件时钟并返回来源标志）。
- 回读校验：`DS1302_SetTime()` **恒返回 `true`**（无写校验），因此必须用 `DS1302_ReadTime()` 回读比对；比较"当日分钟总数"而非单独比较分钟字段，允许写入耗时造成的 2 分钟偏差且能跨小时。
- **CH（Clock Halt）位检查**：`DS1302_ReadTime()` 在 BCD 转换前先判断秒寄存器 bit7，`CH=1`（振荡器停）时直接返回 `false`，让调用方回退软件时钟。否则读到的秒值被冻结，界面会显示一个不动的钟、而日志仍报 `time_source=RTC`。CH 与写保护位的**上电状态均未定义**（见 DS1302 手册），只有 `DS1302_SetTime()` 会把它清 0。
- 星期语义统一为 DS1302 的 **1~7**：`DS1302_ReadTime()` 对 `week < 1` 直接判失败，若写入星期 0（旧写法 `weekday - 1`），周一会同步失败。
- **白天不停振**：退出夜间后代码不再访问 DS1302，但它持续走时（芯片只要有 VCC 或 VBAT 就在计时），以便断电重启后仍能给出正确时间；下次进入夜间时若软件时钟已同步，会被 `DS1302_SetTime()` 覆盖对齐。
- 失败降级：`RTC_LOWPOWER_ENABLE = 0` 或 DS1302 读取失败（含 CH=1）时，夜间改用软件时钟显示，日志打印 `time_source=SoftClock`。

### 8.6 串口输出

`printf` 走 `fputc` → 行缓冲 → DMA。整行输出由互斥量与"行属主"保证不与其他任务交叉；中断上下文或调度器未运行时退化为轮询，保证 HardFault/启动期日志也能打印（`vAssertCalled`、`vApplicationStackOverflowHook` 都依赖它）。

### 8.7 按键手势

**硬件**：PA0，下拉输入 → 空闲低、按下高；EXTI0 **双边沿**触发（按下与松开都进中断）。按键带硬件消抖，软件不再做消抖。

**分层**：`BSP/Src/Key.c` 只负责 GPIO/EXTI 初始化与"上报边沿"（ISR 内仅 `vTaskNotifyGiveFromISR` + `portYIELD_FROM_ISR`）；手势识别全部在 `app_task.c` 的 `Key_Task` 中完成。

**识别时序**（`configUSE_TIMERS = 0`，故不使用软件定时器，直接借 `ulTaskNotifyTake` 的超时能力）：

```
等按下(无限/连击窗口300ms) ──超时──▶ 结算累计击数(N击)
        │按下
        ▼
   等 800ms(长按阈值)
        ├─超时(仍按住)──▶ 长按事件(按住即触发) ──▶ 等松开 ──▶ 清残留通知
        └─收到通知(已松开)──▶ 累计 1 击 ──▶ 回到"等按下"，窗口 300ms
```

**手势枚举**（`BSP/Inc/Key.h`）：`KEY_GESTURE_CLICK_1` / `CLICK_2` / `CLICK_3` / `LONG`。

**分发**（`Key_OnGesture()`）：目前**只接单击**——置 `s_key_toggle_night` 并用 `xTaskNotifyGive` 唤醒 `LightSensor_Task`，由后者统一裁决昼夜（昼夜状态机在它手里，避免两个任务各自改状态）。双击/三击/长按为留空的 `TODO` 扩展点。

**手动切换与自动跟随的关系**：`LightSensor_Task` 新增 `baseline_dark`（自动判定基线）。手动切换后把"当前光照"记为基线，因此光敏**不会立刻把手动结果改回去**；只有环境光真正发生反向变化（`IsDark() != baseline_dark`）时，自动跟随才重新生效。

```
环境暗(baseline=暗, 夜晚) ──单击──▶ 白天, baseline 仍=暗
                                    │
                          环境一直暗: 保持白天(手动优先)
                          环境变亮 : baseline=亮, 自动跟随(已是白天, 无需切换)
                          环境再变暗: baseline=暗, 自动切回夜晚
```

**串口日志**：

| 日志 | 含义 |
| --- | --- |
| `[KEY] task ready: pressed=0` | 按键任务启动，附上电时的引脚电平 |
| `[LIGHT] key switch -> night/day` | 收到按键请求并完成一次手动切换 |
| `[LIGHT] auto: level=..., mode=...` | 自动判定路径（去抖结束后打印） |
| `[LIGHT] auto request NIGHT/DAY` | 自动判定确实改变了模式 |

> 连击时，两击之间的间隔只需小于 `KEY_MULTI_GAP_MS`(300ms)；超过该窗口会先把已累计的击数结算，再重新计数。

> ⚠️ **中断函数名必须与宏「大小写完全一致」**（本项目踩过的坑）：
> `Key.h` 定义 `#define KEY_EXTI_IRQHandler EXTI0_IRQHandler`，所以 `Key.c` 里的函数必须写成
> `void KEY_EXTI_IRQHandler(void)`（全大写 `KEY_`）。若写成混合大小写的 `void Key_EXTI_IRQHandler(void)`，
> C 预处理器**不会做替换**（宏替换区分大小写），于是它只是一个**没人调用的普通函数**，而
> `EXTI0_IRQHandler` 仍指向 `Core/Startup/startup_stm32f40xx.s` 里的 `[WEAK]` 默认实现
> ——那串共享的 `B .` 无限循环。症状：**按一下按键整个系统立刻静止、串口无任何新输出、编译却 0 Error 0 新增警告**。
> 光敏的 `LIGHT_SENSOR_DO_EXTI_IRQHandler` 写成全大写，正因如此才能正常展开成 `EXTI1_IRQHandler`。
>
> 验证方法：`MDK/Output/STM32F407.map` 中应出现
> `startup_stm32f40xx.o(RESET) refers to key.o(i.EXTI0_IRQHandler)`；若中断由启动文件提供则说明宏没展开。

---

## 9. 编译与烧录

### 9.1 环境

| 项 | 版本 |
| --- | --- |
| IDE | Keil MDK-ARM **Plus 5.24.1** |
| 编译器 | **ARMCC V5.06 update 5（build 528）**，即 AC5（非 AC6） |
| C 标准 | C99 已开启 |
| 工程 | `MDK/STM32F407.uvprojx` |
| Target | `STM32F407` |
| 器件 | `STM32F407VETx` |

### 9.2 关键配置（已在工程中设好，无需手动添加）

| 配置 | 值 |
| --- | --- |
| 预处理宏 | `USE_STDPERIPH_DRIVER,STM32F40_41xxx` |
| 包含路径 | `..\User\Inc`、`..\Resource\Inc`、`..\BSP\Inc`、`..\Core`、`..\Core\Startup`、`..\STM32F4xx_StdPeriph_Driver\inc`、`..\STM32F4xx_StdPeriph_Driver\src`、`..\Third_Lib\FreeRTOS\include`、`..\Third_Lib\FreeRTOS\portable`、`..\Third_Lib\LittleFS\Inc` |
| Output | `.\Output\`，名字 `STM32F407` |
| 启动文件 | `Core/Startup/startup_stm32f40xx.s` |

工程共 **9 个分组 / 100 个文件**：

| 分组 | 文件数 | 内容 |
| --- | --- | --- |
| USER/Config | 3 | `stm32f4xx_conf.h`、`FreeRTOSConfig.h`、`lfs_config.h`（跨目录集中列出的编译期配置头） |
| USER/Src | 9 | `main.c`、`Board.c`、`App.c`、`app_task.c`、`bare_test.c`、`Provision.c`、`stm32f4xx_it.c`、`Boot_Page.c`、`Main_Page.c` |
| RESOURCE | 12 | `Font_12/16/22/32/48.c`、`Chinese_Font16/22.c`、`Image.c`、`Image_Boot_Page.c`、`Image_Main_Page.c`、`Image_Weather.c`、`ImageTable.c` |
| BSP | 13 | 见第 4 节 |
| STARTUP | 1 | `startup_stm32f40xx.s` |
| CMSIS | 7 | `stm32f4xx.h`、`system_stm32f4xx.c/.h`、`core_cm4.h`、`core_cmFunc/Instr/Simd.h` |
| STM32F4xx_StdPeriph_Driver | 42 | 标准外设库（磁盘上的 `stm32f4xx_fmc.c` **未加入工程**） |
| THIRD_LIB/FreeRTOS | 9 | 7 个内核 .c + `heap_4.c` + `port.c` |
| THIRD_LIB/LittleFS | 4 | `lfs.c`、`lfs_util.c`、`lfs_port.c`、`LFS_Operation.c` |

> **新增源文件**必须同时加入 `MDK/STM32F407.uvprojx` 的对应分组，并确保头文件目录已在包含路径中。

### 9.3 构建产物

| 文件 | 说明 |
| --- | --- |
| `MDK/Output/STM32F407.axf` | 可执行映像（默认产物） |
| `MDK/Output/STM32F407.map` | 链接映射表（内存分布/符号） |
| `MDK/Output/STM32F407.htm` | 静态调用图 |
| `MDK/Output/STM32F407.build_log.htm` | 构建日志 |
| `MDK/Output/*.o/.crf/.d` | 各编译单元的中间产物 |

**不生成 `.hex`**（`CreateHexFile = 0`）。工程里虽然保存了 `fromelf --bin` 的 User 命令，但 **`RunUserProg1 = 0` 未启用**，所以 `.bin` 不会自动刷新 —— 仓库中的 `MDK/Output/STM32F407.bin` 是 2025-03-07 的旧文件，请勿直接使用。

需要 `.bin` 时的做法（不改工程也可以）：

1. `Project → Options for Target → User`，勾选 `Run #1`（命令已填好：`"$K\ARM\ARMCC\bin\fromelf.exe" --bin --output="$L@L.bin" "#L"`），重新编译；或
2. 手动执行：`D:\Keil5\ARM\ARMCC\bin\fromelf.exe --bin --output=MDK\Output\STM32F407.bin MDK\Output\STM32F407.axf`。

### 9.4 下载 / 调试

- **调试器**：`STM32F407.uvoptx` 中 Debug 页实际选择的是 **ST-Link**（`STLink\ST-LINKIII-KEIL_SWO.dll`）；`uvprojx` 的 Utilities 页仍残留旧的 `UL2CM3` 配置。以 Keil 打开工程后 `Options for Target → Debug` 下拉框显示为准。
- Flash 算法 `STM32F4xx_512`（起始 `0x08000000`，大小 `0x80000`）。
- 调试器配置文件：`MDK/DebugConfig/STM32F407_STM32F407VETx.dbgconf`。
- 烧录后 ESP32-C3 需预先烧录 AT 固件（见 `Documents/ESP32-C3/`），默认波特率 **115200**。

### 9.5 资源占用（2026-09-16 实测）

迁移前（字库与图片编译进 MCU）：

```text
Program Size: Code=31844  RO-data=323604  RW-data=360  ZI-data=128240
Total ROM Size: 355556
```

迁移后（资源存放在 W25Q64，**当前状态**，含按键手势）：

```text
Program Size: Code=51940  RO-data=3748  RW-data=412  ZI-data=126740
Total RO  Size (Code + RO Data)          55688
Total RW  Size (RW Data + ZI Data)      127152
Total ROM Size (Code + RO Data + RW Data) 56100
```

| 项 | 迁移前 | 迁移后 | 说明 |
| --- | --- | --- | --- |
| Code | 31844 | 51940 | +20096 字节：主要是 littlefs 被真正引用后链入（此前虽编译却被链接器整体丢弃）；按键与手势状态机约 +1.0KB |
| RO-data | 323604 | **3748** | −319856 字节：只剩 `Font_t` / `Image_t` 描述表与图片指针表 |
| **Total ROM** | **355556** | **56100** | **−299456 字节（−84.2%）** |
| ZI-data | 128240 | 126740 | 堆由 95KB 降到 88KB（−7168 字节），抵消资源层与 OLED 清屏缓冲新增的约 5.7KB 静态 RAM 后仍净减 1500 字节 |

- Flash 约 **54.4KB / 512KB**（`Total RO Size`），余量充足。
- SRAM 约 **124.2KB / 128KB**（`Total RW Size`），**静态余量仅约 3.8KB**。
- 两大 RAM 占用：FreeRTOS 堆 **88KB** + LCD 渲染缓冲 **约 23KB**（`s_scratch`）。
- 图片乒乓缓冲 **15KB**（`s_picBuf` 两块）与 5 个任务栈（约 17KB，`Key_Task` 因含 `printf` 取 4KB）都从这 88KB 的 FreeRTOS 堆里分配，因此堆内实际余量约 **56KB**（不能用 88KB 直接减任务栈）。

> 迁移后 MCU 内**不含任何点阵与像素数据**，字库和图片全部来自 W25Q64。
> 改动代码后请以最新构建日志为准；新增较大静态缓冲前务必重新确认 `ZI-data`。

### 9.6 版本控制约定

仓库根目录的 `.gitignore` 忽略了 `MDK/Output/`（整个构建输出目录）、Keil 中间文件
（`*.o/*.crf/*.d/*.axf/*.map/*.sct/*.lnp/*.dep/*.iex/*.build_log.htm` 等）、
命令行构建日志 `MDK/build.log`、个人界面布局（`*.uvgui.*/*.uvguix.*`）、J-Link 日志，
以及 `MDK/.vscode/*.log`。

**仍然跟踪**：`STM32F407.uvprojx`、`STM32F407.uvoptx`、`MDK/DebugConfig/`、
`MDK/.vscode/c_cpp_properties.json`、`MDK/.vscode/settings.json`、`KeilClear.bat`。

### 9.7 清理中间文件

根目录 `KeilClear.bat` 会递归删除 `*.bak/*.lst/*.obj/*.crf/*.o/*.d/*.axf/*.map/*.sct/*.htm`
以及 `*.ddk/*.edk/*.lnp/*.mpf/*.mpj/*.omf/*.plg/*.rpt/*.tmp/*.__i/*.tra/*.dep/*.iex/*.scvd`
和 `JLinkLog.txt`。

⚠️ 它也会删除 `*.map`、`*.sct`、`*.htm`，即构建产物会被一并清掉（重新编译即可）。
它**不会**删除 `*.bin` / `*.hex` / `*.elf`。

### 9.8 资源烧录（首次使用必做）

正式固件**不含**资源数据，因此首次使用必须先烧录资源。资源总量 858778 字节（≈839KB）超过 512KB Flash，
需分三批编译烧录：

| 批次 | 内容 | 写入字节 | 编译后 RO-data |
| --- | --- | --- | --- |
| 1 | 汉字库 16 + 22 | 367990 | 369296 |
| 2 | 开机全屏图 + 主页全屏图 | 307208 | 308512 |
| 3 | 9 张 ASCII 表 + 30 张图标 | 183580 | 186064 |

> "写入字节"是 `Provision_Run()` 实际写入 W25Q64 的字节数；每张图片还要多 4 字节宽高头，
> 所以比"编译进 Flash 的点阵/像素数据"（批次 2 = 307200、批次 3 = 183460）分别多 8 / 120 字节。
> 批次 1 的字库是裸数据，两种口径相同（367990）。

操作：把 `User/Inc/BuildConfig.h` 的 `RESOURCE_PROVISION` 置 `1`、`PROVISION_BATCH` 依次改为 1 / 2 / 3，
**每次都要 Rebuild 后烧录**，上电等待串口打印 `[PROV] ... OK`；三批完成后把 `RESOURCE_PROVISION` 改回 `0`，
再 Rebuild 烧录正式固件。

> ⚠️ **切换批次后必须 Rebuild（不是 Build）**：否则 `Font_*.c` / `Image_*.c` 不会按新宏重新预处理，
> 会出现 `L6218E: Undefined symbol` 之类的链接错误。若已出现，执行 `Project → Clean Targets` 后 Rebuild。

> ⚠️ **批次 1 会强制重建文件系统**（清掉历史测试残留），所以**重跑批次 1 会清空批次 2、3 的数据**，
> 必须按 1 → 2 → 3 顺序完整执行。

> ⚠️ **`RESOURCE_DATA_IN_ROM` 的联动必须为 1**：`BuildConfig.h` 里
> `#if (RESOURCE_PROVISION == 1)` 会把 `RESOURCE_DATA_IN_ROM` 强制重定义为 **1**，这样资源数据本体
> 与其 `extern` 声明（`Font.h` / `Image.h` 中同受 `#if (RESOURCE_DATA_IN_ROM == 1)` 保护）才会参与编译。
> 若该联动被误写成 0，`Provision.c` 引用的 `Chinese_Font16_Data`、`Font_*_Table`、`gImage_*` 连声明都不存在，
> **烧录固件会在编译期直接报"标识符未声明"而无法构建**。

完整步骤、预期串口输出与故障排查见 **9.8 节**。

### 9.9 W25Q64 上的文件布局

| 路径 | 大小（字节） | 说明 |
| --- | --- | --- |
| `/font/cn16.bin` | 120160 | 3755 个 16×16 汉字（32 字节/字） |
| `/font/cn22.bin` | 247830 | 3755 个 22×22 汉字（66 字节/字） |
| `/font/as12.bin` … `/font/as48b.bin` | 1140 / 1520 / 1520 / 4180 / 4180 / 6080 / 6080 / 13680 / 13680 | 9 张纯 ASCII 点阵表 |
| `/img/boot.bin`、`/img/main.bin` | 153604 ×2 | 开机全屏图、主页全屏图（240×320，4 字节头 + RGB565） |
| `/img/*.bin`、`/img/w_*.bin` | 见迁移说明 | 30 张图标（4 字节宽高头 + RGB565 像素） |

---

## 10. 编译开关与裸机测试

`User/Inc/BuildConfig.h`：

```c
#define USE_FREERTOS   1        /* 1=FreeRTOS 调度(当前); 0=裸机模块测试 */

/* 资源存储开关（字库/图片已迁移到外部 W25Q64） */
#define RESOURCE_DATA_IN_ROM   0   /* 0=正式固件(资源不编译); 1=烧录固件 */
#define RESOURCE_PROVISION     0   /* 1=进入资源烧录流程 */
#define PROVISION_BATCH        1   /* 烧录批次 1..3 */

#if (RESOURCE_PROVISION == 1)
#undef  RESOURCE_DATA_IN_ROM
#define RESOURCE_DATA_IN_ROM   1   /* 联动: 烧录固件必然需要数据本体 */
#endif

#if (USE_FREERTOS == 0)
#ifndef BM_TEST_MODULE
#define BM_TEST_MODULE BM_TEST_MODULE_LFS   /* 1=OLED 2=光敏 3=DS1302 4=W25Q64 5=littlefs */
#endif
#endif
```

`main()` 共有**三种模式**：

| 宏组合 | 模式 | 行为 |
| --- | --- | --- |
| `RESOURCE_PROVISION = 1` | **资源烧录** | `Board_Peripheral_Init()` + `Usart2_Debug_Init()` → `Provision_Run()`；只初始化 W25Q64 与串口，**不启动 FreeRTOS、不初始化 LCD/SPI3** |
| `USE_FREERTOS = 1`（当前） | RTOS 运行 | `Board_Peripheral_Init()` → `App_Task_Init()` → `vTaskStartScheduler()`；夜间低功耗由 `app_task.c` 实现，DS1302 作为夜间时间源 |
| `USE_FREERTOS = 0` | 裸机测试 | `Board_Peripheral_Init()` → `Test()`（初始化测试 LED 并点亮）→ `TIM5_Init()` → `BareMetal_Module_Test()`，每个测试自带死循环 |

`BM_TEST_MODULE` 在 `BuildConfig.h` 中集中切换（**唯一入口**，`bare_test.c` 中已无同名变量）：

| 宏值 | 用例 | 内容 |
| --- | --- | --- |
| `BM_TEST_MODULE_OLED` | OLED | `OLED_Init` + 固定字符串 + 计数器，500ms 刷新 |
| `BM_TEST_MODULE_LIGHT` | 光敏 | `OLED` 显示 `ADC_VAL=...`（AO 模式）或 `DO_STATE=...`（DO 模式），500ms 刷新 |
| `BM_TEST_MODULE_DS1302` | DS1302 | 每秒读一次外部 RTC：OLED 显示日期 / `HH:MM:SS` / 读取计数，USART2 输出同样内容，并提供首次写入与回读校验 |
| `BM_TEST_MODULE_W25Q64` | W25Q64 | T1~T9：JEDEC / 器件 ID、页编程、扇区擦除、跨页写入、忙等待超时统计；测试区固定在末 4 个扇区（`0x7FC000`），不会碰到 littlefs 区域 |
| `BM_TEST_MODULE_LFS`（默认值） | littlefs | T1~T11：读写 / 目录 / 追加 / 64KB 大文件 / 遍历 / 删除 / 卸载重挂载持久化 / `SaveFont` / `SaveImage`+`LoadImage` / 缺失文件返回 `NOENT` |

裸机 `main()` 不调用 `Board_Init()`，因此 `BareMetal_Module_Test()` 会先调用 `Usart2_Debug_Init()` 初始化调试口；`fputc` 在调度器未运行时自动退化为逐字节轮询，`printf` 可直接使用。

**DS1302 裸机联调要点**（宏集中在 `bare_test.c` 顶部）：

| 宏 | 默认 | 作用 |
| --- | --- | --- |
| `DS1302_TEST_FORCE_SET` | `0` | `0` = 仅在读取失败或年份早于 `DS1302_TEST_MIN_YEAR` 时写入基准时间；`1` = 每次上电都写入。由于 `CH=1`（振荡器停）会让 `DS1302_ReadTime()` 直接失败，`FORCE_SET=0` 时也会自动落入"写入基准时间"分支并顺带启动振荡器；要验证断电保持需置回 `0` 并保证 `CH=0` |
| `DS1302_TEST_MIN_YEAR` | `2020` | 判定"RTC 未初始化"的年份下限 |
| `DS1302_TEST_YEAR` … `DS1302_TEST_SECOND` | `2026-09-13 12:00:00 W7` | 基准时间（星期 1=周一 … 7=周日） |
| `DS1302_TEST_HALT_LIMIT` | `3` | 连续读到相同秒值达到该次数即在界面提示 `HALT`（提示晶振停振 / 电池欠压） |
| `DS1302_TEST_LOG_PERIOD` | `10` | 串口日志周期（秒） |

测试输出与失败处理：

- 开机：`DS1302 init done`；若 RTC 未初始化（读取失败或年份早于下限）则写入基准时间并回读校验，打印 `SET VERIFY OK/FAIL`；否则打印 `RTC already running, keep existing time: ...`。
- 运行时：每秒读一次并刷新 OLED（日期 / `Font_32` 的 `HH:MM:SS` / 状态行），每 `DS1302_TEST_LOG_PERIOD` 秒打印一行 `read OK: ... (RD=... ER=...)`。
- 读取失败：`rd_err` 累加，OLED 保留上一次有效时间并把状态行显示为 `ER=n`；每秒重试，不阻塞、不死循环。
- 走时停滞：秒值连续 `DS1302_TEST_HALT_LIMIT` 次不变时，状态行显示 `HALT` 并打印一次告警。

> 注意：`DS1302_ReadTime()` 现在会先检查 CH 位，`CH=1` 时直接返回 `false`。因此在振荡器停走的情况下，
> 读数会先走进 `ER=n` 分支，而非走到 `HALT` 提示；`HALT` 检测主要覆盖"CH=0 但秒值因晶振/电池问题不前进"的场景。

> 说明：`DS1302_Init()` 已不再写 `0x80` 清零秒寄存器（只把写保护置为开启），否则每次上电都会把时间重置为 `00:00:00`，无法验证断电走时；启动振荡器由 `DS1302_SetTime()` 负责。
>
> 时序：**读操作采用手册标准时序**——数据由 DS1302 在 SCLK 下降沿输出、主机在 SCLK 上升沿采样；位操作半周期延时 `10µs`（`delay_us`，TIM5 基准），CE 拉高后同样等待 `10µs`。本模块 DAT 没有外部上拉、仅靠 MCU 内部约 40k 上拉，放宽半周期是必要的（实测 `ER=0`，读数与串口时间戳同步）。

---

## 11. 需要自行配置的参数

以下值硬编码在 `User/Src/App.c` 文件顶部（**当前仓库中为明文真实凭据，本文档按脱敏形式给出**）：

```c
const char *ssid     = "<YOUR_WIFI_SSID>";       /* 2.4GHz，ESP32-C3 不支持 5GHz */
const char *password = "<YOUR_WIFI_PASSWORD>";
const char *mac      = NULL;                      /* 非 NULL 时作为 AT+CWJAP 的 MAC 参数 */

static const char *weather_url =
    "https://api.seniverse.com/v3/weather/now.json"
    "?key=<YOUR_API_KEY>&location=<YOUR_CITY>&language=en&unit=c";
```

| 参数 | 说明 |
| --- | --- |
| `ssid` / `password` | 2.4GHz WiFi 名称与密码 |
| `mac` | 留 `NULL` 表示按 SSID 连接；填 BSSID 可指定 AP |
| `weather_url` | 心知天气（Seniverse）实况接口，需替换 `key`；`location` 支持城市拼音或经纬度，当前为 `dalian` |
| `language` | 当前 `en`；改为 `zh-Hans` 可让 `text` 字段返回中文（界面仍用本地映射表） |
| `unit` | `c` 摄氏度 |

> ⚠️ 仓库中 `App.c` 实际写的是真实 WiFi 密码与 API Key（单行字符串，不是多行拼接）。**对外发布前请先替换为占位符**，或把这些值抽到单独的、被 `.gitignore` 忽略的配置文件中。
>
> `location` 参数只影响天气数据本身（返回的城市名会填入 `weather_info.city`），**界面不再显示城市名** —— 顶部状态条只有 WiFi 图标与 SSID。

---

## 12. 串口调试日志

**USART2，115200-8N1**，接 USB-TTL 即可看到日志。

上电典型输出：

```
[SYS]Build Date:Sep 15 2026 20:53:12
[LCD ] ping-pong buffer ready: 7680 x 2 = 15360 bytes
[ASSET] W25Q64 jedec = 0xEF4017
[ASSET] littlefs mounted: 2048 blocks x 4096 B
[ASSET] selfcheck: checked=43, missing=0
[UI] Board init done, boot page
[NET] Init Start
[NET] AT init OK
[NET] WiFi init OK
[NET] WiFi connecting to <ssid> ...
[NET] WiFi connected: ssid=... bssid=... channel=6 rssi=-58
[NET] SNTP sync OK: 2026-09-15 20:53:26
[NET] Weather OK: Cloudy, code=4, temp=29.0
[UI] Boot net stage done: wifi=1 service=1
[UI] Enter main page
[LIGHT] task ready: DO_state=1 dark=1
```

运行期日志：

```
[NET] WiFi check OK: ssid=... rssi=-58
[NET] WiFi lost, reconnecting...
[NET] WiFi reconnected: ssid=... rssi=-55
[NET] AT init FAILED
[NET] WiFi connect FAILED
[NET] WiFi info FAILED
[NET] WiFi reconnect FAILED
[NET] SNTP sync FAILED
[NET] Weather HTTP FAILED
[NET] Weather parse FAILED
[SENSOR] DHT22 OK: T=26.3 H=54.1
[SENSOR] DHT22 FAIL: code=1 (no-ack)
```

昼夜切换与低功耗日志（验证第 8 章行为时重点看这几行）：

```
[LIGHT] debounce done: level=dark, mode=day      # 去抖结束后的电平与当前模式
[LIGHT] request NIGHT                            # 请求进入夜间
[LP] Sync RTC time done                          # 进入夜间前回写 DS1302 并回读校验(失败为 fail)
[LP] Enter night: LCD off, OLED on, time_source=RTC
[UI] Enter day: OLED off, LCD on                 # 退出夜间
[NET] LowPower exit, reset update                # Net_Task 收到补更请求
[LIGHT] request DAY
```

资源层日志（字库/图片全部来自 W25Q64，这组是显示异常的排障重点）：

```
[LCD ] ping-pong buffer ready: 7680 x 2 = 15360 bytes   # 图片乒乓缓冲分配成功
[ASSET] W25Q64 jedec = 0xEF4017                          # 芯片识别
[ASSET] littlefs mounted: 2048 blocks x 4096 B
[ASSET] selfcheck: checked=43, missing=0                 # 11 个字库 + 32 个图片资源
```

| 现象 | 含义 | 处理 |
| --- | --- | --- |
| `[ASSET] ERROR: W25Q64 not detected (jedec = 0x......)` | 芯片无响应 | 检查 PA4/PA5/PA6/PA7 与供电 |
| `[ASSET] ERROR: littlefs mount failed, err = n` | 未烧录资源 | 按 9.8 执行三批烧录；**固件刻意不自动格式化** |
| 43 项全部 `[ASSET] MISSING /font/cn16.bin err=-2` | 资源从未烧录（`err=-2` 即 `LFS_ERR_NOENT`）。**挂载成功不代表资源存在**——空的旧文件系统同样能挂载 | 同上 |
| 个别 `[ASSET] MISSING` / `[ASSET] BADSIZE xxx size=... want=...` | 对应批次未烧录或写入中断 | 重跑该批次 |
| `[ASSET] HINT: no resources on flash, flash the provision firmware first` | 上一条的显式提示行 | 同上 |
| `[UI] WARN: asset layer not ready, screen shows blank blocks` | UI 启动时资源层未就绪 | 同上 |
| `[LCD ] ping-pong buffer alloc failed, fallback to single buffer` | 堆空间不足 | 已自动退化为单缓冲（全屏图变慢约一倍），检查堆用量 |
| `[ERR]ST7789_DMA_Pump: DMA传输错误` / `[ERR]DMA transfer error` | SPI3 DMA 异常 | 检查 SPI3 与 DMA1_Stream5 配置 |
| `littlefs init success, block = n x 4096, total = n KB` | `lfs_port.c` 挂载成功 | 正常 |
| 屏幕只有纯色块、无任何文字 | 资源层未就绪 | 看 `[ASSET]` 日志 |

烧录固件（`RESOURCE_PROVISION = 1`）的日志形如：

```
[PROV] ==== resource provision ====
[PROV] batch = 1, fonts = 2, images = 0
[PROV] W25Q64 jedec = 0xEF4017
[PROV] batch 1: format filesystem first
[PROV] /font/cn16.bin             120160  OK
[PROV] /font/cn22.bin             247830  OK
[PROV] batch 1 done: PASS 2, FAIL 0, wrote 367990 bytes
[PROV] fs used blocks = 97 / 2048
```

判定"夜间确实暂停了更新"的方法：进入夜间后连续 ≥5 分钟**不应出现**下列任何一行：

```
[NET] WiFi check OK / WiFi lost, reconnecting / WiFi reconnected
[NET] SNTP sync OK / SNTP sync FAILED
[NET] Weather OK / Weather HTTP FAILED / Weather parse FAILED
[SENSOR] DHT22 OK / DHT22 FAIL
```

排查光敏不切换时的判读：

| 现象 | 结论 |
| --- | --- |
| 没有 `[LIGHT] task ready` | 光敏任务未创建（检查 `UI_Task` 是否走到创建任务那一步） |
| 遮挡 1s 后仍无 `[LIGHT] debounce done` | 电平从未变化 → 接线/供电/模块问题（现有 1s 轮询兜底，不会像纯中断方案那样"永远不动"） |
| `level` 始终为 `bright` | 极性相反（模块可能是"暗→DO 低"），需反转 `Light_Sensor_IsDark()` 的判定 |
| 有 `request NIGHT` 但无 `[LP] Enter night` | 问题在 `UI_Task` 的事件处理分支 |

DHT22 错误码（`BSP/Inc/DHT22.h`）：

| 码 | 宏 | 含义 |
| --- | --- | --- |
| 0 | `DHT22_OK` | 成功 |
| 1 | `DHT22_ERR_NO_ACK` | 总线未拉低，传感器无应答 |
| 2 | `DHT22_ERR_NO_HIGH` | 应答后未拉高 |
| 3 | `DHT22_ERR_NO_LOW` | 数据起始未拉低 |
| 4 | `DHT22_ERR_TIMEOUT` | 位读取超时 |
| 5 | `DHT22_ERR_CHECKSUM` | 校验和错误 |

硬件异常时：

```
Assertion failed in file ... at line ...
Stack overflow in task <name>
```

两者都会停在死循环中（便于在调试器里查看现场）。

---

## 13. 已知限制与待办

| # | 事项 | 说明 |
| --- | --- | --- |
| 1 | AO 模式的 ADC 通道未跟随引脚变更 | 光敏引脚已改为 `GPIOC + Pin0`（= `ADC123_IN10`），但 `Light_Sensor.c` 里仍是 `ADC_Channel_0`（PA0）。当前 `AO_DO_SWITCH = 0`（DO 模式）不编译该分支，切到 AO 模式前必须先改成 `ADC_Channel_10` |
| 2 | 光敏引脚与 EXTI 端口源必须成对修改 | `Light_Sensor.h` 中 GPIO 端口/引脚与 `..._EXTI_PORT_SOURCE`/`..._PIN_SOURCE` 不一致时中断永不触发（已踩过一次：GPIO 改到 PC1、EXTI 仍挂 PA1，表现为遮挡无任何反应）。另外该头文件顶部的引脚注释仍是旧的 `PA0/PA1` |
| 3 | `Board.c` 的 `Test()` 无条件拉低 PB2 | `Test()` 里无条件执行 `GPIO_ResetBits(GPIOB, GPIO_Pin_2)`，而 PB2 只在**裸机分支**用作测试 LED（FreeRTOS 分支用 PC5）。它与 DS1302 无关（DS1302 在 PE7/PE8/PE9），当前无害；建议把该语句放进 `#if (USE_FREERTOS == 0)` 分支 |
| 4 | `DS1302_ReadReg()` 未被引用 | `External_RTC.c` 中的单寄存器读函数始终没有调用者，全量重编会给出 `#177-D` 告警（不影响功能，可删可留） |
| 5 | 夜间只是"降载"，不是 MCU 睡眠 | `configUSE_TICKLESS_IDLE = 0`；夜间仅关屏 + 暂停网络/传感器 update，MCU 与 ESP32-C3 模组功耗不变。进一步省电需先做模组侧省电（`AT+SLEEP` 或硬件断电），再评估 Stop 模式 |
| 6 | 进入夜间时可能等一个 AT 事务结束 | 若 `Net_Task` 正在收发 AT（最长 10s），它会跑完本轮才进入暂停；这是刻意设计（不打断 AT 事务），代价是暂停生效最多延迟约 10s |
| 7 | 天气接口为第三方免费版 | 有调用频率限制，`key` 与配额由使用者自行申请 |
| 8 | 凭据硬编码 | WiFi 密码与天气 API Key 直接写在 `App.c`，建议后续抽到配置区或外部存储 |
| 9 | SRAM 静态余量小 | `Total RW Size ≈ 124.2KB / 128KB`，静态剩余约 3.8KB；新增静态缓冲前务必核算（堆内另有余量约 56KB，但 15KB 已被图片乒乓缓冲占用） |
| 10 | `Service_Room_Update()` 恒返回 `true` | DHT22 读取失败也会置 `EV_ROOM`，界面因此显示 `--`；如需区分成功/失败状态，需要修改返回值语义 |
| 11 | 裸机测试项为编译期固定 | `BM_TEST_MODULE` 是宏，切换测试项需改 `BuildConfig.h` 并重新编译 |
| 12 | 光敏 AO/DO 需重新编译切换 | `AO_DO_SWITCH` 是编译期宏，且 AO 阈值需按实际分压电路标定 |
| 13 | 依赖外网 | 无网络时天气保持默认（晴天图标 + 0.0）、白天时钟显示 `--:--`、WiFi 每 10s 重试 |
| 14 | TIM5 中断禁止调用 FreeRTOS API | 优先级 2 高于 `configMAX_SYSCALL_INTERRUPT_PRIORITY`（5），只能做计数 |
| 15 | 源码注释编码不统一 | 部分文件为 GB2312/GBK，部分是 UTF-8，另有大量纯 ASCII 文件；`Resource/Src/Image/Image_Boot_Page.c` 第 3 行为 GB2312 字节，其中两个字已丢失为 `?`（见第 15 节） |
| 16 | **字库与图片完全依赖 W25Q64** | 迁移后 MCU 内不含任何字模与像素数据；W25Q64 损坏或未烧录即完全无字可显。这是"全部移走"的固有代价，靠串口日志 + 纯色块提示兜底 |
| 17 | **资源烧录需要三轮编译烧录** | 资源总量 858778 字节超过 512KB Flash，无法单次编译带入，必须按 `PROVISION_BATCH` 分三批，且每批都要 Rebuild |
| 18 | **全屏图绘制约 58ms** | 双缓冲已把 W25Q64 读取时间完全隐藏在 DMA 发送之后，但 SPI3 @21MHz 发送 153600 字节本身就是这个量级；开机页与结果页各画一次。该值按 0.381µs/字节理论估算，代码里 `Prof_Us()` 目前没有调用者，未经实测 |
| 19 | **字模读取附加 littlefs 查找开销** | 单字模纯传输约 27µs（22 号汉字）/ 56µs（48 号 ASCII），实际还要叠加 `lfs_file_seek` 的 CTZ skip-list 遍历。若后续实测明显偏高，可加 4KB 块缓存。上表数值为设计估算，未经本机实测 |
| 20 | **DS1302 白天不停振** | 退出夜间后代码不再访问 DS1302，但芯片持续走时（这是"断电后仍能给出正确时间"的前提）。若为了几十纳安的待机电流去写 CH=1 停振，会牺牲该能力，得不偿失 |

---

## 14. 参考资料索引（Documents/）

| 目录 | 内容 |
| --- | --- |
| `Documents/README.md` | 本文档（原独立的"W25Q64 资源迁移说明"已并入 9.8、9.9 节） |
| `Documents/DS1302/` | DS1302 中/英文数据手册、实时时钟模块原理图（共 4 个 PDF，**不含参考工程源码**） |
| `Documents/ESP32-C3/` | ESP32-C3-MINI-1 AT 固件 `v4.1.1.0`（`esp-at.bin`、bootloader、分区表、factory 固件、烧录配置）、`flash_download_tool`、AT 用户指南（中英）、Release Note 与免责声明、模组图片、`AT固件下载.txt` |
| `Documents/STM32F4/` | STM32F407VET6 数据手册、STM32F4xx 参考手册（中文）、STM32F407VE 核心板原理图 V5.5 |
| `Documents/W25Q64/` | W25Q64BV 英文数据手册与 W25Q64 中文手册 |
| `Documents/屏幕/` | ST7789V 规格书、中景园 0.96" OLED 驱动芯片手册与使用文档 |
| `Documents/温湿度计/` | DHT22 / AM2302 产品规格书（中文） |
| `Documents/中文字库/` | 汉字点阵源文件 `Chinese_16.txt` / `Chinese_22.txt`（PCtoLCD2002 输出）与 `GB2312一级字库3755个汉字.txt` |

---

## 15. 编码与维护约定

### 15.1 源码编码现状

`User/`、`Resource/` 与 `Core/` 下混有 GB2312/GBK 与 UTF-8；`BSP/` 下 26 个文件（13 个 `Src/*.c` + 13 个 `Inc/*.h`）**全部是 UTF-8 或纯 ASCII**。共 67 个受检源文件（`User`/`Resource`/`BSP`/`Core` 下的 `.c/.h/.s`），均无 BOM。

| 类别 | 文件 |
| --- | --- |
| **GB2312/GBK**（非 UTF-8） | `Resource/Src/Font/Chinese_Font16.c`、`Resource/Src/Font/Chinese_Font22.c`、`User/Src/Main_Page.c`、`User/Src/main.c`、`Core/stm32f4xx.h`、`Core/system_stm32f4xx.c`、`Documents/中文字库/*.txt` |
| **UTF-8** | `README.md` 及 `Documents/*.md`、`Resource/Inc/Font.h`、`Resource/Inc/Image.h`、`Resource/Src/Image/ImageTable.c`、`User/Inc/app_task.h`、`User/Inc/bare_test.h`、`User/Inc/BuildConfig.h`、`User/Inc/Provision.h`、`User/Inc/main.h`、`User/Src/App.c`、`User/Src/app_task.c`、`User/Src/bare_test.c`、`User/Src/Boot_Page.c`、`User/Src/Board.c`、`User/Src/Provision.c`、`BSP/Src/Key.c`、`BSP/Inc/Key.h`、`BSP/` 下 26 个文件中的 23 个（其余 3 个为纯 ASCII） |
| **纯 ASCII**（无中文，编码无关） | `Resource/Src/Font/Font_*.c`（12/16/22/32/48）、`Resource/Src/Image/Image.c`、`Resource/Src/Image/Image_Main_Page.c`、`Resource/Src/Image/Image_Weather.c`、`User/Inc/App.h`、`User/Inc/Board.h`、`User/Inc/Page.h`、`User/Inc/stm32f4xx_conf.h`、`User/Inc/stm32f4xx_it.h`、`User/Src/stm32f4xx_it.c`、`BSP/Inc/I2C.h`、`BSP/Inc/OLED.h`、`BSP/Inc/Timer.h`、`Core/core_cm4.h`、`Core/core_cmFunc.h`、`Core/core_cmInstr.h`、`Core/core_cmSimd.h`、`Core/system_stm32f4xx.h`、`Core/Startup/startup_stm32f40xx.s` |

- VS Code 中若中文乱码，请手动切换编码（`GB2312` ↔ `UTF-8`）；**不要**用"保存为 UTF-8"的方式批量转换，否则 Keil 侧会再次乱码（此前 `Font_22.c` 的 `.name` 字符串就是这样被写坏的）。
- `Resource/Src/Image/Image_Boot_Page.c` 主体是 UTF-8，但**第 3 行整行是 GB2312 字节**，且其中两个字已丢失为 ASCII `?`——宽解码后实际内容为 `/* 开机页? 未连? 等待连接 */`（按语义应还原为「开机页面 未连接 等待连接」）。该行会让严格 UTF-8 解码报 2 个非法序列，编辑器打开会提示编码错误，但不影响编译。
- 行尾不统一，但**每个文件内部是单一 EOL**（67 个受检源文件中无一个混用）：CRLF 43 个、LF 24 个。
  - **CRLF**：`Core/` 下全部 8 个文件、`Resource/Src/Font`（7 个）与 `Resource/Src/Image`（5 个中的 4 个，除 `ImageTable.c`）、`User/Inc` 的 `App.h`/`Board.h`/`main.h`/`Page.h`/`stm32f4xx_conf.h`/`stm32f4xx_it.h`、`User/Src` 的 `Board.c`/`Main_Page.c`/`Provision.c`/`stm32f4xx_it.c`、`BSP` 的 `AT.*`/`DHT22.h`/`I2C.*`/`Key.*`/`LCD.h`/`Light_Sensor.h`/`OLED.*`/`W25Q64.*`/`External_RTC.c`。
  - **LF**：`User/Inc` 的 `app_task.h`/`bare_test.h`/`BuildConfig.h`/`Provision.h`、`User/Src` 的 `main.c`/`App.c`/`app_task.c`/`bare_test.c`/`Boot_Page.c`、`Resource/Inc/Font.h`/`Resource/Inc/Image.h`/`Resource/Src/Image/ImageTable.c`、`BSP` 的 `Asset.*`/`External_RTC.h`/`Profiling.*`/`Timer.*`/`Usart.*`/`DHT22.c`/`LCD.c`/`Light_Sensor.c`，以及 `README.md`、`uvprojx`、`uvoptx`、`KeilClear.bat`。
- 判断方法：用 UTF-8 打开若报 encoding 错误即为 GB2312；`git diff` 出现整文件重写通常也是编码不一致导致。

### 15.2 字符串编码

屏幕上显示的中文以 **GB2312 双字节** 直接写入源码（如 `Main_Page.c` 的 `weather_map[].chinese` 中的 `"晴"`、`"多云"`），`ST7789_Write_String()` 通过 `Is_GB2312()` 判定双字节、再按 `(区码-0xB0)*94 + (位码-0xA1)` 算字模偏移。**新增中文界面文字时请保持 GB2312 编码写入**，否则字模定位失败、汉字显示为空白。

### 15.3 字库约定（迁移后）

- 汉字点阵集中在 `/font/cn16.bin`（16×16，32 字节/字）与 `/font/cn22.bin`（22×22，66 字节/字），均为 **3755 个 GB2312 一级汉字**，顺序严格等于区位码顺序（啊…座）。
- 定位公式 `offset = ((区码-0xB0) × 94 + (位码-0xA1)) × 每字字节数`，**O(1)**，不再按名字线性查找（旧实现每个汉字平均要 `strcmp` 1878 次）。
- 点阵格式：**横向取模、行优先**，每行 `(宽+7)/8` 字节，**每字节低位 bit0 对应最左侧像素**。重新生成字库时必须沿用这套取模设置，否则整份数据错位。
- `Font_*.c` 中原有的内嵌汉字表（`Chinese_Font_16B_Table` / `Chinese_Font_22B_Table`）已删除，`Font_16B` / `Font_22B` 的 `cn_id` 改为指向 `ASSET_FID_CN16` / `ASSET_FID_CN22`。
- 图片格式：**4 字节大端头（宽、高）+ RGB565 小端像素**，与 `SaveImage()` / `Provision_SaveImage()` 写入格式一致，`Asset` 自检按此核对文件大小。

### 15.4 资源备份

`Resource/Src` 下的资源数据仍完整保留，仅被 `#if (RESOURCE_DATA_IN_ROM == 1) && (PROVISION_BATCH == N)` 排除在正式固件之外。需要重新烧录 W25Q64 时改 `BuildConfig.h` 即可，无需改动这些文件。注意 `ImageTable.c`、`Boot_Page.c`、`Main_Page.c` 没有该守卫，正式固件照常编译。

### 15.5 其它

- **本 README**：UTF-8（无 BOM），LF 行尾，可安全用任意 Markdown 工具渲染。
- **修改引脚或优先级**：请同步更新本文档第 6、7 节；**修改 DS1302 或光敏引脚时，注意代码注释里可能残留旧的引脚号**（当前 `BuildConfig.h`、`bare_test.c` 注释写 DS1302 为 `PB0/PB1/PB2`，`Light_Sensor.h` 注释写 AO/DO 为 `PA0/PA1`，均与宏定义不符）。

---

*文档基于仓库快照 2026-09-15 编写；项目持续演进时，请以 `User/`、`BSP/` 源码与 `MDK/STM32F407.uvprojx` 为准。*
