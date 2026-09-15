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
- **FreeRTOS** 负责调度：显示、联网、传感器、光敏各自独立任务，通过事件组通信；
- **夜晚进入低功耗模式**：关闭彩屏、只保留 OLED 显示时间，并暂停 WiFi / SNTP / 天气 / 室内温湿度的周期更新；退出时立即补更一次。

时间基准白天为 **SNTP 校时 + TIM5 1ms 计数** 推算的软件时钟；夜间切换到 **DS1302 外部 RTC**（进入夜间前用网络时间回写并回读校验），因此夜间不依赖网络也能正常走时。

---

## 2. 功能特性

### 主界面（彩屏 240×320）

- 顶部状态条：WiFi 图标 / SSID（过长自动截断为 `[xxxxx...]`）/ 定位图标与 `[大连]` 标签，无定位时显示无定位图标。
- 大号时钟：`Font_48B` 绘制 `HH:MM`，冒号随秒奇偶闪烁；未校时显示 `--:--`。
- 日期行：`YYYY/MM/DD` + 英文星期（`Monday` … `Sunday`）。
- 实况天气卡片：26 条天气码映射（复用 22 张图标）→ 图标 + 中文（`晴 / 多云 / 阴 / 阵雨 / 雷阵雨 / 小雨 … 霾`），未知天气码回落为晴天；下方显示温度 `xx.x` + `C`。
- 室内温湿度卡片：温度 `C` 与湿度 `%`，DHT22 读取失败时显示 `--`。

### 开机流程

1. 上电 → LED 自检 → `TIM5` → `ST7789_Init` → `USART2` 调试口。
2. 显示开机底图 `Boot_Page_Waitconnect`。
3. 创建 `dht22` / `net` 任务，等待联网阶段结束（最长 **30s** 兜底）。
4. 显示连接结果页：
   - 成功：`[SSID] Connect` / `Mac: xx:xx:...` / `Channel: n, RSSI: -nn`；
   - WiFi 失败：错误图标 + `[WiFi] Disconnect`；
   - 服务失败（SNTP 或天气）：错误图标 + `[Service] Init failed`。
5. 停留 **2.5s** → 绘制主界面 → 启动光敏任务（保证昼夜切换只发生在开机完成之后）。

### 夜间低功耗模式与双屏切换

- **变暗**：用系统时间回写 DS1302（回读校验）→ 置 `s_lowpower = true` → 打开 OLED 并从 DS1302 读时间显示 → 关闭 ST7789 显示（`0x28` 显示睡眠）与背光。
- **夜间**：`netTask` / `DHT22_Task` **冻结周期计数器、不执行任何 update**；`uiTask` 事件等待放宽到 `EV_LP_UI_TICK_MS`（2s），OLED 仅在时/分/日变化时重绘。
- **变亮**：关闭 OLED → 恢复 ST7789（`0x29` + 背光）→ 整屏重绘主界面 → 清 `s_lowpower` 并置 `EV_NET_UPDATE_NOW | EV_SENSOR_UPDATE_NOW`，两个任务各立即补一次 update。
- 切换由光敏任务发起，等待 uiTask 回 `EV_LOWPOWER_ACK`（超时 2s），避免竞态。
- 光敏任务**以中断为主、1000ms 超时轮询兜底**：即使 EXTI 未触发，也能在 1 秒内感知亮度变化并切换。

### 其他

- 串口调试日志：`USART2 @115200-8N1`，行缓冲 + DMA 发送，多任务整行原子输出。
- 网络异常自恢复：WiFi 掉线自动重连，重连成功后立即补一次 SNTP 与天气。

---

## 3. 硬件平台

| 项目 | 参数 |
| --- | --- |
| MCU | STM32F407VET6（Cortex-M4F，512KB Flash，128KB SRAM1 + 64KB CCM） |
| 系统时钟 | 168MHz（`HSE = 25MHz`，`PLL_M=25 / PLL_N=336 / PLL_P=2`） |
| 彩屏 | ST7789V，SPI3，240×320，RGB565 |
| 夜间显示 | SSD1306 0.96" OLED，128×64，软 I2C，从地址 `0x3C` |
| 联网 | ESP32-C3-MINI-1，AT 固件 `v4.1.1.0`，USART1 |
| 室内传感器 | DHT22 / AM2302 单总线 |
| 环境光 | 光敏电阻模块，DO（EXTI）或 AO（ADC + 模拟看门狗）二选一 |
| 调试口 | USART2 @115200 |
| 外部 RTC | DS1302 模块（PB0=RST / PB1=IO / PB2=CLK），夜间低功耗模式的时间源 |

> ⚠️ **晶振注意**：Keil 工程 `Cpu` 串中的 `CLOCK(12000000)` 只影响调试器的 Xtal 设置，**不参与时钟树计算**。
> 实际参与计算的是 `Core/stm32f4xx.h` 的 `HSE_VALUE`（当前 `STM32F40_41xxx` 分支默认 **25000000**）。
> 若你的核心板是 8MHz 晶振，必须同步修改 `HSE_VALUE` 与 `system_stm32f4xx.c` 的 `PLL_M`，否则串口波特率、TIM5 计时、SPI 速率全部偏移。

---

## 4. 目录结构

```
Project4/
├── User/                          # 应用层
│   ├── stm32f4xx_it.c/.h          # 中断向量服务（NMI/HardFault/... 桩函数）
│   ├── stm32f4xx_conf.h           # 标准外设库裁剪配置
│   ├── Inc/
│   │   ├── main.h                 # 仅汇聚 C 标准库头
│   │   ├── Board.h                # 板级初始化声明
│   │   ├── BuildConfig.h          # 编译开关（USE_FREERTOS / 资源烧录）
│   │   ├── App.h                  # 联网/传感器服务与软件时钟接口
│   │   ├── app_task.h             # 任务、事件位、周期参数定义
│   │   ├── Provision.h            # 资源烧录固件接口
│   │   └── bare_test.h            # 裸机模块测试接口
│   ├── Src/
│   │   ├── main.c                 # 入口：资源烧录 / RTOS / 裸机 三种模式
│   │   ├── Board.c                # 时钟使能、LED、TIM5、USART2、LCD、资源层初始化
│   │   ├── App.c                  # WiFi / SNTP / 天气 / DHT22 服务 + 软件时钟
│   │   ├── app_task.c             # uiTask / netTask / DHT22_Task / LightSensor_Task
│   │   ├── Provision.c            # 资源烧录实现（仅 RESOURCE_PROVISION=1 时编译）
│   │   └── bare_test.c            # 裸机模块测试用例
│   └── Third_Resource/
│       └── Screen_Resource/
│           ├── Inc/               # Font.h / Image.h / Page.h
│           └── Src/
│               ├── Font/          # Font_12/16/22/32/48 + Chinese_Font16/22（资源备份）
│               ├── Image/         # Image.c / Image_Boot_Page.c / Image_Main_Page.c /
│               │                  # Image_Weather.c / ImageTable.c（资源清单）
│               └── Page/          # Boot_Page.c（开机页）/ Main_Page.c（主界面）
├── BSP/                           # 板级驱动
│   ├── Inc/ · Src/
│   │   ├── Asset.c                # 资源层：挂载 littlefs、开机自检、O(1) 字模读取
│   │   ├── Profiling.c            # 基于 DWT->CYCCNT 的微秒级计时
│   │   ├── AT.c                   # ESP32-C3 AT 指令、WiFi/CWSTATE/CWJAP、SNTP、HTTP、JSON 解析
│   │   ├── DHT22.c                # 单总线时序、校验和、错误码
│   │   ├── External_RTC.c         # DS1302 驱动（裸机测试 + 夜间低功耗时间源）
│   │   ├── I2C.c                  # 通用软件 I2C
│   │   ├── LCD.c                  # ST7789：SPI3 + DMA1_Stream5、字模渲染、图片双缓冲流式显示
│   │   ├── Light_Sensor.c         # DO(EXTI1) 或 AO(ADC1_IN0 + AWD) 两种模式
│   │   ├── OLED.c                 # SSD1306 命令/数据、取字模、`OLED_ShowClock`
│   │   ├── Timer.c                # TIM5 1ms 基准 + `delay_us/delay_ms`
│   │   ├── Usart.c                # USART1 RX 环形缓冲 + USART2 printf(DMA)
│   │   └── W25Q64.c               # SPI1 Flash 驱动（读 / 页编程 / 扇区擦除 / 忙等待超时）
├── Core/                          # CMSIS 内核 + 启动文件 + 系统时钟
│   ├── Startup/startup_stm32f40xx.s
│   └── Startup/startup_stm32f429_439xx.s   # 未加入工程
├── STM32F4xx_StdPeriph_Driver/    # STM32F4 标准外设库
├── Third_Lib/
│   ├── FreeRTOS/                  # FreeRTOS 内核
│   │   └── portable/FreeRTOSConfig.h
│   └── LittleFS/                  # littlefs v2.11
│       ├── Src/lfs.c · lfs_util.c
│       ├── Src/lfs_port.c         # W25Q64 读/写/擦除回调 + 挂载/格式化
│       └── LittleFS_Operation/    # SaveFont / LoadFont / SaveImage / LoadImage
├── MDK/                           # Keil MDK5 工程
│   ├── STM32F407.uvprojx          # 工程文件（入口）
│   ├── DebugConfig/               # 调试器配置
│   └── Output/                    # 构建产物 .axf/.map/.htm/.o
├── Documents/                     # 器件资料、参考工程与本说明文档
└── KeilClear.bat                  # 递归清理 Keil 中间文件
```

> **字库与图片已迁移到外部 W25Q64**：`Screen_Resource` 下的 `Font_*.c`、`Chinese_Font*.c`、
> `Image*.c` 里的点阵/像素数据仍完整保留，但被 `#if` 排除在正式固件之外（详见第 9、10 节
> 与 `Documents/W25Q64资源迁移说明.md`）。

---

## 5. 软件架构

### 分层

```
应用层      User/Src : main.c / Board.c / App.c / app_task.c / Provision.c / Page/*.c
板级驱动    BSP/Src  : Asset / LCD / OLED / AT / I2C / DHT22 / Light_Sensor / Timer / Usart / W25Q64
操作系统    Third_Lib/FreeRTOS（portable/FreeRTOSConfig.h）
文件系统    Third_Lib/LittleFS（lfs_port.c 对接 W25Q64，LittleFS_Operation 提供读写封装）
硬件抽象    STM32F4xx_StdPeriph_Driver + Core（CMSIS / 启动 / system_stm32f4xx.c）
```

设计约定：

- **LCD 只由 uiTask 写**；netTask/sensor 任务只更新数据并置事件位。
- **USART1（AT）只由 netTask 使用**，避免 AT 响应串扰。
- **共享数据用临界区整块替换**：`wifi_info`、`weather_info`、`room_info`、软件时钟的 `epoch/sync_ms` 都在 `taskENTER_CRITICAL()` 内整体赋值，uiTask 不会读到半写状态。
- **字模与图片只从 W25Q64 读**：MCU 内不再保存任何点阵/像素数据。`Asset` 层在 `Board_Init()` 中挂载 littlefs 并逐项自检；资源缺失时该项渲染退化为纯色填充，不阻塞任务、不重启、不自动格式化。

### 任务一览（`User/Src/app_task.c`）

| 任务 | 优先级 | 栈（字 / 字节） | 职责 | 周期 |
| --- | --- | --- | --- | --- |
| `uiTask` | 3 | 1024 / 4KB | 创建事件组、`Board_Init`、开机页 → 创建 `dht22`/`net` → 等 `EV_NET_READY`（30s 超时）→ 结果页 → 主界面 → 创建光敏任务；事件驱动刷新，昼夜切换执行者 | 白天 500ms / 夜间 `EV_LP_UI_TICK_MS`(2s) |
| `DHT22_Task` | 4 | 512 / 2KB | 等待 10s 超时即为采集周期，同时响应 `EV_SENSOR_UPDATE_NOW` 立即补采；低功耗期间跳过；优先级高于 UI，避免抢占破坏 us 级时序 | 10s |
| `netTask` | 2 | 1024 / 4KB | 独占 USART1/AT；开机联网 + SNTP + 天气；1s 心跳推进三个周期计数器，响应 `EV_NET_UPDATE_NOW` 立即补更；低功耗期间冻结计数器 | 1s 心跳 |
| `LightSensor_Task` | 2 | 512 / 2KB | 注册中断回调、初始化光敏；等待中断（1000ms 超时轮询兜底）→ 2s 去抖 → 昼夜切换并等 ACK | 事件驱动 + 1s 兜底 |

`netTask` 周期参数（`User/Inc/app_task.h`）：

| 项 | 正常周期 | 失败重试 |
| --- | --- | --- |
| WiFi 检查 | `WIFI_PERIOD_S = 60s` | `RETRY_WIFI_S = 10s` |
| SNTP 校时 | `SNTP_PERIOD_S = 4h` | `RETRY_SNTP_S = 5s` |
| 天气更新 | `WEATHER_PERIOD_S = 1h` | `RETRY_WEATHER_S = 60s` |

其它常量：

- `DEBOUNCE_MS = 2000`（昼夜切换去抖 2s）
- `EV_LP_UI_TICK_MS = 2000`（夜间 `uiTask` 事件等待超时；夜间只显示到分钟）
- `RTC_LOWPOWER_ENABLE = 1`（夜间时间源：`1` = DS1302，`0` = 软件时钟）
- `configTICK_RATE_HZ = 1000`、`configMAX_PRIORITIES = 5`（优先级 0~4）
- `configUSE_TIME_SLICING = 0`、`configTOTAL_HEAP_SIZE = 88KB`（heap_4）
- `configCHECK_FOR_STACK_OVERFLOW = 2`；`configASSERT` 失败进入 `vAssertCalled`
- 未开启 tickless idle（`configUSE_TICKLESS_IDLE = 0`）

### 事件位（`User/Inc/app_task.h`）

| 位 | 名称 | 生产者 | 含义 |
| --- | --- | --- | --- |
| bit0 | `EV_WEATHER` | netTask | 天气已更新，刷新天气模块 |
| bit1 | `EV_ROOM` | DHT22_Task | 房间温湿度已更新 |
| bit2 | `EV_NET_READY` | netTask | 开机网络阶段结束（无论成败） |
| bit3 | `EV_WIFI` | netTask | WiFi 状态变化，刷新顶部状态条 |
| bit4 | `EV_LOWERPOWER` | LightSensor_Task | 进入夜间 |
| bit5 | `EV_LOWPOWER_ACK` | uiTask | 昼夜切换完成确认 |
| bit6 | `EV_WAKEUP` | LightSensor_Task | 回到白天 |
| bit7 | `EV_NET_UPDATE_NOW` | uiTask | 退出低功耗：`netTask` 立即补 WiFi+SNTP+天气（唯一消费者，清除式等待） |
| bit8 | `EV_SENSOR_UPDATE_NOW` | uiTask | 退出低功耗：`DHT22_Task` 立即补采（唯一消费者，清除式等待） |

> ⚠️ 这两个补更事件位**必须分开**：`xEventGroupWaitBits` 使用清除式等待，若两个任务共用一个位，先到的任务会把位清掉，另一个任务就永远收不到通知（表现为“退出夜间后只有一半数据被刷新”）。

### 数据流

```
ESP32-C3 ──USART1──▶ AT.c ──▶ wifi_info   ──▶ Main_Page_Net_Update / Boot_Page_Show
                       │
                       ├──▶ AT_SNTP_Get_Time ──▶ Clock_Sync ──▶ 软件时钟（TIM5 推算）
                       │                                          └─▶ 主界面时钟 / OLED 时钟
                       └──▶ AT_Get_HTTP ──▶ Parse_Weather_Response ──▶ weather_info ──▶ 天气卡片

DHT22 ──▶ DHT22_ReadData ──▶ room_info ──▶ 室内温湿度卡片
光敏中断 ──▶ vTaskNotifyGiveFromISR ──▶ LightSensor_Task（2s 去抖）──▶ 事件组 ──▶ uiTask 切屏

夜间（低功耗）:
进入夜间前: 软件时钟 ──▶ DS1302_SetTime（回读校验）
夜间:       DS1302_ReadTime ──▶ 成功 ──▶ OLED 显示
                            └── 失败 ──▶ 软件时钟降级（打印 time_source=SoftClock）
退出夜间:   uiTask ──▶ EV_NET_UPDATE_NOW ─────▶ netTask 立即 WiFi+SNTP+天气
                    └─▶ EV_SENSOR_UPDATE_NOW ──▶ DHT22_Task 立即采集

资源（字库/图片，全部来自 W25Q64）:
W25Q64 ──SPI1 21MHz──▶ Asset 层 ──▶ 字模: (区码-0xB0)*94+(位码-0xA1) 直接算偏移，O(1)
                                └─▶ 图片: 分块流式读取 + DMA1_Stream5 乒乓双缓冲 ──SPI3──▶ ST7789
```

---

## 6. 外设与引脚分配

### USART1 — ESP32-C3 AT 模组

| 信号 | 引脚 | 说明 |
| --- | --- | --- |
| USART1_TX | PA9 | AF7，115200-8N1 |
| USART1_RX | PA10 | AF7，RXNE 中断 → 512 字节环形缓冲 |

接收路径：`USART1_IRQHandler`（`BSP/Src/Usart.c`）逐字节入环形缓冲，AT 层用 `Usart1_RX_Count()` / `Usart1_RX_Read()` 轮询取出，等待期间 `vTaskDelay(1)` 让出 CPU。缓冲满则丢弃。

### USART2 — 调试输出

| 信号 | 引脚 | 说明 |
| --- | --- | --- |
| USART2_TX | PA2 | AF7，115200-8N1，DMA1_Stream6 / Channel4 |
| USART2_RX | PA3 | AF7（当前未使用） |

`printf` 重定向到 `fputc`：按行加锁（互斥量 + 行主记录），遇 `'\n'` 或缓冲满（256B）时以 DMA 整行发出；调度器未运行 / 中断上下文中退化为逐字节轮询。

### SPI3 — ST7789V 彩屏

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
  在 `ST7789_Init()` 里从 FreeRTOS 堆分配（15KB）。

### SPI1 — W25Q64 Flash

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
- 存放全部字库与图片（约 822KB，占 247/2048 块）；文件布局见第 9 节与迁移说明文档。
- **SPI1 与 SPI3 同为 21MHz**——这正是图片双缓冲能把 W25Q64 读取时间完全隐藏在 DMA 发送之后的前提。

### 软件 I2C — SSD1306 OLED

| 信号 | 引脚 | 说明 |
| --- | --- | --- |
| SCL | PB6 | 开漏 + 内部上拉 |
| SDA | PB7 | 开漏 + 内部上拉 |
| 从地址 | `0x3C` | 命令前缀 `0x00`，数据前缀 `0x40` |

### 传感器 / ADC / 定时器

| 功能 | 引脚 | 说明 |
| --- | --- | --- |
| DHT22 DATA | PE6 | 单总线，输出/输入切换，输入上拉 |
| 光敏 AO | PC0 | `GPIOC + GPIO_Pin_0`，对应 `ADC123_IN10`（驱动里的 ADC 通道仍是 `ADC_Channel_0`，见第 13 节） |
| 光敏 DO | PC1 | `GPIOC + GPIO_Pin_1` → `EXTI1`（端口源为 `EXTI_PortSourceGPIOC`），上升沿/下降沿双沿触发 |
| 测试 LED | PC5 | 推挽输出，低电平点亮 |
| DS1302 外部 RTC | PB0=RST / PB1=IO / PB2=CLK | 裸机测试项 `BM_TEST_MODULE_DS1302` + 夜间低功耗时间源；`DS1302_Init()` 只置写保护，不清零秒寄存器 |

开关宏：`BSP/Inc/Light_Sensor.h` 中 `AO_DO_SWITCH`，**默认 `0`（DO 模式）**；改为 `1` 使用 ADC + 模拟看门狗。AO 模式的阈值常量：`LIGHT_SENSOR_AO_DARK_TH = 3000`、`LIGHT_SENSOR_AO_LIGHT_TH = 2000`（迟滞回防抖），极性由 `LIGHT_SENSOR_AO_DARK_HIGH` 决定。

DO 模式判定：DO 引脚为**高**（光照未达电位器阈值）→ 视为“暗”。

> ⚠️ 改光敏引脚时，`BSP/Inc/Light_Sensor.h` 里的 **GPIO 端口/引脚**与 **`LIGHT_SENSOR_DO_EXTI_PORT_SOURCE` / `..._PIN_SOURCE`** 必须成对修改。两者不一致时 EXTI 中断源仍挂在旧端口上，**电平变化不会产生任何中断**，表现为“遮挡后无反应、串口也无日志”。

---

## 7. 中断优先级与 FreeRTOS 约束

NVIC 分组为 **`NVIC_PriorityGroup_4`**（4 位全为抢占优先级）。

| 中断 | 抢占优先级 | 能否调用 FreeRTOS API |
| --- | --- | --- |
| PendSV / SysTick | 15（`configKERNEL_INTERRUPT_PRIORITY`） | — |
| **TIM5** | **2** | ❌ **禁止**（优先级数值 < `configMAX_SYSCALL_INTERRUPT_PRIORITY`） |
| USART1 | 5 | ✅ 可调用 `...FromISR` |
| EXTI1（光敏 DO，PC1） | 5 | ✅ 可调用 `...FromISR` |
| ADC（光敏 AO） | 5 | ✅ 可调用 `...FromISR` |

规则（`Third_Lib/FreeRTOS/portable/FreeRTOSConfig.h` 中有对应注释）：

- `configMAX_SYSCALL_INTERRUPT_PRIORITY = 5 << 4`；优先级数值 **≥ 5** 的中断才允许调用 FreeRTOS API。
- `TIM5_IRQHandler` 优先级为 2，**只做 `TIM5_ms++`**，不得加锁、不得调用任何 FreeRTOS API。
- 光敏 ISR 只做 `vTaskNotifyGiveFromISR` + `portYIELD_FROM_ISR`，去抖与显示逻辑都在任务里完成。

---

## 8. 关键流程说明

### 8.1 AT 初始化与联网时序

```
AT_Init():
    GPIO/ USART1 / NVIC(优先级5) 初始化
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
| `name` | `city[32]` | 是否拿到定位（非空即显示定位图标） |
| `path` | `location[128]` | 备用 |
| `text` | `weather[16]` | 英文天气描述（当前界面改用中文映射表显示） |
| `code` | `weather_code` | 查 26 项 `weather_map`（复用 22 张图标）得图标 + 中文 |
| `temperature` | `temperature` | 天气卡片温度 |

主界面顶部固定显示 GB2312 字面量 `[大连]`（`Main_Page.c` 中以字节数组硬编码），与 URL 的 `location=dalian` 对应。

### 8.3 软件时钟

- `Clock_Sync(&date_info)`：校验 `year >= 2000` 与月份合法后，用 `Clock_DaysFromCivil()`（Howard Hinnant 民用历算法）把日期换算为 Unix 时间戳，与当前 `TIM5_Get_ms()` 一起在临界区内保存。
- `Clock_GetDateTime()`：临界区内一次性取出 `synced / epoch / sync_ms / now_ms`，再算 `epoch + elapsed_ms/1000`，最后用 `Clock_CivilFromDays()` 反算年月日与星期（`((days+3)%7+7)%7+1`，1=周一）。
- 未同步时返回全 0，界面据此绘制 `--:--` / 空日期，因此 **上电后到首次 SNTP 成功之间不会显示错误时间**。

### 8.4 昼夜切换与低功耗握手

```
LightSensor_Task                          uiTask                      netTask / DHT22_Task
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

夜间状态由 uiTask 的 `s_lcd_on / s_oled_on` 记录（`s_lowpower` 供各任务查询），事件刷新只在对应屏幕开启时执行；OLED 时钟仅在**时、分或日期变化**时重绘，时间源为 DS1302（失败回退软件时钟）。

夜间 `netTask` / `DHT22_Task` 的关键约定：

- **冻结周期计数器**：`if (s_lowpower) continue;` 放在递减之前。若边暂停边递减，退出夜间时三个计数器会同时归零，导致同一秒内连发多次 AT 事务。
- **补更走独立事件位**：`netTask` 等 `EV_NET_UPDATE_NOW`、`DHT22_Task` 等 `EV_SENSOR_UPDATE_NOW`，均为清除式等待，互不抢占。
- **正在执行的 AT 事务不被打断**：进入夜间时若 `netTask` 正在收发 AT（最长 10s），本轮跑完后下一拍才进入跳过状态；`uiTask` 不等待 netTask，仍立即回 ACK。

### 8.5 DS1302 夜间时间源

- 三个内部静态辅助函数（`User/Src/app_task.c`）：`RTC_Ensure_Init()`（懒初始化）、`RTC_Sync_From_SystemClock()`（用网络时间回写并回读校验）、`RTC_ReadDataTime()`（优先读 DS1302，失败回退软件时钟并返回来源标志）。
- 回读校验：`DS1302_SetTime()` **恒返回 `true`**（无写校验），因此必须用 `DS1302_ReadTime()` 回读比对；比较“当日分钟总数”而非单独比较分钟字段，允许写入耗时造成的 2 分钟偏差且能跨小时。
- 星期语义统一为 DS1302 的 **1~7**：`DS1302_ReadTime()` 对 `week < 1` 直接判失败，若写入星期 0（旧写法 `weekday - 1`），周一会同步失败。
- 失败降级：`RTC_LOWPOWER_ENABLE = 0` 或 DS1302 读取失败时，夜间改用软件时钟显示，日志打印 `time_source=SoftClock`。

### 8.6 串口输出

`printf` 走 `fputc` → 行缓冲 → DMA。整行输出由互斥量与“行属主”保证不与其他任务交叉；中断上下文或调度器未运行时退化为轮询，保证 HardFault/启动期日志也能打印（`vAssertCalled`、`vApplicationStackOverflowHook` 都依赖它）。

---

## 9. 编译与烧录

### 环境

| 项 | 版本 |
| --- | --- |
| IDE | Keil MDK-ARM **Plus 5.24.1** |
| 编译器 | **ARMCC V5.06 update 5（build 528）**，即 AC5（非 AC6） |
| C 标准 | C99 已开启 |
| 工程 | `MDK/STM32F407.uvprojx` |
| Target | `STM32F407` |
| 器件 | `STM32F407VETx` |

### 关键配置（已在工程中设好，无需手动添加）

| 配置 | 值 |
| --- | --- |
| 预处理宏 | `USE_STDPERIPH_DRIVER,STM32F40_41xxx` |
| 包含路径 | `..\User`、`..\User\Inc`、`..\User\Third_Resource\Screen_Resource\Inc`、`..\BSP\Inc`、`..\Core`、`..\Core\Startup`、`..\STM32F4xx_StdPeriph_Driver\inc`、`..\STM32F4xx_StdPeriph_Driver\src`、`..\Third_Lib\FreeRTOS\include`、`..\Third_Lib\FreeRTOS\portable`、`..\Third_Lib\LittleFS\Inc`、`..\Third_Lib\LittleFS\LittleFS_Operation\Inc` |
| Output | `.\Output\`，名字 `STM32F407` |
| 启动文件 | `Core/Startup/startup_stm32f40xx.s` |

### 构建产物

| 文件 | 说明 |
| --- | --- |
| `MDK/Output/STM32F407.axf` | 可执行映像（默认产物） |
| `MDK/Output/STM32F407.map` | 链接映射表（内存分布/符号） |
| `MDK/Output/STM32F407.htm` | 静态调用图 |
| `MDK/Output/STM32F407.build_log.htm` | 构建日志 |

**不生成 `.hex`**（`CreateHexFile = 0`）。工程里虽然保存了 `fromelf --bin` 的 User 命令，但 **`RunUserProg1 = 0` 未启用**，所以 `.bin` 不会自动刷新 —— 仓库中的 `MDK/Output/STM32F407.bin` 是 2025-03-07 的旧文件，请勿直接使用。

需要 `.bin` 时的做法（不改工程也可以）：

1. `Project → Options for Target → User`，勾选 `Run #1`（命令已填好：`"$K\ARM\ARMCC\bin\fromelf.exe" --bin --output="$L@L.bin" "#L"`），重新编译；或
2. 手动执行：`D:\Keil5\ARM\ARMCC\bin\fromelf.exe --bin --output=MDK\Output\STM32F407.bin MDK\Output\STM32F407.axf`。

### 下载 / 调试

- 默认调试器为 **ULINK2（`UL2CM3`）**，Flash 算法 `STM32F4xx_512`（起始 `0x08000000`，大小 `0x80000`）。
- 使用 ST-Link / J-Link 时在 `Options for Target → Debug` 切换即可。
- 调试器配置文件：`MDK/DebugConfig/STM32F407_STM32F407VETx.dbgconf`。
- 烧录后 ESP32-C3 需预先烧录 AT 固件（见 `Documents/ESP32-C3/`），默认波特率 **115200**。

### 清理中间文件

根目录 `KeilClear.bat` 会递归删除 `*.bak/*.lst/*.obj/*.crf/*.o/*.d/*.axf/*.map/*.sct/*.htm` 等。
⚠️ 它也会删除 `*.map`、`*.sct`、`*.htm`，即构建产物会被一并清掉（重新编译即可）。

### 资源占用（参考值，来自 2026-09-15 构建日志）

迁移前（字库与图片编译进 MCU）：

```text
Program Size: Code=31844  RO-data=323604  RW-data=360  ZI-data=128240
Total ROM Size: 355556
```

迁移后（资源存放在 W25Q64，**当前状态**）：

```text
Program Size: Code=51044  RO-data=3748  RW-data=396  ZI-data=126612
Total ROM Size: 54804
```

| 项 | 迁移前 | 迁移后 | 说明 |
| --- | --- | --- | --- |
| Code | 31844 | 51044 | +19.2KB：littlefs 被真正引用后链入（此前虽编译却被链接器整体丢弃） |
| RO-data | 323604 | **3748** | −312KB：只剩 `Font_t` / `Image_t` 描述表与图片指针表 |
| **Total ROM** | **355556** | **54804** | **−300752 字节（−84.6%）** |
| ZI-data | 128240 | 126612 | 堆从 95KB 下调到 88KB，抵消了资源层新增的约 4.2KB 静态 RAM |

- Flash 约 **55KB / 512KB**（`Code + RO-data`），余量充足。
- SRAM1 约 **124KB / 128KB**（`RW-data + ZI-data`），**静态余量仅约 4KB**。
- 两大 RAM 占用：FreeRTOS 堆 **88KB** + LCD 渲染缓冲 **约 23KB**（`s_scratch`）。
- 图片乒乓缓冲 **15KB** 从 FreeRTOS 堆分配（不占静态 RAM）；堆内实际只用约 13.6KB 任务栈，余量约 74KB。

> 迁移后 MCU 内**不含任何点阵与像素数据**，字库和图片全部来自 W25Q64。
> 改动代码后请以最新构建日志为准；新增较大静态缓冲前务必重新确认 `ZI-data`。

### 资源烧录（首次使用必做）

正式固件**不含**资源数据，因此首次使用必须先烧录资源。资源总量约 822KB 超过 512KB Flash，
需分三批编译烧录：

| 批次 | 内容 | 编译后 RO-data |
| --- | --- | --- |
| 1 | 汉字库 16 + 22（367990 字节） | 369296 |
| 2 | 开机全屏图 + 主页全屏图（307200 字节） | 308512 |
| 3 | 9 张 ASCII 表 + 32 张图标（183460 字节） | 186064 |

操作：改 `User/Inc/BuildConfig.h` 的 `PROVISION_BATCH` 为 1 / 2 / 3，**每次都要 Rebuild 后烧录**，
上电等待串口打印 `[PROV] ... OK`；三批完成后把 `RESOURCE_PROVISION` 改回 `0`，再 Rebuild 烧录正式固件。

> ⚠️ **切换批次后必须 Rebuild（不是 Build）**：否则 `Font_*.c` / `Image_*.c` 不会按新宏重新预处理，
> 会出现 `L6218E: Undefined symbol` 之类的链接错误。若已出现，执行 `Project → Clean Targets` 后 Rebuild。

> ⚠️ **批次 1 会强制重建文件系统**（清掉历史测试残留），所以**重跑批次 1 会清空批次 2、3 的数据**，
> 必须按 1 → 2 → 3 顺序完整执行。

完整步骤、预期串口输出与故障排查见 **`Documents/W25Q64资源迁移说明.md`**。

---

## 10. 编译开关与裸机测试

`User/Inc/BuildConfig.h`：

```c
#define USE_FREERTOS   1        /* 1=FreeRTOS 调度(当前); 0=裸机模块测试 */

/* 资源存储开关（字库/图片已迁移到外部 W25Q64） */
#define RESOURCE_DATA_IN_ROM   0   /* 0=正式固件(资源不编译); 1=烧录固件 */
#define RESOURCE_PROVISION     0   /* 1=进入资源烧录流程 */
#define PROVISION_BATCH        1   /* 烧录批次 1..3 */

#if (USE_FREERTOS == 0)
#define BM_TEST_MODULE BM_TEST_MODULE_LFS   /* 1=OLED 2=光敏 3=DS1302 4=W25Q64 5=littlefs */
#endif
```

`main()` 共有**三种模式**：

| 宏组合 | 模式 | 行为 |
| --- | --- | --- |
| `RESOURCE_PROVISION = 1` | **资源烧录** | `Board_Peripheral_Init()` + `Usart2_Debug_Init()` → `Provision_Run()`；只初始化 W25Q64 与串口，**不启动 FreeRTOS、不初始化 LCD/SPI3** |
| `USE_FREERTOS = 1`（当前） | RTOS 运行 | `Board_Peripheral_Init()` → `App_Task_Init()` → `vTaskStartScheduler()`；夜间低功耗由 `app_task.c` 实现，DS1302 作为夜间时间源 |
| `USE_FREERTOS = 0` | 裸机测试 | `Board_Peripheral_Init()` + `TIM5_Init()` → `BareMetal_Module_Test()`，每个测试自带死循环 |

`RESOURCE_DATA_IN_ROM` 无需手动改：`RESOURCE_PROVISION == 1` 时自动置 1。
资源烧录的完整流程见第 9 节「资源烧录」。

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
| `DS1302_TEST_FORCE_SET` | `0` | `0` = 仅在读取失败或年份早于 `DS1302_TEST_MIN_YEAR` 时写入基准时间；`1` = 每次上电都写入（若读到 `CH=1` 时钟停走，可置 `1` 强制写入一次以启动振荡器；验证断电保持必须置回 `0`） |
| `DS1302_TEST_MIN_YEAR` | `2020` | 判定“RTC 未初始化”的年份下限 |
| `DS1302_TEST_YEAR` … `DS1302_TEST_SECOND` | `2026-09-13 12:00:00 W7` | 基准时间（星期 1=周一 … 7=周日） |
| `DS1302_TEST_HALT_LIMIT` | `3` | 连续读到相同秒值达到该次数即在界面提示 `HALT`（提示晶振停振 / 电池欠压） |
| `DS1302_TEST_LOG_PERIOD` | `10` | 串口日志周期（秒） |

测试输出与失败处理：

- 开机：`DS1302 init done`；若 RTC 未初始化（读取失败或年份早于下限）则写入基准时间并回读校验，打印 `SET VERIFY OK/FAIL`；否则打印 `RTC already running, keep existing time: ...`。
- 运行时：每秒读一次并刷新 OLED（日期 / `Font_32` 的 `HH:MM:SS` / 状态行），每 `DS1302_TEST_LOG_PERIOD` 秒打印一行 `read OK: ... (RD=... ER=...)`。
- 读取失败：`rd_err` 累加，OLED 保留上一次有效时间并把状态行显示为 `ER=n`；每秒重试，不阻塞、不死循环。
- 走时停滞：秒值连续 `DS1302_TEST_HALT_LIMIT` 次不变时，状态行显示 `HALT` 并打印一次告警（`CH` 位被 `DS1302_ReadTime()` 屏蔽，只能用这种方式间接判断）。

观察顺序：首次上电（`SET VERIFY OK`，OLED 开始走秒）→ 按复位（时间应继续而非回到基准）→ 断 VCC 10s 再上电（电池保持，时间连续）→ 拔掉模块（OLED 显示 `ER=n`、串口告警，程序不卡死）。

> 说明：`DS1302_Init()` 已不再写 `0x80` 清零秒寄存器（只把写保护置为开启），否则每次上电都会把时间重置为 `00:00:00`，无法验证断电走时；启动振荡器由 `DS1302_SetTime()` 负责。
>
> 时序：**读操作采用手册标准时序**——数据由 DS1302 在 SCLK 下降沿输出、主机在 SCLK 上升沿采样；位操作半周期延时 `10µs`（`delay_us`，TIM5 基准），CE 拉高后同样等待 `10µs`。本模块 DAT 没有外部上拉、仅靠 MCU 内部约 40k 上拉，放宽半周期是必要的（实测 `ER=0`，读数与串口时间戳同步）。

---

## 11. 需要自行配置的参数

以下值硬编码在 `User/Src/App.c` 文件顶部，**首次使用必须替换为你自己的值**（本文档不记录真实凭据）：

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

> 主界面顶部固定显示 `[大连]`（GB2312 字面量），换城市时需同步修改 `User/Third_Resource/Screen_Resource/Src/Page/Main_Page.c` 中 `loc_label[]` 的字节序列。

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
[NET] LowPower exit, reset update                # netTask 收到补更请求
[LIGHT] request DAY
```

资源层日志（字库/图片全部来自 W25Q64，这组是显示异常的排障重点）：

```
[LCD ] ping-pong buffer ready: 7680 x 2 = 15360 bytes   # 图片乒乓缓冲分配成功
[ASSET] W25Q64 jedec = 0xEF4017                          # 芯片识别
[ASSET] littlefs mounted: 2048 blocks x 4096 B
[ASSET] selfcheck: checked=43, missing=0                 # 11 个字库 + 32 张图片
```

| 现象 | 含义 | 处理 |
| --- | --- | --- |
| `[ASSET] ERROR: W25Q64 not detected` | 芯片无响应 | 检查 PA4/PA5/PA6/PA7 与供电 |
| `[ASSET] ERROR: littlefs mount failed` | 未烧录资源 | 按第 9 节执行三批烧录；**固件刻意不自动格式化** |
| 43 项全部 `MISSING ... err=-2` | 资源从未烧录（`err=-2` 即 `LFS_ERR_NOENT`）。**挂载成功不代表资源存在**——空的旧文件系统同样能挂载 | 同上 |
| 个别 `MISSING` / `BADSIZE` | 对应批次未烧录或写入中断 | 重跑该批次 |
| `[ASSET] HINT: no resources on flash` | 上一条的显式提示行 | 同上 |
| `[LCD ] ping-pong buffer alloc failed` | 堆空间不足 | 已自动退化为单缓冲（全屏图变慢约一倍），检查堆用量 |
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

判定“夜间确实暂停了更新”的方法：进入夜间后连续 ≥5 分钟**不应出现**下列任何一行：

```
[NET] WiFi check OK / WiFi lost, reconnecting / WiFi reconnected
[NET] SNTP sync OK / SNTP sync FAILED
[NET] Weather OK / Weather HTTP FAILED / Weather parse FAILED
[SENSOR] DHT22 OK / DHT22 FAIL
```

排查光敏不切换时的判读：

| 现象 | 结论 |
| --- | --- |
| 没有 `[LIGHT] task ready` | 光敏任务未创建（检查 `uiTask` 是否走到创建任务那一步） |
| 遮挡 1s 后仍无 `[LIGHT] debounce done` | 电平从未变化 → 接线/供电/模块问题（现有 1s 轮询兜底，不会像纯中断方案那样“永远不动”） |
| `level` 始终为 `bright` | 极性相反（模块可能是“暗→DO 低”），需反转 `Light_Sensor_IsDark()` 的判定 |
| 有 `request NIGHT` 但无 `[LP] Enter night` | 问题在 `uiTask` 的事件处理分支 |

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
| 2 | 光敏引脚与 EXTI 端口源必须成对修改 | `Light_Sensor.h` 中 GPIO 端口/引脚与 `..._EXTI_PORT_SOURCE`/`..._PIN_SOURCE` 不一致时中断永不触发（已踩过一次：GPIO 改到 PC1、EXTI 仍挂 PA1，表现为遮挡无任何反应） |
| 3 | `Board.c` 的 `Test()` 会拉低 PB2 | `Test()` 里无条件执行 `GPIO_ResetBits(GPIOB, GPIO_Pin_2)`，而 PB2 是 DS1302 的 CLK。因它在 `DS1302_Init()` 之前执行、之后会被重新配置，当前无害；建议把它放进 `#if (USE_FREERTOS == 1)` 分支避免将来踩坑 |
| 4 | `DS1302_ReadReg()` 未被引用 | `External_RTC.c` 中的单寄存器读函数始终没有调用者，全量重编会给出 `#177-D` 告警（不影响功能，可删可留） |
| 5 | 夜间只是“降载”，不是 MCU 睡眠 | `configUSE_TICKLESS_IDLE = 0`；夜间仅关屏 + 暂停网络/传感器 update，MCU 与 ESP32-C3 模组功耗不变。进一步省电需先做模组侧省电（`AT+SLEEP` 或硬件断电），再评估 Stop 模式 |
| 6 | 进入夜间时可能等一个 AT 事务结束 | 若 `netTask` 正在收发 AT（最长 10s），它会跑完本轮才进入暂停；这是刻意设计（不打断 AT 事务），代价是暂停生效最多延迟约 10s |
| 7 | 天气接口为第三方免费版 | 有调用频率限制，`key` 与配额由使用者自行申请 |
| 8 | 凭据硬编码 | WiFi 密码与天气 API Key 直接写在 `App.c`，建议后续抽到配置区或外部存储 |
| 9 | SRAM 静态余量小 | `ZI-data ≈ 126.6KB / 128KB`，静态剩余约 4KB；新增静态缓冲前务必核算（堆内另有余量约 74KB） |
| 10 | `Service_Room_Update()` 恒返回 `true` | DHT22 读取失败也会置 `EV_ROOM`，界面因此显示 `--`；如需区分成功/失败状态，需要修改返回值语义 |
| 11 | 裸机测试项为编译期固定 | `BM_TEST_MODULE` 是宏，切换测试项需改 `BuildConfig.h` 并重新编译 |
| 12 | 光敏 AO/DO 需重新编译切换 | `AO_DO_SWITCH` 是编译期宏，且 AO 阈值需按实际分压电路标定 |
| 13 | 依赖外网 | 无网络时天气保持默认（晴天图标 + 0.0）、白天时钟显示 `--:--`、WiFi 每 10s 重试 |
| 14 | TIM5 中断禁止调用 FreeRTOS API | 优先级 2 高于 `configMAX_SYSCALL_INTERRUPT_PRIORITY`（5），只能做计数 |
| 15 | 源码注释编码为 GB2312/GBK | 部分文件混排 UTF-8；用 UTF-8 打开会显示乱码（见下一节） |
| 16 | **字库与图片完全依赖 W25Q64** | 迁移后 MCU 内不含任何字模与像素数据；W25Q64 损坏或未烧录即完全无字可显。这是"全部移走"的固有代价，靠串口日志 + 纯色块提示兜底 |
| 17 | **资源烧录需要三轮编译烧录** | 资源总量约 822KB 超过 512KB Flash，无法单次编译带入，必须按 `PROVISION_BATCH` 分三批，且每批都要 Rebuild |
| 18 | **全屏图绘制约 58ms** | 双缓冲已把 W25Q64 读取时间完全隐藏在 DMA 发送之后，但 SPI3 @21MHz 发送 153600 字节本身就是这个量级；开机页与结果页各画一次 |
| 19 | **字模读取附加 littlefs 查找开销** | 单字模纯传输约 27µs（22 号汉字）/ 56µs（48 号 ASCII），实际还要叠加 `lfs_file_seek` 的 CTZ skip-list 遍历。若后续实测明显偏高，可加 4KB 块缓存（见迁移说明第 8 节） |

---

## 14. 参考资料索引（Documents/）

| 目录 | 内容 |
| --- | --- |
| `Documents/DS1302/` | DS1302 中/英文数据手册、模块原理图，以及两个 Keil C51 参考工程（LCD1602 显示 / 串口显示，含 STARTUP.A51） |
| `Documents/ESP32-C3/` | ESP32-C3-MINI-1 AT 固件 `v4.1.1.0`（`esp-at.bin`、bootloader、分区表、factory 固件）、`flash_download_tool_3.9.11.exe`、AT 用户指南（中英）、`AT固件下载.txt`（官方烧录指引链接） |
| `Documents/STM32F4/` | STM32F407VET6 数据手册、STM32F4xx 参考手册（中文）、STM32F407VE 核心板原理图 V5.5 |
| `Documents/屏幕/` | ST7789V 规格书、中景园 0.96" OLED 驱动芯片手册与使用文档 |
| `Documents/温湿度计/` | DHT22 / AM2302 产品规格书（中文） |
| `Documents/W25Q64资源迁移说明.md` | 字库/图片迁移的完整说明：W25Q64 文件布局、三批烧录步骤、预期串口输出、故障排查表、性能测量方法 |
| `Documents/中文字库/` | 汉字点阵源文件 `Chinese_16.txt` / `Chinese_22.txt`（PCtoLCD2002 输出）与 GB2312 一级字库字表 |

---

## 15. 编码与维护约定

- **源码注释编码**：`User/`、`BSP/` 下**混排 GB2312/GBK 与 UTF-8**。VS Code 中若中文乱码，请手动切换编码（`GB2312` ↔ `UTF-8`）；**不要**用“保存为 UTF-8”的方式批量转换，否则 Keil 侧会再次乱码。
  - **GB2312**：`Font_*.c`、`Chinese_Font*.c`、`Image*.c`、`Main_Page.c`、`main.c`、`App.c`
  - **UTF-8**：`README.md` 及 `Documents/*.md`、`Font.h`、`Image.h`、`Asset.*`、`Provision.*`、`Profiling.*`、`BuildConfig.h`、`Boot_Page.c`、`Board.c`、`app_task.c`、`bare_test.c`
  - 判断方法：用 UTF-8 打开若报 encoding 错误即为 GB2312；`git diff` 出现整文件重写通常也是编码不一致导致。
- **字符串编码**：屏幕上显示的中文以 **GB2312 双字节** 直接写入源码（例如 `Main_Page.c` 的 `loc_label[]`），`ST7789_Write_String()` 通过 `Is_GB2312()` 取区位码。**新增中文界面文字时请保持 GB2312 编码写入**，否则字模定位失败、汉字显示为空白。
- **字库约定（迁移后）**：
  - 汉字点阵集中在 `/font/cn16.bin`（16×16，32 字节/字）与 `/font/cn22.bin`（22×22，66 字节/字），均为 **3755 个 GB2312 一级汉字**，顺序严格等于区位码顺序（啊…座）。
  - 定位公式 `offset = ((区码-0xB0) × 94 + (位码-0xA1)) × 每字字节数`，**O(1)**，不再按名字线性查找（旧实现每个汉字平均要 `strcmp` 1878 次）。
  - 点阵格式：**横向取模、行优先**，每行 `(宽+7)/8` 字节，**每字节低位 bit0 对应最左侧像素**。重新生成字库时必须沿用这套取模设置，否则整份数据错位。
  - `Font_*.c` 中原有的内嵌汉字表（`Chinese_Font_16B_Table` / `Chinese_Font_22B_Table`）已删除，`Font_16B` / `Font_22B` 改为指向上述两个完整字库。
- **资源备份**：`Screen_Resource` 下的资源数据仍完整保留，仅被 `#if (RESOURCE_DATA_IN_ROM == 1)` 排除在正式固件之外。需要重新烧录 W25Q64 时改 `BuildConfig.h` 即可，无需改动这些文件。
- **本 README**：UTF-8（无 BOM），可安全用任意 Markdown 工具渲染。
- **新增源文件**：需同时加入 `MDK/STM32F407.uvprojx` 的对应分组（USER / USER/Src / USER/Third_Resource / BSP），并确保头文件目录已在包含路径中。
- **修改引脚或优先级**：请同步更新本文档第 6、7 节。

---

*文档基于仓库快照 2026-09-15 编写；项目持续演进时，请以 `User/`、`BSP/` 源码与 `MDK/STM32F407.uvprojx` 为准。*
