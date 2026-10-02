# STM32F407 桌面天气时钟

> 基于 **STM32F407VET6 + FreeRTOS** 的桌面天气时钟 / 室内环境监测终端。
> 白天用 2.4" SPI 彩屏显示时钟、日期、实况天气与室内温湿度；夜间自动切到 0.96" OLED 只显示时间，
> 同时冻结周期任务、让 ESP32-C3 进入 Modem-sleep。
> 时间基准是 **SNTP 校时 + TIM5 推算的软件时钟**，夜间改用 **DS1302 外部 RTC**，断网也能正常走时。

| 项目 | 内容 |
| --- | --- |
| 主控 | STM32F407VETx（Cortex-M4F @168MHz，512KB Flash，128KB SRAM + 64KB CCM） |
| 操作系统 | FreeRTOS：5 个应用任务、tick 1kHz、heap_4、88KB 堆 |
| 文件系统 | littlefs v2.11，挂载在 W25Q64（8MB SPI Flash） |
| 联网 | ESP32-C3-MINI-1（AT 固件 v4.1.1.0），USART1 @115200 |
| 显示 | ST7789V 240×320 彩屏（SPI3 + DMA）/ SSD1306 128×64 OLED（软件 I2C） |
| 工具链 | Keil MDK 5.24.1 / ARMCC V5.06 update 5（AC5）/ C99 |
| 固件规模 | `Code 54004 · RO-data 3744 · RW-data 608 · ZI-data 127648`（最近一次构建） |
| 外置资源 | 11 个字库 + 32 张图片 = 858778 B（≈839KB），全部在 W25Q64；MCU 内点阵/像素数据 **0 字节** |

本文档聚焦**代码架构与主要机制**；具体数值以源码为准。

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
| W25Q64 | CS=PA4 · CLK=PA5 · MISO=PA6 · MOSI=PA7（SPI1/AF5） |
| ESP32-C3 | TX=PA9 · RX=PA10（USART1/AF7） |
| 调试串口 | TX=PA2 · RX=PA3（USART2/AF7） |
| ATGM336H 定位 | TX=PB10 · RX=PB11（USART3/AF7） |
| SSD1306 OLED | SCL=PB6 · SDA=PB7（软件 I2C，开漏 + 内部上拉） |
| DHT22 | DATA=PE6（单总线） |
| DS1302 | RST=PE7 · IO=PE8 · CLK=PE9 |
| 光敏电阻 | DO=PC1（EXTI1 双沿）· AO=PC0（ADC123_IN10） |
| 用户按键 | PA0（下拉输入 + EXTI0 双沿，兼 WKUP1） |
| 测试 LED | PC5（RTOS）/ PB2（裸机），低电平点亮 |

### 1.3 外设配置

| 外设 | 关键参数 |
| --- | --- |
| USART1 | 115200-8N1，RXNE 中断 + 512B 环形缓冲，NVIC 抢占 5 |
| USART2 | 115200-8N1，TX 走 DMA1_Stream6/Ch4，**无中断**，轮询 TCIF |
| USART3 | 9600-8N1，RXNE 中断 + 512B 环形缓冲，NVIC 抢占 5 |
| SPI1 | 主机 · 8 位 · Mode 0 · 预分频 4 → **21MHz** |
| SPI3 | 主机 · Mode 0 · 预分频 2 → **21MHz**，TX 走 DMA1_Stream5/Ch0 |
| TIM5 | PSC=83（1MHz 计数）· ARR=999（1ms 中断）· NVIC 抢占 **2** |
| ADC1 | 仅光敏 AO 模式使用，当前 `AO_DO_SWITCH = 0` 不参与编译 |

> SPI1 与 SPI3 同为 21MHz 不是巧合：这是图片乒乓双缓冲能把 W25Q64 读取时间完全隐藏在
> SPI3 发送之后的前提，改动任一侧分频前需一并评估。

---

## 2. 代码架构

### 2.1 分层

```
应用层      User/          main / Board / App / app_task（装配层）/ Provision / bare_test /
                           stm32f4xx_it / Boot_Page / Main_Page
任务层      Task/          任务装配与各任务独立实现（UI_Task / Net_Task / DHT22_Task /
                           LightSensor_Task / Key_Task + Task_Common / Task_Config）
资源层      BSP/Asset.c   littlefs 挂载、开机自检、O(1) 字模与图片读取
板级驱动    BSP/           LCD · OLED · AT · ATGM336H · Utf8_Gb2312 · I2C · DHT22 ·
                           External_RTC · Light_Sensor · Key · Timer · Usart · W25Q64 · Profiling
操作系统    Third_Lib/     FreeRTOS（portable/FreeRTOSConfig.h）
文件系统    Third_Lib/     littlefs v2.11（lfs_port.c 对接 W25Q64，LFS_Operation 封装读写）
硬件抽象    Core/ + SPL   CMSIS + STM32F4 标准外设库
资源数据    Resource/     字库与图片的源码内备份（正式固件不编译）
```

依赖方向**自上而下单向**：应用层 → 资源层 / 板级驱动 → FreeRTOS / littlefs → 硬件抽象。
BSP 层只操作硬件，不含业务逻辑，也不反向调用 `User/`。

### 2.2 目录结构

```
Project4/
├── Task/                       任务层（Inc 7 个头 / Src 6 个源，见 2.3）
│   ├── Inc/  Task_Config.h · Task_Common.h · UI_Task.h · Net_Task.h ·
│   │         DHT22_Task.h · LightSensor_Task.h · Key_Task.h
│   └── Src/  Task_Common.c · UI_Task.c · Net_Task.c · DHT22_Task.c ·
│             LightSensor_Task.c · Key_Task.c
├── User/                       应用层（Inc 10 个头 / Src 9 个源）
│   ├── Inc/  main.h · Board.h · BuildConfig.h · App.h · app_task.h ·
│   │         Provision.h · bare_test.h · Page.h · stm32f4xx_conf.h · stm32f4xx_it.h
│   └── Src/  main.c · Board.c · App.c · app_task.c（任务装配层）· Provision.c ·
│             bare_test.c · stm32f4xx_it.c · Boot_Page.c · Main_Page.c
├── BSP/                        板级驱动（Inc 与 Src 各 15 个文件，见 2.3）
├── Resource/                   资源数据（正式固件不编译）
│   ├── Inc/  Font.h（字库描述与文件 ID）· Image.h（图片描述）
│   └── Src/  Font/  Font_12/16/22/32/48.c + Chinese_Font16/22.c
│             Image/ Image.c · Image_Boot_Page.c · Image_Main_Page.c ·
│                    Image_Weather.c · ImageTable.c（图片清单 32 项）
├── Core/                       CMSIS + 启动文件 + 系统时钟
├── STM32F4xx_StdPeriph_Driver/ STM32F4 标准外设库
├── Third_Lib/
│   ├── FreeRTOS/               内核 + include/ + portable/
│   └── LittleFS/               littlefs v2.11 + LFS_Operation
├── MDK/                        Keil 工程（STM32F407.uvprojx）
└── Documents/                  本说明、器件手册与工具
```

> **路径对应关系容易混淆，特别注意**：Keil 工程里任务层的**分组名是 `USER/Task`**，但磁盘位置
> 是项目根的 `Task/`（与 `User/` 平级），并**不在 `User/` 之下**——分组 `USER/Src` 才对应
> `User/Src/`。工程包含路径里加的是 `..\Task\Inc`。新增任务文件时挂到 `USER/Task` 分组即可。

### 2.3 模块职责

**应用层（`User/Src`）**

| 文件 | 职责 |
| --- | --- |
| `main.c` | 三种构建模式的唯一入口；`configASSERT` 与栈溢出钩子的实现 |
| `Board.c` | 板级初始化：时钟、GPIO、各外设时钟使能、测试 LED |
| `App.c` | 服务层：WiFi / SNTP / 天气 / 温湿度 / 软件时钟；共享数据的整块赋值 |
| `app_task.c` | 任务装配层：只负责创建 `ui` 任务；任务实现见下行 |
| `Page.h` + `Boot_Page.c` / `Main_Page.c` | 界面绘制与页面布局常量（`Page.h` 是界面的公共头） |
| `Provision.c` | 资源烧录：把源码内备份的字库/图片写入 W25Q64 |
| `bare_test.c` | 裸机模块测试用例（W25Q64 / littlefs / DS1302 / 光敏 / OLED / GNSS） |
| `stm32f4xx_it.c` | 中断桩 |

**任务层（`Task/Src`）**

| 文件 | 职责 |
| --- | --- |
| `Task_Common.c/.h` | 共享底座：事件组 `Task_Events()` 的创建与获取、低功耗标志 `g_lowpower` |
| `Task_Config.h` | 任务参数集中配置：优先级、栈大小、各周期、去抖与按键时序 |
| `UI_Task.c/.h` | `ui` 任务：板级初始化、开机画面与结果页、事件循环刷新；昼夜进出的**执行者** |
| `Net_Task.c/.h` | `net` 任务：AT/WiFi/SNTP/天气，对外只暴露启动与开机结果读取 |
| `DHT22_Task.c/.h` | `dht22` 任务：单总线温湿度周期采集 |
| `LightSensor_Task.c/.h` | `light_sensor` 任务：光照去抖与昼夜状态机；对外暴露"请求切换昼夜" |
| `Key_Task.c/.h` | `key` 任务：PA0 手势状态机；长按调用光敏的请求接口，不直接改其状态 |

**板级驱动（`BSP/Src`）**

| 文件 | 职责 |
| --- | --- |
| `Asset.c` | 资源层：挂载 littlefs、开机自检、按 ID 直接定位字模与图片 |
| `LCD.c` | ST7789：SPI3 + DMA、字模渲染、图片流式读取与乒乓双缓冲 |
| `OLED.c` | SSD1306：命令/数据、取字模、夜间时钟显示 |
| `AT.c` | ESP32-C3 AT 指令、WiFi/SNTP/HTTP、天气/IP 响应解析 |
| `ATGM336H.c` | 北斗+GPS：NMEA 装配、校验和、RMC 解析（**接口预留，当前无调用者**） |
| `Utf8_Gb2312.c` | UTF-8 → GB2312 一级汉字转码（**预留，当前无调用者**） |
| `I2C.c` | 通用软件 I2C |
| `DHT22.c` | 单总线时序、校验和、错误码 |
| `External_RTC.c` | DS1302 读写（夜间时间源 + 裸机测试项） |
| `Light_Sensor.c` | 光敏 DO（EXTI1）/ AO（ADC1 + 模拟看门狗）两种模式 |
| `Key.c` | PA0 按键，EXTI0 双边沿上报，不做手势识别 |
| `Timer.c` | TIM5 1ms 基准 + `delay_us` / `delay_ms` |
| `Usart.c` | USART1/USART3 RX 环形缓冲；USART2 printf（DMA + 整行互斥） |
| `W25Q64.c` | SPI Flash：读 / 页编程 / 扇区擦除 / 忙等待超时统计 |
| `Profiling.c` | 基于 DWT->CYCCNT 的微秒级计时，用于测量渲染耗时 |

### 2.4 关键约定

1. **LCD 只由 `UI_Task` 写**。其他任务只更新数据并置事件位，避免多任务同时操作 SPI3 造成撕裂。
2. **USART1（AT）只由 `Net_Task` 使用**。AT 是"请求—等待"模式，第二个使用者必然导致串扰。
3. **共享数据整块替换**。`wifi_info`、`weather_info`、`room_info` 与软件时钟都在临界区内
   整体赋值，消费者读不到半写状态。
4. **中断只做通知**，去抖、手势、显示等耗时逻辑一律下沉到任务。
5. **头文件的类型定义归它所属的模块**：AT 解析出的结构体在 `AT.h`，界面布局类型在 `Page.h`。
6. **任务之间只通过接口通信**：跨任务请求走各任务头文件里的函数（如
   `LightSensor_Task_RequestToggleNight()`），除 `Task_Common` 外不共享全局变量。

---

## 3. 任务与并发模型

### 3.1 任务一览

| 任务 | 优先级 | 栈 | 职责 | 循环节拍 |
| --- | --- | --- | --- | --- |
| `UI_Task` / `"ui"` | 3 | 4KB | `Task_Common_Init()` 建事件组、`Board_Init()`、开机流程、**LCD 唯一写者**、昼夜切换执行者 | 白天 500ms / 夜间 2000ms |
| `DHT22_Task` / `"dht22"` | 4 | 2KB | 单总线温湿度采集，成功后置 `EV_DHT22` | 5min |
| `Net_Task` / `"net"` | 2 | 4KB | **独占 USART1/AT**：联网、SNTP、天气；1s 心跳推进三个周期计数器 | 1s |
| `LightSensor_Task` / `"light_sensor"` | 2 | 2KB | 光照判定、2s 去抖、发起昼夜切换并等 ACK | 事件驱动 + 1s 兜底 |
| `Key_Task` / `"key"` | 2 | 4KB | PA0 手势状态机（单击/双击/三击/长按） | 事件驱动 |

**创建时机本身构成依赖关系**：`main()` 只创建 `ui`；`UI_Task` 里先建 `dht22` 与 `net`，
等 `EV_NET_READY`（≤30s）并显示结果页后，才创建 `light_sensor` 与 `key`——
这样**昼夜切换只可能发生在开机流程之后**。

**优先级不是随手定的**：`DHT22` 高于 `UI`，因为单总线对微秒级时序敏感，被刷屏抢占会直接
读到校验和错误；`net` 降到 2，因为一次 AT 事务最长 10s，优先级过高会把显示饿死。
`net` / `light_sensor` / `key` 同为 2 且 `configUSE_TIME_SLICING = 0`（同级不轮转），
因此这三者必须都能主动阻塞——当前分别阻塞在事件组、任务通知与 `vTaskDelay` 上。

### 3.2 事件位

统一定义在 `Task/Inc/Task_Common.h`，全部为 `1UL << n`：

| 位 | 名称 | 置位者 → 消费者 | 含义 |
| --- | --- | --- | --- |
| bit0 | `EV_WEATHER` | `Net_Task` → `UI_Task` | 天气已更新 |
| bit1 | `EV_DHT22` | `DHT22_Task` → `UI_Task` | 室内温湿度已更新 |
| bit2 | `EV_NET_READY` | `Net_Task` → `UI_Task` | 开机网络阶段结束（无论成败） |
| bit3 | `EV_WIFI` | `Net_Task` → `UI_Task` | WiFi 状态变化 |
| bit4 | `EV_LOWERPOWER` | `LightSensor_Task` → `UI_Task` | 请求进入夜间 |
| bit5 | `EV_LOWPOWER_ACK` | `UI_Task` → `LightSensor_Task` | 昼夜切换执行完毕 |
| bit6 | `EV_WAKEUP` | `LightSensor_Task` → `UI_Task` | 请求回到白天 |
| bit7 | `EV_NET_UPDATE_NOW` | `UI_Task` → `Net_Task` | 退出夜间后立即补网络更新 |
| bit8 | `EV_DHT22_UPDATE_NOW` | `UI_Task` → `DHT22_Task` | 退出夜间后立即补温湿度采集 |

> **bit7 与 bit8 必须分开**。`xEventGroupWaitBits` 是清除式等待，两个任务共用一个位时，
> 先被唤醒的会把位清掉，另一个永远收不到通知。

### 3.3 同步手段

| 机制 | 用途 |
| --- | --- |
| 事件组 `Task_Events()` | 上表 9 个事件位，任务间主要通信方式（`Task_Common.c` 持有句柄，不暴露变量） |
| 任务通知（计数语义） | ISR → 任务的光敏/按键边沿；按键的"切换昼夜"请求（`LightSensor_Task_RequestToggleNight()`） |
| 临界区 `taskENTER_CRITICAL()` | 软件时钟与三个共享结构体的整块赋值（全部在 `App.c` 内） |
| 互斥量 | 仅调试串口的整行原子输出（配合"行属主"记录） |

未使用队列、信号量、`vTaskSuspend` 做同步；`configUSE_TIMERS = 0`（按键手势因此借用
`ulTaskNotifyTake` 的超时能力）。

### 3.4 中断优先级与 FreeRTOS 约束

| 中断源 | 抢占优先级 | 可否调用 FreeRTOS API | ISR 内实际动作 |
| --- | --- | --- | --- |
| PendSV / SysTick | 15 | — | 内核上下文切换 |
| **TIM5**（1ms 时基） | **2** | ❌ **禁止** | 仅 `TIM5_ms++` |
| USART1（AT 接收） | 5 | ✅ | 逐字节入环形缓冲，未调用任何 API |
| USART3（定位接收） | 5 | ✅ | 同上 |
| EXTI0 / EXTI1 / ADC1 AWD | 5 | ✅ | `vTaskNotifyGiveFromISR` + `portYIELD_FROM_ISR` |
| USART2 / DMA1_Stream5,6 | — | — | **未配置 NVIC**，全靠轮询完成标志 |

`configMAX_SYSCALL_INTERRUPT_PRIORITY = 5 << 4`，只有优先级数值 **≥ 5** 的中断才允许调用
FreeRTOS API。TIM5 是 2，因此它的 ISR 里不得加锁、不得调用任何 RTOS 接口。

### 3.5 FreeRTOS 关键配置

| 配置 | 值 |
| --- | --- |
| `configTICK_RATE_HZ` | 1000（1ms 一个 tick） |
| `configMAX_PRIORITIES` | 5（可用 0~4） |
| `configUSE_TIME_SLICING` | 0（同优先级不轮转） |
| `configUSE_TICKLESS_IDLE` | 0（**未启用**） |
| `configTOTAL_HEAP_SIZE` | `1024 * 88` = 90112 B（heap_4，任务栈与图片缓冲都从这里分配） |
| `configCHECK_FOR_STACK_OVERFLOW` | 2 |
| `configUSE_TIMERS` / `configUSE_MUTEXES` / `configUSE_EVENT_GROUPS` | 0 / 1 / 1 |
| `configTASK_NOTIFICATION_ARRAY_ENTRIES` | 3 |
| `configSUPPORT_DYNAMIC_ALLOCATION` / `STATIC` | 1 / 0（只开动态分配） |

### 3.6 数据流

```
ESP32-C3 ──USART1──▶ AT.c ──▶ wifi_info ──▶ 顶部状态条 / 开机结果页
                       │
                       ├─▶ AT_SNTP_Get_Time ─▶ Clock_Sync ─▶ 软件时钟（TIM5 推算）
                       │                                      └─▶ 主界面 / OLED 时钟
                       ├─▶ AT_Get_IP ─▶ 公网 IP ─▶ 天气 URL 的 location=<IP>
                       └─▶ AT_Get_HTTP ─▶ Parse_Http_Response ─▶ weather_info ─▶ 天气卡片

DHT22 ──▶ DHT22_ReadData ──▶ room_info ──▶ 室内温湿度卡片

光敏 EXTI1 ──通知──▶ LightSensor_Task（2s 去抖）──▶ 事件组 ──▶ UI_Task 切屏
按键 EXTI0 ──通知──▶ Key_Task（手势识别）──▶ 单击请求 ──▶ LightSensor_Task（统一裁决）

夜间的三个时间点：
  进入夜间前  软件时钟 ──▶ DS1302_SetTime()（写后回读校验）
  夜间        DS1302_ReadTime() ──成功──▶ OLED 显示；失败则回退软件时钟
  退出夜间    UI_Task ──▶ EV_NET_UPDATE_NOW / EV_DHT22_UPDATE_NOW ──▶ 立即补更

资源（字库/图片，全部来自 W25Q64）：
  W25Q64 ──SPI1 21MHz──▶ Asset 层 ──┬─▶ 字模：区位码直接算偏移，O(1)
                                    └─▶ 图片：分块流式读取 + DMA1_Stream5 乒乓 ──SPI3──▶ ST7789
```

---

## 4. 主要机制

### 4.1 时钟

- **白天**：SNTP 校时得到 UTC，`Clock_Sync()` 用民用历算法换算成 Unix 时间戳，与当前
  `TIM5_Get_ms()` 一起保存在临界区内；之后靠 TIM5 自行推算，每 4h 再校一次。
- **夜间**：进入夜间前把软件时钟写入 DS1302（写后回读校验），夜间只读 DS1302，断网也能走时。
- **周期**：WiFi 状态检查 10min（失败重试 10s）· SNTP 4h（重试 5s）· 天气 1h（重试 60s）。
  掉线自动重连，重连成功后立即补一次 SNTP 与天气。

### 4.2 昼夜切换与低功耗

采用"**请求—执行—ACK**"握手，避免两个任务各自改状态：

1. `LightSensor_Task` 判定光照变化并去抖 2s 后置 `EV_LOWERPOWER` / `EV_WAKEUP`；
2. `UI_Task` 是唯一执行者：关彩屏与背光、只留 OLED、冻结网络与温湿度周期计数、
   让 ESP32-C3 进入 Modem-sleep；
3. 执行完置 `EV_LOWPOWER_ACK`，`LightSensor_Task` 收到后才允许下一次判定；
4. 退出夜间时先唤醒模组，再用 **bit7 / bit8 两个独立事件位**通知两个任务立即补更。

按键手势也走同一入口——`Key_Task` 只调用 `LightSensor_Task_RequestToggleNight()` 发出请求，
最终由 `LightSensor_Task` 统一裁决（单击/三击尚未分配）。

---

## 5. 资源存储：W25Q64 + littlefs

字模与图片不放在 MCU Flash 里，而是烧到 W25Q64 上由 littlefs 管理，MCU 内只保留描述表。

**字库（11 个，合计 420050 B）**：`/font/cn16.bin`（3755 个 16×16 汉字）、`cn22.bin`（22×22），
以及 `as12/16/16b/22/22b/32/32b/48/48b.bin` 九张 ASCII 表。

**图片（32 个，合计 438728 B）**：`boot.bin` / `main.bin` 两张全屏图（各 153604 B）、
`err.bin`、`wifi.bin` / `wifi_off.bin`、`loc.bin` / `noloc.bin`（定位图标，当前界面未使用）、
`temp.bin` / `humi.bin` / `thermo.bin`、22 张天气图标 `w_*.bin`。
图片文件带 4 字节宽高头，格式为 RGB565 小端。

两项合计 **858778 字节**（占 8MB 的 10.2%），开机自检项数 = 11 + 32 = **43 项**。

**字模定位是 O(1) 的**，不查表：ASCII 用 `(ch - 0x20) * ob * size`，汉字用区位码
`((区码-0xB0)*94 + (位码-0xA1)) * cn_bytes`。点阵为横向取模、行优先，每字节 bit0 对应最左像素。

---

## 6. 构建、烧录与运行

### 6.1 三种构建模式

`User/Inc/BuildConfig.h` 的宏决定 `main()` 走哪条分支：

| 宏组合 | 模式 | 行为 |
| --- | --- | --- |
| `RESOURCE_PROVISION = 1` | **资源烧录** | 只初始化 W25Q64 与串口 → `Provision_Run()`（内部死循环）。不启动 RTOS、不初始化 LCD |
| `USE_FREERTOS = 1`（默认） | **RTOS 运行** | `Board_Peripheral_Init()` → `App_Task_Init()` → `vTaskStartScheduler()`，正式固件 |
| `USE_FREERTOS = 0` | **裸机测试** | 由 `BM_TEST_MODULE` 选择测试项，每个测试自带死循环 |

裸机模式可选测试项：`OLED` / `LIGHT` / `DS1302` / `W25Q64` / `LFS` / `GNSS`。
建议先跑 `W25Q64` 再跑 `LFS`——底层驱动不通时文件系统必然失败，分开跑便于定位。

### 6.2 资源烧录（首次使用必做）

资源总量超过 512KB Flash，必须分三批编译烧录，每批只把该批数据编译进来：

| 批次 | 内容 | 字节数 |
| --- | --- | --- |
| 1 | 汉字库 16 + 22 | 367990 |
| 2 | 开机全屏图 + 主页全屏图 | 307200 |
| 3 | 9 张 ASCII 表 + 图标 | 183460 |

每批把 `PROVISION_BATCH` 改成对应值、`RESOURCE_PROVISION = 1` 后编译烧录。

### 6.3 需要自行配置的参数

| 参数 | 位置 | 说明 |
| --- | --- | --- |
| WiFi 凭据 | `User/Src/App.c` 顶部的 `ssid` / `password` / `mac` | 要连接的 2.4GHz WiFi；`mac = NULL` 表示不绑定 BSSID |
| 心知天气 key | `User/Src/App.c` 的 `WEATHER_URL_FMT` 与 `weather_url` 初值 | **两处都有，要一起改** |
| `AMAP_KEY` | `BSP/Src/AT.c` | 高德"Web服务"key（逆地理编码预留用） |
| `HSE_VALUE` | `Core/stm32f4xx.h` | 必须与实际晶振一致 |

---

## 7. 编码与维护约定

- **源码编码**：UTF-8（无 BOM），每个文件内部行尾统一。例外：`User/Src/main.c` 与
  `User/Src/Main_Page.c` 是 **GB2312**——界面中文要直接送进 GB2312 字库，改动时不能用
  只认 UTF-8 的编辑器保存，也不要"另存为 UTF-8"批量转换。
- **中文界面字符串**必须按 GB2312 写入，否则 LCD 会把一个汉字拆成两个字节查表，显示空白或错字。
- **注释用中文**，函数头用 `/** @brief ... */`，分节用 `/* ====== 标题 ====== */`。
- **新增源文件**要同时加入 `MDK/STM32F407.uvprojx` 的对应分组，并确认头文件目录已在包含路径中。
  任务层的文件统一放项目根的 `Task/`（`Inc/` 放头、`Src/` 放源，`..\Task\Inc` 已在包含路径中），
  新增任务按"每个任务一个 `.c` + 一个 `.h`"的方式组织，`.h` 里只放 `*_Task_Start()` 与必要的
  跨任务请求接口，任务函数本身保持 `static`；任务参数（优先级/栈/周期）加到 `Task_Config.h`。
- **文档与源码不一致时以源码为准**，并顺手修正文档。

---

## 8. 已知限制

| 类别 | 问题 |
| --- | --- |
| 正确性 | 光敏 AO 模式用了 `ADC_Channel_0`（应为 `ADC_Channel_10`）；切到 AO 模式前必须先改 |
| 正确性 | `Test_LED_GPIO_init()` 裸机分支只赋值了 `GPIO_Pin` / `GPIO_Mode`，其余字段是栈上垃圾值 |
| 正确性 | `Parse_Http_Response()` 不清零输出结构体，缺失字段会保留调用方的旧值 |
| 正确性 | `Asset` 的图片掩码用 `1UL << i`，恰好只够 32 张，再加一张会溢出串位 |
| 健壮性 | `W25Q64_RWByte()` 等 SPI 等待、`LCD.c` 的 DMA 完成等待都是**无超时忙等**，硬件异常会卡死 |
| 健壮性 | `TIM5_ms` 是 64 位 `volatile`，ISR 与任务间读写非原子（当前靠重读 + UIF 补偿规避） |
| 冗余 | `Image_Main_Page`（153600 B）已烧录、参与开机自检，但界面从未绘制它 |
| 冗余 | `DS1302_ReadReg()` 未被引用，是当前全量重编下唯一的编译告警 |
| 预留 | GPS + 高德逆地理编码 + 中文转码三套代码已写好但**应用层未接入**，链接器会把未引用段丢弃，因此**不占 Flash**。要接入时注意 `ATGM336H_Poll()` **必须至少每 300ms 调用一次**（模块每秒约 615 字节，512B 环形缓冲约 0.83s 就满） |

---

## 9. 参考资料

`Documents/` 下按器件分类存放了数据手册与工具（合计约 137MB）：

| 目录 | 内容 |
| --- | --- |
| `DS1302/` | DS1302 中/英文数据手册、模块原理图 |
| `ESP32-C3/` | AT 固件 v4.1.1.0、烧录工具与配置、AT 用户指南 |
| `STM32F4/` | STM32F407VET6 数据手册、参考手册、核心板原理图 |
| `W25Q64/` | W25Q64BV 数据手册（中/英） |
| `屏幕/` | ST7789V 规格书、SSD1306 OLED 手册与使用文档 |
| `温湿度计/` | DHT22 / AM2302 数据手册与规格书 |
| `中文字库/` | 汉字点阵源文件与 GB2312 一级字库表 |
| `Tools/` | `gen_gb2312_table.py`（生成转码表）、`verify_utf8_gb2312.py`（校验） |
