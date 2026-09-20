# STM32F407 桌面天气时钟

> 基于 **STM32F407VET6 + FreeRTOS** 的桌面天气时钟 / 室内环境监测终端。
> 白天用 2.4" SPI 彩屏显示时钟、日期、实况天气与室内温湿度；夜间自动切到 0.96" OLED 只显示时间，并冻结周期任务、让联网模组进入省电档位。
> 时间基准是 **SNTP 校时 + TIM5 计数的软件时钟**，夜间切换到 **DS1302 外部 RTC**，断网也能正常走时。

| 项目 | 内容 |
| --- | --- |
| 主控 | STM32F407VETx（Cortex-M4F @168MHz，512KB Flash，128KB SRAM + 64KB CCM） |
| 操作系统 | FreeRTOS（5 个应用任务，tick 1kHz，heap_4，88KB 堆） |
| 文件系统 | littlefs v2.11，挂载在 W25Q64（8MB SPI Flash）上 |
| 联网 | ESP32-C3-MINI-1，AT 固件 v4.1.1.0，USART1 @115200 |
| 显示 | ST7789V 240×320 彩屏（SPI3 + DMA）/ SSD1306 128×64 OLED（软件 I2C） |
| 工具链 | Keil MDK-ARM Plus 5.24.1 / ARMCC V5.06 update 5（AC5）/ C99 |
| 固件规模 | Code 52324 B · RO-data 3748 B · RW-data 412 B · ZI-data 126740 B（Flash ≈54.8KB，SRAM ≈124.2KB） |

> 本文档以**源码为唯一依据**重新梳理，与源码不一致时以源码为准。
> 涉及具体数值处均标注了来源文件与行号，便于改动后回溯核对。

---

## 目录

- [1. 项目概览](#1-项目概览)
- [2. 硬件平台](#2-硬件平台)
- [3. 任务与并发模型](#3-任务与并发模型)
- [4. 外设与驱动](#4-外设与驱动)
- [5. 关键流程](#5-关键流程)
- [6. 资源存储：W25Q64 + littlefs](#6-资源存储w25q64--littlefs)
- [7. 编译、烧录与构建模式](#7-编译烧录与构建模式)
- [8. 裸机模块测试](#8-裸机模块测试)
- [9. 需要自行配置的参数](#9-需要自行配置的参数)
- [10. 调试与排障](#10-调试与排障)
- [11. 设计决策与踩坑记录](#11-设计决策与踩坑记录)
- [12. 已知限制与待办](#12-已知限制与待办)
- [13. 编码与维护约定](#13-编码与维护约定)
- [14. 参考资料索引](#14-参考资料索引)

---

## 1. 项目概览

### 1.1 功能

**显示**

- 主界面（彩屏 240×320）：顶部 WiFi 图标 + SSID；`Font_48` 大号 `HH:MM`（冒号随秒闪烁）；`YYYY/MM/DD` + 英文星期；实况天气卡片（图标 + 中文描述 + 温度）；室内温湿度卡片。
- 开机流程：LED 自检 → 开机底图 → 联网（最多等 30s）→ 结果页（成功/失败两种样式，停留 2.5s）→ 主界面。
- 夜间界面（OLED 128×64）：`HH:MM` + `YYYY-MM-DD`，仅在时/分/日期变化时重绘。

**联网**

- ESP32-C3 以 Station 模式连入 2.4GHz WiFi，SNTP 校时（东八区），HTTP 拉取心知天气实况接口。
- 三个独立周期：WiFi 状态检查 60s（失败重试 10s）、SNTP 4h（失败重试 5s）、天气 1h（失败重试 60s）。
- 掉线自动重连，重连成功后立即补一次 SNTP 与天气。

**传感与交互**

- DHT22/AM2302 采集室内温湿度，每 10s 一次，读取失败时界面显示 `--`。
- 光敏电阻自动判定昼夜，2s 去抖后切换；中断为主、1s 轮询兜底。
- 单个轻触按键（PA0）识别**单击 / 双击 / 三击 / 长按**，目前单击用于手动切换昼夜。

**低功耗**

- 夜间关闭彩屏显示与背光、只保留 OLED；冻结网络与温湿度的周期计数器；ESP32-C3 进入 Wi-Fi Modem-sleep。
- 退出夜间时先唤醒模组，再通过**两个独立事件位**通知网络与传感器任务立即补更。
- 昼夜切换采用"请求—执行—ACK"握手，避免两个任务各自改状态。

### 1.2 关键指标

| 指标 | 数值 | 说明 |
| --- | --- | --- |
| 应用任务 | 5 个 | ui / net / dht22 / light_sensor / key |
| 事件位 | 9 个 | 见 3.2 |
| 中断优先级组 | `NVIC_PriorityGroup_4` | 4 位抢占、0 位子优先级 |
| 静态 RAM | 127152 B（124.2KB / 128KB） | 余量约 3.8KB |
| Flash | 56072 B（54.8KB / 512KB） | 余量充足 |
| 外置资源 | 858778 B（≈839KB） | 11 个字库 + 32 张图片，全部在 W25Q64 |
| MCU 内的点阵/像素数据 | **0 字节** | 资源迁移后 MCU 内只剩描述表 |

### 1.3 软件分层

```
应用层      User/Src   main.c / Board.c / App.c / app_task.c / Provision.c /
                       bare_test.c / stm32f4xx_it.c / Boot_Page.c / Main_Page.c
资源层      BSP/Src    Asset.c（littlefs 挂载、自检、O(1) 字模与图片读取）
板级驱动    BSP/Src    LCD / OLED / AT / I2C / DHT22 / External_RTC /
                       Light_Sensor / Key / Timer / Usart / W25Q64 / Profiling
操作系统    Third_Lib  FreeRTOS（portable/FreeRTOSConfig.h）
文件系统    Third_Lib  littlefs v2.11（lfs_port.c 对接 W25Q64，LFS_Operation 封装读写）
硬件抽象    Core / STM32F4xx_StdPeriph_Driver（CMSIS + 标准外设库）
```

### 1.4 目录结构

```
Project4/
├── .gitignore                       # 忽略构建输出与 Keil 中间文件
├── KeilClear.bat                    # 递归清理 Keil 中间文件
├── 项目简历-STM32F407桌面天气时钟.md  # 面向求职的项目材料
├── User/                            # 应用层（Inc 10 个头 / Src 9 个源）
│   ├── Inc/  main.h · Board.h · BuildConfig.h · App.h · app_task.h ·
│   │         Provision.h · bare_test.h · Page.h · stm32f4xx_conf.h · stm32f4xx_it.h
│   └── Src/  main.c（三种构建模式入口）· Board.c（板级初始化）·
│             App.c（联网/传感器服务 + 软件时钟）· app_task.c（5 个任务）·
│             Provision.c（资源烧录）· bare_test.c（裸机测试用例）·
│             stm32f4xx_it.c（中断桩）· Boot_Page.c · Main_Page.c（界面绘制）
├── BSP/                             # 板级驱动（Inc 与 Src 各 13 个文件）
│   ├── Inc/ · Src/
│   │   ├── Asset.c         资源层：挂载 littlefs、开机自检、O(1) 字模读取
│   │   ├── LCD.c           ST7789：SPI3 + DMA、字模渲染、图片乒乓双缓冲
│   │   ├── OLED.c          SSD1306：命令/数据、取字模、OLED_ShowClock
│   │   ├── AT.c            ESP32-C3 AT 指令、WiFi/SNTP/HTTP、JSON 解析
│   │   ├── I2C.c           通用软件 I2C
│   │   ├── DHT22.c         单总线时序、校验和、错误码
│   │   ├── External_RTC.c  DS1302（裸机测试项 + 夜间时间源）
│   │   ├── Light_Sensor.c  光敏 DO(EXTI1) / AO(ADC1+AWD) 两种模式
│   │   ├── Key.c           PA0 按键：EXTI0 双边沿上报
│   │   ├── Timer.c         TIM5 1ms 基准 + delay_us/delay_ms
│   │   ├── Usart.c         USART1 RX 环形缓冲 + USART2 printf(DMA)
│   │   ├── W25Q64.c        SPI Flash：读 / 页编程 / 扇区擦除 / 忙等待超时
│   │   └── Profiling.c     基于 DWT->CYCCNT 的微秒级计时
├── Resource/                        # 资源数据（Inc 2 个 / Src 12 个）
│   ├── Inc/  Font.h（字库描述与文件 ID）· Image.h（图片描述）
│   └── Src/  Font/  Font_12/16/22/32/48.c + Chinese_Font16/22.c（源码内备份）
│             Image/ Image.c · Image_Boot_Page.c · Image_Main_Page.c ·
│                    Image_Weather.c · ImageTable.c（图片清单，32 项）
├── Core/                            # CMSIS + 启动文件 + 系统时钟
│   ├── core_cm4.h / core_cmFunc.h / core_cmInstr.h / core_cmSimd.h
│   ├── stm32f4xx.h（HSE_VALUE 等器件配置）· system_stm32f4xx.c/.h（PLL 配置）
│   └── Startup/startup_stm32f40xx.s
├── STM32F4xx_StdPeriph_Driver/      # STM32F4 标准外设库
├── Third_Lib/
│   ├── FreeRTOS/                    # 内核 + include/ + portable/
│   │                                #   （FreeRTOSConfig.h · heap_4.c · port.c · portmacro.h）
│   └── LittleFS/                    # littlefs v2.11
│       ├── Inc/  lfs.h · lfs_config.h · lfs_util.h · lfs_port.h · LFS_Operation.h
│       └── Src/  lfs.c · lfs_util.c · lfs_port.c · LFS_Operation.c
├── MDK/                             # Keil MDK5 工程
│   ├── STM32F407.uvprojx / .uvoptx   # 工程与调试器配置（已跟踪）
│   ├── DebugConfig/ · .vscode/       # 调试器与编辑器配置
│   └── Output/                       # 构建产物（已被 .gitignore 忽略）
└── Documents/                       # 器件资料与本说明文档
```

> `Resource/Src` 下的字库与图片数据**仍完整保留**，只是被
> `#if (RESOURCE_DATA_IN_ROM == 1) && (PROVISION_BATCH == N)` 排除在正式固件之外（见 6.6）。
> `ImageTable.c` 只含指针表（32 × 4 = 128 字节），没有该守卫，正式固件照常编译。

## 2. 硬件平台

### 2.1 主控与时钟

| 项目 | 参数 |
| --- | --- |
| MCU | STM32F407VETx（Cortex-M4F，512KB Flash，128KB SRAM = 112KB SRAM1 + 16KB SRAM2，另 64KB CCM） |
| 系统时钟 | 168MHz（`HSE = 25MHz`，`PLL_M = 25 / PLL_N = 336 / PLL_P = 2`） |
| 总线时钟 | AHB 168MHz · APB1 42MHz · APB2 84MHz |
| 中断分组 | `NVIC_PriorityGroup_4`（4 位抢占、0 位子优先级） |

> ⚠️ **晶振与 `HSE_VALUE` 必须一致**：参与时钟树计算的是 `Core/stm32f4xx.h` 中的 `HSE_VALUE`
> （`STM32F40_41xxx` 分支当前为 `25000000`）。Keil 工程里的 `CLOCK(12000000)` 只影响调试器的
> Xtal 显示，**不参与任何计算**。若换成 8MHz 晶振的核心板，必须同步修改 `HSE_VALUE` 与
> `system_stm32f4xx.c` 的 `PLL_M`，否则串口波特率、TIM5 计时与 SPI 速率会整体偏移。

### 2.2 引脚分配总表

| 外设 | 信号 | 引脚 | 配置 | 定义位置 |
| --- | --- | --- | --- | --- |
| ST7789 彩屏 | SCLK | PC10 | AF6（SPI3） | `BSP/Inc/LCD.h:17` |
| | MISO | PC11 | AF6（SPI3，只写屏用不到） | `BSP/Inc/LCD.h:23` |
| | MOSI | PC12 | AF6（SPI3） | `BSP/Inc/LCD.h:20` |
| | CS | PE2 | 推挽输出，软件片选 | `BSP/Inc/LCD.h:26` |
| | RESET | PE3 | 推挽输出 | `BSP/Inc/LCD.h:29` |
| | DC | PE4 | 推挽输出，命令/数据选择 | `BSP/Inc/LCD.h:32` |
| | BLK | PE5 | 推挽输出，背光 | `BSP/Inc/LCD.h:35` |
| W25Q64 | CS | PA4 | 推挽输出，软件片选 | `BSP/Inc/W25Q64.h:14` |
| | CLK | PA5 | AF5（SPI1） | `BSP/Inc/W25Q64.h:15` |
| | MISO | PA6 | AF5（SPI1） | `BSP/Inc/W25Q64.h:16` |
| | MOSI | PA7 | AF5（SPI1） | `BSP/Inc/W25Q64.h:17` |
| ESP32-C3 | TX | PA9 | AF7（USART1） | `BSP/Inc/AT.h:12` |
| | RX | PA10 | AF7（USART1） | `BSP/Inc/AT.h:13` |
| 调试串口 | TX | PA2 | AF7（USART2） | `BSP/Src/Usart.c:241` |
| | RX | PA3 | AF7（USART2，当前未使用） | `BSP/Src/Usart.c:242` |
| SSD1306 OLED | SCL | PB6 | 开漏 + 内部上拉 | `BSP/Src/OLED.c:11,13` |
| | SDA | PB7 | 开漏 + 内部上拉 | `BSP/Src/OLED.c:12,14` |
| DHT22 | DATA | PE6 | 输出/输入动态切换，输入上拉 | `BSP/Inc/DHT22.h:10-11` |
| DS1302 | RST（CE） | PE7 | 推挽输出 | `BSP/Inc/External_RTC.h:21` |
| | IO | PE8 | 双向 | `BSP/Inc/External_RTC.h:22` |
| | CLK | PE9 | 推挽输出 | `BSP/Inc/External_RTC.h:23` |
| 光敏电阻 | DO | PC1 | 输入 + EXTI1 双沿 | `BSP/Inc/Light_Sensor.h:24-27` |
| | AO | PC0 | 模拟输入（`ADC123_IN10`） | `BSP/Inc/Light_Sensor.h:14-15` |
| 用户按键 | KEY | PA0 | 下拉输入 + EXTI0 双沿 | `BSP/Inc/Key.h:10-14` |
| 测试 LED | LED | PC5（RTOS）/ PB2（裸机） | 推挽输出，低电平点亮 | `User/Src/Board.c:39-49` |

### 2.3 外设配置

| 外设 | 时钟 | 关键参数 | 定义位置 |
| --- | --- | --- | --- |
| USART1 | APB2 84MHz | 115200-8N1，RXNE 中断 + 512 字节环形缓冲，NVIC 抢占 5 | `BSP/Src/AT.c:50` |
| USART2 | APB1 42MHz | 115200-8N1，TX 走 DMA1_Stream6 / Channel4，**无中断、轮询 TCIF** | `BSP/Src/Usart.c:255,124` |
| SPI1 | APB2 84MHz | 主机、8 位、`CPOL=0 / CPHA=0`（**SPI Mode 0**）、预分频 4 → **21MHz** | `BSP/Src/W25Q64.c:79-82` |
| SPI3 | APB1 42MHz | 主机、8/16 位可切、`CPOL=0 / CPHA=0`（**Mode 0**）、预分频 2 → **21MHz**，TX 走 DMA1_Stream5 / Channel0 | `BSP/Src/LCD.c:556-559` |
| TIM5 | APB1 42MHz ×2 = 84MHz | PSC = 83（1MHz 计数）、ARR = 999（1ms 更新中断），NVIC 抢占 2 | `BSP/Src/Timer.c:21-37` |
| ADC1 | APB2 84MHz | 仅用于光敏 AO 模式；当前 `AO_DO_SWITCH = 0`，该分支不参与编译 | `BSP/Inc/Light_Sensor.h:11` |
| EXTI0 / EXTI1 | SYSCFG | 双边沿触发，NVIC 抢占 5，ISR 内只做任务通知 | `BSP/Src/Key.c`、`Light_Sensor.c` |
| 软件 I2C | — | `delay_us(5)` 半周期，开漏 + 内部上拉 | `BSP/Src/I2C.c` |

**SPI1 与 SPI3 同为 21MHz** 不是巧合：这是图片乒乓双缓冲能够把 W25Q64 读取时间完全隐藏在
SPI3 发送之后的前提（见 6.4）。改动任一侧的分频前请先看那一节。

### 2.4 硬件注意事项

- **DS1302 挂在 GPIOE**：`DS1302_PORT = GPIOE`、`RST = Pin_7`、`IO = Pin_8`、`CLK = Pin_9`
  （`BSP/Inc/External_RTC.h:20-23`）。`BuildConfig.h:14` 与 `bare_test.c:144` 的引脚说明与之一致。
- **光敏的 GPIO 端口/引脚与 EXTI 端口源/引脚源必须成对修改**。只改 GPIO、不改
  `LIGHT_SENSOR_DO_EXTI_PORT_SOURCE` / `..._PIN_SOURCE`，EXTI 仍挂在旧端口上，电平变化
  **不会产生任何中断**，表现为"遮挡无反应、串口也无日志"。本项目已踩过一次
  （GPIO 改到 PC1、EXTI 仍挂 PA1）。`Light_Sensor.h` 顶部的引脚注释也仍是旧的 `PA0/PA1`。
- **PA0 既是用户按键也是 `WKUP1`**。当前用不到，但若将来做 Standby 唤醒，该引脚可以直接复用。
- **测试 LED 有两个引脚**：FreeRTOS 分支用 PC5，裸机分支用 PB2（`Board.c:39-49`）。
  注意 `Test()` 中的 `GPIO_ResetBits(GPIOB, GPIO_Pin_2)` 是**无条件执行**的，即使 FreeRTOS 分支
  也会拉低 PB2（见 12.3）。
- **光敏 DO 模式判定**：DO 引脚为**高**（光照未达电位器阈值）即视为"暗"；AO 模式则用
  `ADC1 + 模拟看门狗` 双阈值迟滞判定，阈值宏 `LIGHT_SENSOR_AO_DARK_TH = 3000`、
  `LIGHT_SENSOR_AO_LIGHT_TH = 2000`。

## 3. 任务与并发模型

### 3.1 任务一览

| 任务（函数 / 注册名） | 优先级 | 栈（word / 字节） | 职责 | 循环节拍 |
| --- | --- | --- | --- | --- |
| `UI_Task` / `"ui"` | 3 | 1024 / 4KB | 创建事件组、执行 `Board_Init()`、走完开机流程；**LCD 的唯一写者**；昼夜切换的执行者 | 白天 500ms，夜间 2000ms |
| `DHT22_Task` / `"dht22"` | 4 | 512 / 2KB | 单总线温湿度采集，成功后置 `EV_DHT22` | 10s 超时即采集周期 |
| `Net_Task` / `"net"` | 2 | 1024 / 4KB | **独占 USART1/AT**：联网、SNTP 校时、天气更新；1s 心跳推进三个周期计数器 | 1s 心跳 |
| `LightSensor_Task` / `"light_sensor"` | 2 | 512 / 2KB | 光照判定、2s 去抖、发起昼夜切换并等 ACK；统一裁决按键的手动切换请求 | 事件驱动 + 1s 轮询兜底 |
| `Key_Task` / `"key"` | 2 | 1024 / 4KB | PA0 手势状态机（单击/双击/三击/长按） | 事件驱动，无轮询、无软件定时器 |

**创建时机不是一次性的**，顺序本身构成依赖关系：

1. `main()` → `App_Task_Init()` 只创建 `ui` 一个任务，随后 `vTaskStartScheduler()`
   （`User/Src/app_task.c:676-679`）。
2. `UI_Task` 内：`xEventGroupCreate()` → `Board_Init()` → 显示开机底图 → 创建 `dht22` 与 `net`
   （`app_task.c:600-601`）。
3. 等待 `EV_NET_READY`（最长 30s）→ 显示结果页 → 停留 2.5s → 绘制主界面 → 创建
   `light_sensor` 与 `key`（`app_task.c:615-617`）。

把光敏与按键任务的创建推迟到主界面之后，是为了**保证昼夜切换只可能发生在开机流程完成之后**。

**优先级不是随手定的**：

- `DHT22_Task` 优先级**高于** `UI_Task`：单总线协议对微秒级时序敏感，被刷屏抢占会直接读到校验和错误。
- `Net_Task` 降一级放在 2：一次 AT 事务最长可达 10s，优先级过高会把显示饿死。
- `net` / `light_sensor` / `key` 三者同为优先级 2，且 `configUSE_TIME_SLICING = 0`
  （同优先级**不**按 tick 轮转），因此这三者必须都能主动阻塞——当前实现确实如此
  （分别阻塞在事件组、任务通知、`vTaskDelay` 上）。

### 3.2 事件位

统一定义在 `User/Inc/app_task.h:45-53`，全部为 `1UL << n`：

| 位 | 名称 | 置位者 | 消费者 | 含义 |
| --- | --- | --- | --- | --- |
| bit0 | `EV_WEATHER` | `Net_Task` | `UI_Task` | 天气数据已更新，刷新天气卡片 |
| bit1 | `EV_DHT22` | `DHT22_Task` | `UI_Task` | 室内温湿度已更新，刷新房间卡片 |
| bit2 | `EV_NET_READY` | `Net_Task` | `UI_Task` | 开机网络阶段结束（**无论成败**） |
| bit3 | `EV_WIFI` | `Net_Task` | `UI_Task` | WiFi 状态变化，刷新顶部状态条 |
| bit4 | `EV_LOWERPOWER` | `LightSensor_Task` | `UI_Task` | 请求进入夜间 |
| bit5 | `EV_LOWPOWER_ACK` | `UI_Task` | `LightSensor_Task` | 昼夜切换执行完毕的确认 |
| bit6 | `EV_WAKEUP` | `LightSensor_Task` | `UI_Task` | 请求回到白天 |
| bit7 | `EV_NET_UPDATE_NOW` | `UI_Task` | `Net_Task` | 退出夜间后立即补一次网络更新 |
| bit8 | `EV_DHT22_UPDATE_NOW` | `UI_Task` | `DHT22_Task` | 退出夜间后立即补一次温湿度采集 |

> ⚠️ **两个补更事件位必须分开**。`xEventGroupWaitBits` 是**清除式**等待：若两个任务共用一个位，
> 先被唤醒的任务会把位清掉，另一个就永远收不到通知，症状是"退出夜间后只有一半数据被刷新"。
> 本项目实际踩过这个坑（见 11.2）。

`UI_Task` 的主循环一次等待 `EV_WEATHER | EV_DHT22 | EV_WIFI | EV_LOWERPOWER | EV_WAKEUP`
（`app_task.c:624-626`）；`Net_Task` 与 `DHT22_Task` 各自等待属于自己的位，是**唯一的消费者**。

### 3.3 同步手段

| 机制 | 保护/传递的对象 | 位置 |
| --- | --- | --- |
| 事件组 `g_evt` | 上表 9 个事件位 | 创建于 `app_task.c:583` |
| 任务通知（计数语义） | 光敏边沿 → `LightSensor_Task`；按键边沿 → `Key_Task`；按键"切换昼夜"请求 → `LightSensor_Task` | ISR 内 `vTaskNotifyGiveFromISR`，任务内 `ulTaskNotifyTake` |
| 临界区 `taskENTER_CRITICAL()` | 软件时钟（`clock_synced` / `clock_epoch` / `clock_sync_ms`）、`wifi_info`、`weather_info`、`room_info` | 全部在 `User/Src/App.c` 内**整块赋值** |
| 互斥量 `xSemaphoreCreateMutex` | 仅用于调试串口的**整行原子输出**（配合"行属主"记录） | `BSP/Src/Usart.c:212,217,232` |
| —（未使用） | 未使用队列、二值/计数信号量、`vTaskSuspend` 做同步；`configUSE_COUNTING_SEMAPHORES = 0` | — |

### 3.4 三条并发约定

1. **LCD 只由 `UI_Task` 写**。其他任务只更新数据并置事件位，杜绝多任务同时操作 SPI3 造成撕裂。
2. **USART1（AT）只由 `Net_Task` 使用**。AT 响应是"请求—等待"模式，第二个使用者必然导致串扰。
3. **共享数据整块替换**。`wifi_info`、`weather_info`、`room_info` 与软件时钟的
   `epoch + sync_ms` 全部在临界区内整体赋值，`UI_Task` 永远读不到半写状态——
   对 64 位字段尤其必要。

### 3.5 FreeRTOS 配置

`Third_Lib/FreeRTOS/portable/FreeRTOSConfig.h`：

| 配置 | 值 | 说明 |
| --- | --- | --- |
| `configTICK_RATE_HZ` | 1000 | 1ms 一个 tick |
| `configMAX_PRIORITIES` | 5 | 可用优先级 0~4 |
| `configUSE_PREEMPTION` | 1 | 抢占式调度 |
| `configUSE_TIME_SLICING` | 0 | 同优先级不轮转（见 3.1） |
| `configUSE_TICKLESS_IDLE` | 0 | **未启用**，原因见 11.1 |
| `configTOTAL_HEAP_SIZE` | `1024 * 88` = 90112 B | heap_4；任务栈与图片缓冲都从这里分配 |
| `configCHECK_FOR_STACK_OVERFLOW` | 2 | 溢出进入 `vApplicationStackOverflowHook` |
| `configUSE_TIMERS` | 0 | 软件定时器关闭（按键手势因此借用 `ulTaskNotifyTake` 的超时能力） |
| `configUSE_MUTEXES` | 1 | 仅调试串口使用 |
| `configUSE_EVENT_GROUPS` | 1 | 任务间主要通信方式 |
| `configUSE_TASK_NOTIFICATIONS` | 1，`configTASK_NOTIFICATION_ARRAY_ENTRIES = 3` | — |
| `configUSE_16_BIT_TICKS` | 0 | 32 位 tick |
| `configMINIMAL_STACK_SIZE` | 128 word | 空闲任务栈 |
| `configUSE_IDLE_HOOK` / `TICK_HOOK` / `MALLOC_FAILED_HOOK` | 全 0 | 未启用钩子 |
| `configGENERATE_RUN_TIME_STATS` / `configUSE_TRACE_FACILITY` | 全 0 | 未启用运行时统计 |
| `configKERNEL_INTERRUPT_PRIORITY` | `15 << 4` | PendSV / SysTick |
| `configMAX_SYSCALL_INTERRUPT_PRIORITY` | `5 << 4` | 阈值见 3.6 |
| `configASSERT` | 失败调用 `vAssertCalled(__FILE__, __LINE__)` | 实现于 `User/Src/main.c:62-67` |

分配方式为**只开动态分配**（`configSUPPORT_DYNAMIC_ALLOCATION = 1`，
`configSUPPORT_STATIC_ALLOCATION = 0`）。

### 3.6 中断优先级与 FreeRTOS 约束

NVIC 分组为 `NVIC_PriorityGroup_4`，因此**所有 `SubPriority` 写入值都无效**（子优先级位数为 0）。

| 中断源 | 抢占优先级 | 可否调用 FreeRTOS API | ISR 内实际动作 |
| --- | --- | --- | --- |
| PendSV / SysTick | 15 | — | 内核上下文切换 |
| **TIM5**（1ms 时基） | **2** | ❌ **禁止** | 仅 `TIM5_ms++`（`BSP/Src/Timer.c:48-55`） |
| USART1（AT 接收） | 5 | ✅ 允许 | 逐字节入环形缓冲，**未调用任何 API** |
| EXTI0（按键 PA0） | 5 | ✅ 允许 | `vTaskNotifyGiveFromISR` + `portYIELD_FROM_ISR` |
| EXTI1（光敏 DO，PC1） | 5 | ✅ 允许 | 同上 |
| ADC1 AWD（光敏 AO） | 5 | ✅ 允许 | 同上（当前 `AO_DO_SWITCH = 0`，不参与编译） |
| USART2（调试口） | — | — | **未配置 NVIC、未使能中断**，printf 靠轮询 DMA TCIF |
| DMA1_Stream5 / Stream6 | — | — | **无中断**，全工程未调用 `DMA_ITConfig`，均轮询完成标志 |

规则：`configMAX_SYSCALL_INTERRUPT_PRIORITY = 5 << 4`，优先级数值 **≥ 5** 的中断才允许调用
FreeRTOS API。TIM5 的优先级是 2，数值小于 5，因此**它的 ISR 里不得加锁、不得调用任何
FreeRTOS API**——当前实现只做一次自增，符合约束。

去抖、手势识别、显示等耗时逻辑全部下沉到任务里，ISR 只负责"通知一次"。

### 3.7 数据流

```
ESP32-C3 ──USART1──▶ AT.c ──▶ wifi_info ──▶ 顶部状态条 / 开机结果页
                       │
                       ├─▶ AT_SNTP_Get_Time ─▶ Clock_Sync ─▶ 软件时钟（TIM5 推算）
                       │                                      └─▶ 主界面时钟 / OLED 时钟
                       └─▶ AT_Get_HTTP ─▶ Parse_Weather_Response ─▶ weather_info ─▶ 天气卡片

DHT22 ──▶ DHT22_ReadData ──▶ room_info ──▶ 室内温湿度卡片

光敏 EXTI1 ──vTaskNotifyGiveFromISR──▶ LightSensor_Task（2s 去抖）──▶ 事件组 ──▶ UI_Task 切屏
按键 EXTI0 ──vTaskNotifyGiveFromISR──▶ Key_Task（手势状态机）──▶ 单击请求 ──▶ LightSensor_Task
                                                                          （统一裁决昼夜）

夜间的三个时间点：
  进入夜间前  软件时钟 ──▶ DS1302_SetTime()（写后回读校验）
  夜间        DS1302_ReadTime() ──成功──▶ OLED 显示
                                └─失败──▶ 回退软件时钟
  退出夜间    UI_Task ──▶ EV_NET_UPDATE_NOW   ──▶ Net_Task 立即补 WiFi+SNTP+天气
                      └─▶ EV_DHT22_UPDATE_NOW ──▶ DHT22_Task 立即补采

资源（字库/图片，全部来自 W25Q64）：
  W25Q64 ──SPI1 21MHz──▶ Asset 层 ──┬─▶ 字模：区位码直接算偏移，O(1)
                                    └─▶ 图片：分块流式读取 + DMA1_Stream5 乒乓双缓冲 ──SPI3──▶ ST7789
```

## 4. 外设与驱动

本章按驱动逐项列出引脚、参数、对外接口与实现要点。所有行号对应 `BSP/` 下的源码。

### 4.1 USART1 — ESP32-C3 AT 模组

| 项 | 内容 |
| --- | --- |
| 引脚 | PA9 = TX，PA10 = RX，AF7 |
| 参数 | 115200-8N1，无校验、无流控；RXNE 中断使能，TX 轮询 |
| 中断 | `USART1_IRQHandler`（`BSP/Src/Usart.c:31-45`），NVIC 抢占 5 / 子 0 |
| 接收链 | 中断逐字节 → **512 字节环形缓冲**（`Usart.c:11`）→ AT 层任务级轮询搬入 `rx_buf[1024]`（`AT.c:5`） |
| 缓冲区满 | **丢弃新字节**，不覆盖旧数据（`Usart.c:36-42`） |
| 发送前清场 | `Usart1_RX_Flush()` 丢弃未取走的残留；由 `AT_Send_Command()` 在发送前调用，见 11.11 |
| 应答判定 | 按行累积到 `'\n'` 后与 4 条固定字符串做 **`strcmp` 完全匹配**：`"OK\r\n"` / `"ERROR\r\n"` / `"busy p...\r\n"` / `"ready\r\n"` |
| 等待策略 | 用 TIM5 计时；无数据且调度器已运行时 `vTaskDelay(1)` 让出 CPU（`AT.c:132-143`） |

**封装的指令与超时**

| 指令 | 用途 | 超时 |
| --- | --- | --- |
| `AT` | 启动探测：每 100ms 一次，最多 30 次 | ≈3s |
| `AT+RESTORE` | 恢复出厂设置。**不在开机必经路径上**，只在 AT 初始化失败时由 `AT_Factory_Reset()` 下发（见 11.11） | 2000ms |
| `AT+CWMODE=1` | 设为 Station 模式 | — |
| `AT+CWJAP="<ssid>","<pwd>"[,"<mac>"]` | 连接 AP（`mac` 为 `NULL` 时省略第三参数） | **10000ms** |
| `AT+CWSTATE?` | 读连接状态与 SSID（判定必需项） | 2000ms |
| `AT+CWJAP?` | 读 BSSID / channel / RSSI（可选） | 2000ms |
| `AT+SLEEP=<0..3>` | 模组省电档位 | 2000ms |
| `AT+CIPSNTPCFG=1,8` | 使能 SNTP，东八区 | 2000ms |
| `AT+CIPSNTPTIME?` | 读网络时间 | 2000ms |
| `AT+HTTPCLIENT=2,1,"<url>",,,2` | 取 JSON 响应体 | **10000ms** |

**对外接口**（`BSP/Inc/AT.h`）

| 函数 | 职责 |
| --- | --- |
| `AT_Init()` | GPIO / USART1 / NVIC 初始化 → `AT_Wait_Boot(3000)` 反复发 `AT` 直到拿到 `OK`。**不含 `AT+RESTORE`**（见 11.11） |
| `AT_Factory_Reset()` | `AT+RESTORE` 恢复出厂 → `AT_Wait_Ready(5000)` 等模组重启完成。只在 AT 初始化失败时按需调用 |
| `AT_Wait_Ready(timeout)` | 等待 `ready` 行；**忽略中途冒出的 `OK`/`ERROR`/`busy`**，一直等到 `ready` 或总超时（见 11.11） |
| `AT_Send_Command(cmd, timeout)` | 发指令（自动补 `\r\n`），**仅 `OK` 算成功**；`busy p...` 在总超时内重发（发送前先清 RX 环形缓冲） |
| `AT_Get_Response()` | 返回内部 `rx_buf` 指针 |
| `AT_WiFi_Init()` | `AT+CWMODE=1` |
| `AT_Connect_WiFi(ssid, password, mac)` | `AT+CWJAP` 连接 |
| `AT_Get_WiFi_Info(info)` | 解析连接状态、SSID、BSSID、信道、RSSI |
| `AT_Is_WiFi_Conected()` | 由 `CWSTATE` 的 `state == 2` 判定 |
| `AT_Set_Sleep(mode)` | 下发 `AT+SLEEP=<mode>` |
| `AT_SNTP_Init()` / `AT_SNTP_Get_Time(out)` | SNTP 使能与取时（把 `1970` 等无效值判为失败） |
| `AT_Get_HTTP(url)` | 返回 HTTP 响应体文本 |
| `Parse_Weather_Response(resp, info)` | 解析心知天气 JSON |

**要点与坑**

- ⚠️ 应答是**含 `\r\n` 的整行完全匹配**。若模组打开了回显（echo）或只回 `\n`，`OK` 不会被识别，
  所有指令都会超时——排查串口问题时优先确认这一点。
- `rx_buf` 是**多行累积**的，每条命令只在进入时把首字节置 `'\0'`；超过 1023 字节直接返回
  `AT_ACK_NONE`，没有"已截断"的显式标志。
- 解析统一用 `strstr` + `sscanf`，没有引入 JSON 库（见 5.4）。
- `USART1` 的 GPIO/波特率/NVIC 初始化在 `AT.c`，而 `USART1_IRQHandler` 在 `Usart.c`，
  两者分居不同文件，改动时注意别漏。

### 4.2 USART2 — 调试输出

| 项 | 内容 |
| --- | --- |
| 引脚 | PA2 = TX，PA3 = RX，AF7（RX 当前未使用） |
| 参数 | 115200-8N1；**未配置 NVIC、未使能中断** |
| 发送 | `printf` → `fputc` → **256 字节行缓冲**（`Usart.c:94`）→ 遇 `'\n'` 或缓冲满 → DMA1_Stream6 / Channel4 整行发出 |
| 完成判定 | 先等 `TCIF6`（字节搬进 DR），再等 USART 的 `TC`（最后一位移出）；等待期间可阻塞则 `vTaskDelay(1)` |
| 整行原子 | 互斥量 `dbg_mtx` + 行属主 `dbg_line_owner`，同一任务后续字符免检 |
| 上下文退化 | 处于中断中或调度器未运行时，退化为**逐字节轮询** |

> ⚠️ **`printf` 必须带 `'\n'`**。整行锁在遇到换行并发送完毕后才释放；若某次 `printf` 不含换行，
> 锁会被该任务一直持有（同任务可重入），其他任务的 `printf` 将阻塞到 `portMAX_DELAY`。
> 启动期日志与 HardFault 现场能打印出来，正是依赖"退化路径"。

### 4.3 SPI3 — ST7789 彩屏

| 项 | 内容 |
| --- | --- |
| 引脚 | SCLK = PC10，MISO = PC11，MOSI = PC12（均 AF6）；CS = PE2，RESET = PE3，DC = PE4，BLK = PE5 |
| 参数 | 主机、8/16 位可切、Mode 0、MSB First、预分频 2 → 21MHz |
| 像素搬运 | DMA1_Stream5 / Channel0，HalfWord，Normal 模式，FIFO 关闭，**无中断**、轮询 `TCIF5` |
| DMA 分块 | `NDTR` 单次上限 65535 半字，超出自动分块（`LCD.c:196-231`） |
| 数据宽度切换 | 命令阶段 8 位、像素阶段 16 位；切换前先等 `BSY` 并在 `SPE=0` 下调用 `SPI_DataSizeConfig` |

**缓冲分配**

| 缓冲 | 大小 | 用途 |
| --- | --- | --- |
| `s_scratch` | 240 × 48 × 2 = **23040 B** | 整行文字与图标共用的渲染缓冲（48 行对齐 `Font_48`） |
| `s_picBuf[2]` | 2 × **7680 B** | 图片乒乓双缓冲（240 × 16 行 × 2），从 FreeRTOS 堆分配 |
| `s_glyph` | **144 B** | 单个字模最大字节数（48 号 ASCII = 3 字节/行 × 48 行） |

**初始化顺序**（`LCD.c:494-525`）

复位（低 20ms → 高 120ms）→ `0x11` 退出睡眠 + 120ms → `0x36=0x00`（扫描方向）、
`0x3A=0x55`（16bpp）→ 一批电源/伽马寄存器 → **GRAM 刷黑** → `0x29` 开显示 → 点亮背光。

> ⚠️ ST7789 复位**不清 GRAM**。若先开显示或先点背光，会先亮出复位前残留在屏上的旧画面
> （例如上次烧录的裸机测试内容）。刷黑必须排在开显示与背光**之前**。

**字符串渲染流程**（`ST7789_Write_String`）

1. 扫描整行，把最多 **40 个字符**的位置、宽度与资源偏移收集到 `GRef_t g[40]`——此阶段不碰 Flash；
2. 逐字符读字模，铺进 `s_scratch`；
3. 整行一次 `SetWindow` + DMA 写出，避免每个字符都开关窗口。

汉字用 GB2312 区位码 O(1) 定位，ASCII 用 `(ch - 0x20) * ob * size` 定位（见 6.3）。

**图片绘制**：`ST7789_Draw_Picture()` 分块流式读取——一块由 DMA 发送时，CPU 同时从 SPI1 读下一块
（见 6.4）。`ST7789_Draw_Picture_AutoTransparent()` 额外把接近白色的像素（各分量归一化差值 < 90）
原地替换为目标背景色。

**对外接口**：`ST7789_Init` / `ST7789_Display_Power(on)`（`on`：`0x29` + 背光亮；`off`：背光灭 + `0x28`，
显存保留）/ `ST7789_Fill_Color` / `ST7789_Write_String` / `ST7789_Draw_Picture` /
`ST7789_Draw_Picture_AutoTransparent`。

> ⚠️ `Is_GB2312()`（`LCD.c:592-595`）用 `char` 与 `0xA1~0xF7` 比较，**依赖 ARMCC 默认 `char` 无符号**。
> 换用默认有符号 `char` 的编译器（如 arm-none-eabi-gcc）会恒返回 false，汉字全部无法定位。

### 4.4 SPI1 — W25Q64

| 项 | 内容 |
| --- | --- |
| 引脚 | CLK = PA5，MISO = PA6，MOSI = PA7（均 AF5）；CS = PA4 |
| 参数 | 主机、8 位、Mode 0、MSB First、预分频 4 → 21MHz |
| 访问方式 | **全程 CPU 轮询，无 DMA**（`W25Q64.c:88` 的 SPI1 TX DMA 使能语句被注释） |
| 忙等待超时 | `W25Q64_BUSY_TIMEOUT = 0x08000000`（约 1.34 亿次轮询），超时计数由 `W25Q64_GetBusyTimeoutCount()` 暴露 |
| 页编程 | 用 `addr & (PAGE_SIZE-1)` 取页内偏移，**自动跨页分块**；超时只置 `ok=false`，仍继续写完剩余数据 |
| 擦除对齐 | 扇区擦除向下对齐 4KB，32KB 块擦除向下对齐 32KB |

**指令使用情况**（`W25Q64.c` 实际引用）

| 指令 | 码 | 用途 |
| --- | --- | --- |
| Write Enable | `0x06` | 每次编程/擦除前 |
| Read Status 1 | `0x05` | 轮询 BUSY |
| Read Data | `0x03` | 读数据 |
| Page Program | `0x02` | 页编程（≤256 字节/次） |
| Sector Erase | `0x20` | 4KB 扇区擦除 |
| Block Erase 32KB | `0x52` | 32KB 块擦除 |
| Chip Erase | `0xC7` | 整片擦除 |
| JEDEC ID | `0x9F` | 期望 `0xEF4017` |
| Read ID | `0x90` | 厂商 + 器件 ID |
| Release Power-down | `0xAB` | 上电后释放掉电模式 |

`W25Q64_WaitBusy` 每次读状态寄存器都**重新拉低再拉高 CS**——CS 不释放会导致后续命令静默失效。

**对外接口**：`W25Q64_Init` / `W25Q64_ReadId` / `W25Q64_ReadJedecId` / `W25Q64_IsBusy` /
`W25Q64_GetBusyTimeoutCount` / `W25Q64_Read` / `W25Q64_PageProgram` / `W25Q64_SectorErase` /
`W25Q64_Block32Erase` / `W25Q64_ChipErase`。

> ⚠️ `W25Q64.c` 顶部注释称芯片"支持模式 1 和模式 3（模式1: CPOL=0, CPHA=1）"，**注释有误**：
> 实际配置是 Mode 0，而 W25Q64 只支持 Mode 0 与 Mode 3。**代码是对的，注释是错的。**

### 4.5 软件 I2C — SSD1306 OLED

| 项 | 内容 |
| --- | --- |
| 引脚 | SCL = PB6，SDA = PB7，均**开漏输出 + 内部上拉** |
| 时序 | 纯 GPIO 位操作，每个电平 `delay_us(5)` → SCL 周期约 10µs（≈100kHz） |
| 从地址 | `0x3C`（7 位，发送前左移 1 位）；命令控制字节 `0x00`，数据控制字节 `0x40` |
| 尺寸 | 128 × 64 |
| 清屏 | **按页整块写**：8 次页循环，每页一次 I2C 事务写 128 字节 |
| 缓冲区 | `s_oled_zeros[128]`（整页清屏数据）、`bitmap[48][24]`（临时位图） |

**写字符流程**（`OLED_WriteChar`）

1. 按"原始尺寸"从字库读整个字模到 `s_glyph`；
2. 展开成 `bitmap[48][24]` 的 0/1 位图；
3. 裁剪到屏内，按页装配——逐列把该页的 8 行拼成一个字节写入。

**上电状态管理**：SSD1306 只要不断电就一直保持 GDDRAM 内容与显示开关状态，所以裸机测试后
切回 FreeRTOS 固件时，屏上会一直挂着上一次的测试画面。因此：

- `Board_Init()` 上电即执行 `OLED_Init()` + `OLED_Display_Off()`（白天由彩屏显示）；
- `OLED_Init()` 内部是**先清屏、最后才 `0xAF` 开显示**，否则从开显示到清屏写完这段时间旧内容可见；
- 首次进入夜间时还会再执行一次 `OLED_Init()` + `OLED_Clear()`。

**对外接口**：`OLED_Init` / `OLED_Clear` / `OLED_WriteChar` / `OLED_Write_String` /
`OLED_Display_On` / `OLED_Display_Off` / `OLED_ShowClock(hh, mm, year, mon, day)`；
全局实例 `oled_i2c` 定义于 `OLED.c:10-15`。

> ⚠️ **OLED 只能显示可见 ASCII**：`OLED_WriteChar` 把索引限制在 `< 95`，不取汉字字库。
> 而 `OLED_Write_String` 却按 `font->size / 2` 步进，传入含中文的字符串会整体错位——
> 夜间时钟只画数字与日期，正是为了避开这一点。

### 4.6 DHT22 — 室内温湿度

| 项 | 内容 |
| --- | --- |
| 引脚 | DATA = PE6；初始为推挽输出高，读取时切为输入上拉 |
| 起始时序 | 拉低 ≥1ms（`vTaskDelay(pdMS_TO_TICKS(2))`）→ 释放并切输入 → `delay_us(30)` 稳定 → 依次等待应答低电平、应答高电平、数据起始低电平（各 200µs 超时） |
| **位判别** | 记录每位的**上升沿时刻**，等该位高电平结束，用 `(int64_t)(TIM5_Get_us() - t_rise) > DHT22_BIT_THRESHOLD_US(40)` 判 1 |
| 位等待超时 | 300µs |
| 数据格式 | 5 字节：湿度高/低、温度高/低、校验和；校验和 = 前 4 字节之和的低字节；温度 bit15=1 表示负值 |
| 失败重试 | 首次失败后间隔 50 tick 再试一次（`DHT22.c:188-189`） |

**为什么用"测高电平宽度"而不是"固定延时后采样"**：位 `0` 的高电平只有 26~28µs，位 `1` 为 70µs，
在"延时 40µs 后采样"的方案里位 0 的裕量只有 2~5µs，任何中断延迟抖动都会读错。改为测宽度后，
对十几微秒的抖动免疫，`DHT22_ERR_TIMEOUT` 只会在真正等不到电平翻转时出现。
代价是判别依赖 `TIM5_Get_us()` 的单调性——见 4.10 与本项目实际踩过的坑（11.3）。

**错误码**（`BSP/Inc/DHT22.h`）

| 码 | 宏 | 含义 |
| --- | --- | --- |
| 0 | `DHT22_OK` | 成功 |
| 1 | `DHT22_ERR_NO_ACK` | 总线未拉低，传感器无应答 |
| 2 | `DHT22_ERR_NO_HIGH` | 应答后未拉高 |
| 3 | `DHT22_ERR_NO_LOW` | 数据起始未拉低 |
| 4 | `DHT22_ERR_TIMEOUT` | 位读取超时 |
| 5 | `DHT22_ERR_CHECKSUM` | 校验和错误 |

> ⚠️ `DHT22_Init()` 恒返回 `true`，**并不检测传感器是否存在**；且内部使用 `vTaskDelay`，
> 因此**调度器未启动时不可用**（裸机测试项中也没有 DHT22）。

### 4.7 DS1302 — 外部 RTC

| 项 | 内容 |
| --- | --- |
| 引脚 | RST(CE) = PE7，IO = PE8（写开漏 / 读输入），CLK = PE9 |
| 时序 | 纯 GPIO 位操作，每个 SCLK 半周期与 CE 建立/保持各 `delay_us(10)`（≈50kHz） |
| 位序 | **LSB 先出**。写入在 SCLK 上升沿被从机采样；读取由从机在**下降沿**换位，主机在**上升沿之前**（SCLK 低电平期间）采样，见 11.10 |
| 访问方式 | **不使用单寄存器读**：读用突发读命令 `0xBF` 连读 8 字节，写用突发写 `0xBE` 连写 8 字节 |
| 字节顺序 | 秒 / 分 / 时 / 日 / 月 / 周 / 年 / 写保护 |
| 校验 | **连读两次比对**：比较分/时/日/月/周/年（**不含秒**），不一致即判本次读取失败 |
| 单次读校验 | CH（Clock Halt）位判断 + 全字段 BCD 范围校验；`week < 1` 直接判失败 |
| 星期语义 | **1~7**（1 = 周一），与 DS1302 寄存器一致 |

**两个刻意的设计**

- `DS1302_Init()` **只把写保护寄存器置 `0x80`，不清零秒寄存器**。旧写法每次上电写 `0x80` 清零秒，
  会把时间重置为 `00:00:00`，导致"断电走时"根本无法验证。启动振荡器改由 `DS1302_SetTime()`
  写入的秒值（CH=0）负责。
- `DS1302_SetTime()` **恒返回 `true`**（没有写校验），因此调用方必须自己回读比对——
  见 5.5 中"用当日分钟总数比较"的做法。

> ⚠️ **`DS1302_ReadReg()`（`External_RTC.c:151-169`）定义后从未被调用**。它正是全量重编时
> `warning #177-D: function "DS1302_ReadReg" was declared but never referenced` 的来源。
> 增量编译若没有重编该文件，这条告警不会出现——**看不到告警不代表它不存在**。

### 4.8 光敏电阻

两种模式由 `BSP/Inc/Light_Sensor.h:11` 的 `AO_DO_SWITCH` **编译期**二选一，
当前值为 **`0`（DO 模式）**，AO 分支完全不参与编译。

| 模式 | 引脚 | 判定方式 |
| --- | --- | --- |
| DO（当前） | PC1 → EXTI1 双沿 | 引脚为**高**（光照未达电位器阈值）即"暗"；中断内读引脚更新锁存状态 |
| AO | PC0（模拟输入） | ADC1 连续转换 + 模拟看门狗，越界触发 AWD 中断 |

**AO 模式的迟滞设计**：进"暗"用 `LIGHT_SENSOR_AO_DARK_TH = 3000`，回"亮"用
`LIGHT_SENSOR_AO_LIGHT_TH = 2000`；触发后 `Light_Sensor_AWD_Arm()` 会**按反方向重装阈值窗口**，
避免在阈值附近反复触发造成中断风暴。极性由 `LIGHT_SENSOR_AO_DARK_HIGH = 1` 决定。

`Light_Sensor_Init()` 会先采样一次引脚/ADC 决定初始状态，因此上电时不会误触发一次切换。

> ⚠️ **AO 模式的 ADC 通道号与实际引脚不符**：`Light_Sensor.c:83,87` 配置的是 `ADC_Channel_0`
> （STM32F4 上对应 PA0），而光敏 AO 实际接在 **PC0 = ADC123_IN10**（`Light_Sensor.h:9,14-15`
> 也写着 IN10）。当前 `AO_DO_SWITCH = 0` 使该分支不编译，所以现象没暴露——
> **切到 AO 模式前必须先改成 `ADC_Channel_10`。**

> ⚠️ `Light_Sensor_Read()` 在 DO 模式下**恒返回 0**（它只在 AO 分支返回真实 ADC 值），
> 调用方不要把它当作"读原始光强"使用。

### 4.9 按键

| 项 | 内容 |
| --- | --- |
| 引脚 | PA0，**下拉输入** → 空闲低、按下高 |
| 中断 | EXTI0，**双边沿**触发（按下与松开都进中断），SYSCFG 端口源 GPIOA / 引脚源 0，NVIC 抢占 5 |
| 分层 | `BSP/Src/Key.c` 只做 GPIO/EXTI 初始化与"上报边沿"；手势识别全部在 `app_task.c` 的 `Key_Task` |
| ISR 内动作 | 清 EXTI 挂起标志 → 调用注册的回调 → 回调内 `vTaskNotifyGiveFromISR` + `portYIELD_FROM_ISR` |
| 消抖 | 按键**自带硬件消抖**，软件层不再做按键去抖 |

> ⚠️ **中断服务函数名必须与宏"大小写完全一致"**（本项目实际踩过的坑）：
> `Key.h` 定义 `#define KEY_EXTI_IRQHandler EXTI0_IRQHandler`，所以 `Key.c` 里的函数必须写成
> `void KEY_EXTI_IRQHandler(void)`（全大写 `KEY_`）。若写成混合大小写的 `Key_EXTI_IRQHandler`，
> C 预处理器**不会做替换**（宏替换区分大小写），于是它只是一个**没人调用的普通函数**，
> 而 `EXTI0_IRQHandler` 仍指向启动文件里的 `[WEAK]` 默认实现——那串共享的 `B .` 无限循环。
> 症状：**按一下按键整个系统立刻静止、串口无任何新输出，而编译 0 Error 0 新增警告**。
>
> 验证方法：`MDK/Output/STM32F407.map` 中应出现
> `startup_stm32f40xx.o(RESET) refers to key.o(i.EXTI0_IRQHandler)`。
> 若中断仍由启动文件提供，就说明宏没展开。

### 4.10 TIM5 — 系统时基与延时

| 项 | 内容 |
| --- | --- |
| 时钟 | APB1 定时器时钟 = 84MHz（APB1 预分频 ≥2 时定时器倍频 ×2） |
| 参数 | `PSC = 84e6/1e6 - 1 = 83` → 1MHz 计数；`ARR = 999` → 1ms 更新中断 |
| 中断 | `TIM5_IRQHandler`，NVIC 抢占 **2**（高于 `configMAX_SYSCALL_INTERRUPT_PRIORITY = 5`） |
| ISR 内容 | 仅 `TIM5_ms++`，**不得调用任何 FreeRTOS API** |
| 对外接口 | `TIM5_Get_ms()`、`TIM5_Get_us()`、`delay_us()`、`delay_ms()` |

**`TIM5_Get_us()` 的单调性保证**：采样 `CNT` 与 `TIM5_ms` 时做一致性检查——
`while (ms != TIM5_ms)` 检测采样期间是否被中断更新；当 `CNT` 很小而更新事件已发生
（`TIM5->SR` 的 `UIF` 置位）时**补回缺失的 1ms**。这样即使"计数器已回绕、但更新中断还没执行"，
返回值也**永不倒退**。

**`delay_us()` 用有符号差值比较**：

```c
uint64_t start = TIM5_Get_us();
while ((int64_t)(TIM5_Get_us() - start) < (int64_t)us) ;
```

若沿用无符号相减，一旦出现"时间倒流"就会**下溢成约 2⁶⁴ 的巨大值**，循环条件立刻不成立而
**提前返回**。这与 DS1302 的"数字乱跳"直接相关，详见 11.3。

`delay_us` / `delay_ms` 都是**忙等**，不让出 CPU。

### 4.11 Profiling — DWT 周期计数

`Prof_Init()` 使能 `CoreDebug->DEMCR.TRCENA`、清零 `DWT->CYCCNT` 并使能 `DWT->CTRL.CYCCNTENA`；
`Prof_Us(start)` 用无符号减法自动处理 32 位回绕（168MHz 下约 25.6s 回绕一次），再除以
`SystemCoreClock / 1000000` 得到微秒数。

不占用定时器、不产生中断，适合评估渲染耗时。当前有**两个测量点**，都位于 `Board_Init()`
的 `Prof_Init()` 之后；输出的日志格式见 10.3：

| 测量点 | 位置 | 覆盖范围 |
| --- | --- | --- |
| 全屏图片绘制 | `LCD.c` 的 `ST7789_DrawImage_Stream()` | 只对 `240×320` 整屏图片计时（目前只有开机底图），范围从设置窗口到 SPI3 `BSY` 落下；小图标不计时以免刷屏。日志同时报告走了乒乓双缓冲还是单缓冲回退 |
| 整页重绘 | `app_task.c` 的 `UI_Enter_Day()` 与首次进主页面 | `Main_Page_Display()` 全量重绘：整屏清屏填充 + 时钟/日期/天气/室内四个模块 |

> 两个注意点：`DWT->CYCCNT` 在 CPU 进入 sleep（idle 的 `__WFI`）时**会停止**，所以不要用它
> 测量跨越等待区间的时间（例如等网络响应）；`Prof_Us()` 内部做的是
> `SystemCoreClock / 1000000` 整数除法，改主频时要求它是 1MHz 的整数倍。

## 5. 关键流程

### 5.1 上电与开机流程

```
main()
 └─ NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4)
 └─ Board_Peripheral_Init()     使能 GPIOA/B/C/E、USART1/2、SPI1/3、DMA1、TIM5、ADC1、SYSCFG
 └─ App_Task_Init()             只创建 ui 任务
 └─ vTaskStartScheduler()

UI_Task
 ├─ xEventGroupCreate()
 ├─ Board_Init()                Test() → TIM5_Init() → Usart2_Debug_Init() → Prof_Init()
 │                              → ST7789_Init() → Asset_Init() → OLED_Init() → OLED_Display_Off()
 ├─ Asset_Ready() 检查（失败仅告警，不阻塞）
 ├─ Boot_Page_Wait()            显示开机底图
 ├─ 创建 dht22 / net 任务
 ├─ xEventGroupWaitBits(EV_NET_READY, 30s)   ← 最多等 30s 兜底
 ├─ Boot_Page_Show()            结果页：成功 / WiFi 失败 / 服务失败 三种样式
 ├─ vTaskDelay(2500)            结果页停留 2.5s
 ├─ Main_Page_Display()         绘制主界面
 ├─ 创建 light_sensor / key 任务
 └─ 事件循环
```

`Board_Init()` 的调用顺序有两个刻意安排：

- `TIM5_Init()` 与 `Usart2_Debug_Init()` 排在前面，**后面所有初始化日志才看得见**；
- 上电即 `OLED_Init()` + `OLED_Display_Off()`，把 OLED 拉到"GDDRAM 全 0、显示关"的已知状态
  （原因见 4.5）。

`Net_Task` 在开机阶段依次执行：`AT_Init()` → `AT_WiFi_Init()` → `Service_WiFi_Connect()` →
`AT_SNTP_Init()` → `Service_Time_Sync()` → `Service_Weather_Update()`，无论成败最后都置
`EV_NET_READY`，并把结果写进 `NetBoot_t{wifi_ok, service_ok}` 供结果页读取。

### 5.2 联网时序与自恢复

```
AT_Init()
    GPIO / USART1 / NVIC(抢占5)
    AT_Wait_Boot(3000)      反复发 "AT"，每 100ms 一次，最多 30 次
                            （开机不再下发 AT+RESTORE，见 11.11）

Wireless_Init()             AT+CWMODE=1；整体最多尝试 3 次，间隔 200ms
                            失败返回 false，不再"失败也报 OK"（见 11.11）
                            第 1 次失败后追加一次 AT_Factory_Reset()
                            （AT+RESTORE + 等 ready），仅此一次

Service_WiFi_Connect()
    AT+CWJAP="<ssid>","<pwd>"          超时 10000ms
    AT+CWSTATE?                        必需项，失败最多重试 3 次（间隔 200ms）
    AT+CWJAP?                          BSSID / channel / RSSI，失败不影响整体

Service_WiFi_Update()                  周期 60s，失败后缩短为 10s
    查询 AT+CWSTATE? 最多 2 次（间隔 200ms）
      查询失败           → 返回 -1（只报错，不重连）
      查询成功且已连接   → 刷新 wifi_info，返回 0
      查询成功但未连接   → AT+CWJAP 重连，再最多查 3 次确认
                            成功 → 返回 1（调用方立即补 SNTP + 天气）
                            失败 → 把 wifi_info 写为离线，返回 -1

Service_WiFi_Sleep(enable)             AT+SLEEP=1（Modem-sleep）/ AT+SLEEP=0（全速）

Service_Time_Sync()                    AT+CIPSNTPTIME? 轮询最多 15 次，每次间隔 1s
Service_Weather_Update()               AT+HTTPCLIENT=2,1,"<url>",,,2 → Parse_Weather_Response()
```

**核心设计：区分「查询失败」与「掉线」**

`Service_WiFi_Update()` 只有在 **AT 查询成功、且模组确实报告未连接** 时才发 `AT+CWJAP`。
纯粹拿不到 AT 回复（模组忙、上一条 HTTP 响应残留）只会重试一次查询后返回 `-1`，
**不会触发一次无谓的 10s 阻塞重连**，也不会把 SNTP 与天气一起停掉。

失败路径同样会写回 `wifi_info`（`connected = 0`），使顶部状态条与 `Net_Task` 的 `wifi_up`
保持同步。旧实现在失败时不写回，导致 `wifi_info.connected` 一旦为 1 就再也不会回落，
顶栏会一直显示"已连接"。

**三层自恢复**

| 层级 | 失败检测 | 重试周期 | 成功后 |
| --- | --- | --- | --- |
| WiFi | `AT+CWSTATE?` | `RETRY_WIFI_S = 10s` | 立即补一次 SNTP 与天气 |
| SNTP | 轮询 15 次仍未取到有效时间 | `RETRY_SNTP_S = 5s` | 回到 `SNTP_PERIOD_S = 4h` |
| 天气 | HTTP 或解析失败 | `RETRY_WEATHER_S = 60s` | 回到 `WEATHER_PERIOD_S = 1h` |

`AT_SNTP_Get_Time()` 会把 `1970` 年之类的无效时间判为失败，等待下一轮重试。

### 5.3 软件时钟

| 函数 | 职责 |
| --- | --- |
| `Clock_Sync(&date_info)` | 校验 `year >= 2000`、`1 ≤ month ≤ 12` 后，用民用历算法把日期换算为 Unix 时间戳，与当前 `TIM5_Get_ms()` 一起在**临界区内**保存 |
| `Clock_IsSynced()` | 临界区内读 `clock_synced` |
| `Clock_GetDateTime(&out)` | 临界区内一次性快照 `synced / epoch / sync_ms / now_ms`，再算 `epoch + elapsed_ms/1000`，最后反算年月日与星期 |

日期换算采用 **Howard Hinnant 的民用历算法**（`Clock_DaysFromCivil` / `Clock_CivilFromDays`），
不依赖标准库的 `time.h`。星期公式为 `((days + 3) % 7 + 7) % 7 + 1`，结果 **1 = 周一**。

未同步时 `Clock_GetDateTime()` 把结构体清零，界面据此绘制 `--:--` 与空日期——
因此**上电后到首次 SNTP 成功之间不会显示错误时间**。

### 5.4 天气 JSON 解析

`Parse_Weather_Response()`（`BSP/Src/AT.c:525-577`）不引入 JSON 库，直接用 `strstr` 定位字段：

| JSON 字段 | 目标成员 | 用途 |
| --- | --- | --- |
| `results.location.name` | `city[32]` | 解析保留，**界面当前不显示** |
| `results.location.path` | `location[128]` | 备用 |
| `results.now.text` | `weather[16]` | 英文描述（界面改用本地中文映射表） |
| `results.now.code` | `weather_code` | 查 `weather_map[]`（**26 项**）得图标 + 中文 |
| `results.now.temperature` | `temperature` | 天气卡片温度 |

`weather_map[]` 定义在 `User/Src/Main_Page.c`，把 26 个天气码映射到 **22 张图标**，
未知码回落为晴天。映射表本身占 312 字节（`main_page.o(.constdata)`）。

> ⚠️ `Parse_Weather_Response()` **不清零** `info`（对比 `AT_Get_WiFi_Info()` 里有 `memset`）。
> 若响应中缺少某个字段，该字段会保留调用方的旧值而不是变成空——排查"天气显示陈旧值"时注意。

> 定位显示已移除：`Main_Page_Net_Update()` 只绘制 WiFi 图标与 SSID，`weather_info.city`
> 仍在解析但不再参与绘制；`Image_location` / `Image_no_location` 两张图标也不再被引用
> （资源文件仍留在 W25Q64 上，仍参与开机自检）。

### 5.5 昼夜切换与低功耗握手

**状态变量**（全部在 `User/Src/app_task.c`）

| 变量 | 位置 | 语义 |
| --- | --- | --- |
| `s_night` | 文件作用域 `static bool` | 当前昼夜模式，初值 `false`（白天） |
| `s_baseline_dark` | 文件作用域 `static bool` | 自动判定基线 = 当前已生效的光照状态 |
| `s_lowpower` | `volatile bool` | 低功耗标志，由 `UI_Task` 在进/出夜间时改 |
| `s_key_toggle_night` | `volatile bool` | 按键请求标志 |
| `s_lcd_on` / `s_oled_on` | `static bool` | 两块屏的开关状态 |
| `wifi_sleeping` | `Net_Task` 局部 | 模组当前省电档位（仅本任务访问） |

**完整时序**

```
LightSensor_Task                              UI_Task              Net_Task / DHT22_Task
  中断通知 或 1s 轮询兜底
  与 s_baseline_dark 比较
  2s 去抖（新边沿重新计时 / 按键立即结束）
  复核电平 → 更新基线
  若 dark != s_night：
      置 EV_LOWERPOWER 或 EV_WAKEUP ───────▶
                                    UI_Enter_Night() / UI_Enter_Day()
                                    执行切屏动作
      等 EV_LOWPOWER_ACK（2s 超时）◀──────  置 EV_LOWPOWER_ACK

按键单击 ──▶ Key_Task 置 s_key_toggle_night + 通知 LightSensor_Task
             └─ Light_HandleKey()：翻转 s_night、把当前光照记为基线、走同一条提交路径
```

**`UI_Enter_Night()`**：若软件时钟已同步，先用 `RTC_Sync_From_SystemClock()` 回写 DS1302 并回读校验
→ 置 `s_lowpower = true` → 首次进入时 `OLED_Init()` + `OLED_Clear()` → `OLED_Display_On()`
→ 读时间（DS1302 优先，失败回退软件时钟）→ `OLED_ShowClock()` → `ST7789_Display_Power(false)`
（背光灭 + `0x28` 显示睡眠）→ 更新屏状态标志。

**`UI_Enter_Day()`**：`OLED_Display_Off()` → `ST7789_Display_Power(true)` → `Main_Page_Display()`
整屏重绘 → 更新屏状态标志 → `s_lowpower = false` → 置
`EV_NET_UPDATE_NOW | EV_DHT22_UPDATE_NOW`。

**夜间各任务的行为**

| 任务 | 夜间行为 |
| --- | --- |
| `UI_Task` | 不再绘制 LCD，改走 OLED 分支：每 2s 检查一次，**仅当时/分/日期变化才重绘** |
| `Net_Task` | 仍保持 1s 心跳；`s_lowpower` 变化时切 ESP32-C3 省电档位；**冻结 WiFi / SNTP / 天气三个计数器，不执行任何 update** |
| `DHT22_Task` | 每 10s 醒来后**跳过采集** |
| `LightSensor_Task` | **继续工作**（昼夜判定不能停） |
| `Key_Task` | **继续工作** |

**三条必须遵守的顺序约定**

1. **冻结计数器要放在递减之前**。`if (s_lowpower) continue;` 若写在 `--counter` 之后，
   夜间计数器会继续归零，退出夜间时三个计数器同时到期，同一秒内连发多次 AT 事务。
2. **补更走两个独立事件位**。`Net_Task` 等 `EV_NET_UPDATE_NOW`、`DHT22_Task` 等
   `EV_DHT22_UPDATE_NOW`，都是清除式等待，互不抢占（原因见 3.2）。
3. **先唤醒模组，再做补更**。`Net_Task` 在事件组返回后、`if (s_lowpower) continue;` **之前**
   检查档位是否变化，变化则先发 `AT+SLEEP=0`。否则补更的第一条 AT 命令会落在仍处于
   Modem-sleep 的模组上（不掉连接，只是响应要晚一个 DTIM 周期）。设置失败时保持原状态，
   下个周期自动重试。

**正在执行的 AT 事务不被打断**：进入夜间时若 `Net_Task` 正在收发 AT（最长 10s），
本轮会跑完、下一拍才进入跳过状态；`UI_Task` 不等待 `Net_Task`，仍立即回 ACK。
代价是暂停生效最多延迟约 10s。

### 5.6 按键手势

手势识别全部在 `Key_Task`（`configUSE_TIMERS = 0`，不使用软件定时器，直接借
`ulTaskNotifyTake` 的超时能力）：

```
clicks == 0 时：等按下（无限等待）
clicks >  0 时：等按下，窗口 KEY_MULTI_GAP_MS = 300ms
         │超时 ──▶ 结算累计击数
         │按下
         ▼
    等 KEY_LONG_PRESS_MS = 800ms
         ├─ 超时（仍按住）──▶ 长按事件（按住即触发）
         │                    ──▶ 以 KEY_RELEASE_POLL_MS = 20ms 步长等松开 ──▶ 清残留通知
         └─ 收到通知（已松开）──▶ clicks++ ──▶ 回到"等按下"
                                  达到 KEY_CLICK_MAX = 3 立即结算
```

| 手势 | 枚举 | 当前动作 |
| --- | --- | --- |
| 单击 | `KEY_GESTURE_CLICK_1` | 置 `s_key_toggle_night` 并唤醒 `LightSensor_Task` |
| 双击 | `KEY_GESTURE_CLICK_2` | `TODO`，留空 |
| 三击 | `KEY_GESTURE_CLICK_3` | `TODO`，留空 |
| 长按 | `KEY_GESTURE_LONG` | `TODO`，留空 |

**为什么交给光敏任务裁决**：昼夜状态机（`s_night` / `s_baseline_dark`）在 `LightSensor_Task`
手里，让 `Key_Task` 直接改状态会造成两个任务竞争同一份状态。因此按键只发一个"请求"标志，
由光敏任务统一提交。

**手动优先与自动跟随的关系**：`LightSensor_Task` 用 `s_baseline_dark` 记录"自动判定基线"
（= 当前已生效的光照状态）。手动切换后把**当前光照**记入基线，因此光敏**不会立刻把手动结果
改回去**；只有环境光真正发生反向变化（`IsDark() != s_baseline_dark`）时自动跟随才重新生效。

```
环境暗（基线=暗），手动切到白天 ──单击──▶ 白天，基线仍=暗
       环境一直暗：保持白天（手动优先）
       环境变亮  ：基线=亮，自动跟随（已是白天，无需切换）
       环境再变暗：基线=暗，自动切回夜晚
```

上电时基线取反（`s_baseline_dark = !Light_Sensor_IsDark()`），使"与基线相同则跳过"不成立，
从而**强制先跟随一次环境光**。

## 6. 资源存储：W25Q64 + littlefs

### 6.1 为什么把资源搬出 MCU

11 张字模表（GB2312 一级汉字 3755 字 × 16/22 点阵 + 9 张 ASCII 表）与 32 张图片合计约 **839KB**，
而 STM32F407VET6 只有 512KB Flash——编译进固件时 RO-data 达 323604 字节，总 ROM 355556 字节，
余量紧张且无法再扩展界面。

方案：在 W25Q64 上跑 littlefs，把全部点阵与像素数据以文件形式存放，MCU 内只保留
`Font_t` / `Image_t` 描述表，新增 `Asset` 资源访问层统一负责挂载、自检与读取。

| 指标 | 迁移前 | 迁移后 | 变化 |
| --- | --- | --- | --- |
| RO-data | 323604 | **3748** | −319856（−98.8%） |
| 总 ROM | 355556 | **56484** | −299072（**−84.1%**） |
| MCU 内的点阵/像素数据 | 全部 | **0 字节** | 只剩描述表与指针表 |

### 6.2 littlefs 移植参数

| 项 | 值 | 说明 |
| --- | --- | --- |
| 版本 | `LFS_VERSION = 0x0002000b`（v2.11） | 磁盘格式 `0x00020001` |
| `LFS_BLOCK_SIZE` | 4096 | 等于 W25Q64 扇区大小 |
| `LFS_BLOCK_COUNT` | 2048 | 8MB / 4KB，覆盖整片，**无预留分区** |
| `LFS_READ_SIZE` / `LFS_PROGRAM_SIZE` | 16 / 16 | — |
| `LFS_CACHE_SIZE` / `LFS_LOOKAHEAD_SIZE` | 16 / 16 | lookahead 须为 8 的倍数 |
| `LFS_BLOCK_CYCLES` | 500 | 磨损均衡参数 |
| 缓存区 | 3 × 16 字节静态数组 | `s_lfs_read_buffer` / `s_lfs_prog_buffer` / `s_lfs_lookahead_buffer` |

**回调对接**（`Third_Lib/LittleFS/Src/lfs_port.c`）

| 回调 | 实现 |
| --- | --- |
| `read` | `addr = block * 4096 + off` → `W25Q64_Read()`，**恒返回 `LFS_ERR_OK`** |
| `prog` | `W25Q64_PageProgram()`，返回 `false` → `LFS_ERR_IO`；底层按 256 字节页自动分段 |
| `erase` | `W25Q64_SectorErase(block * 4096)`，失败 → `LFS_ERR_IO` |
| `sync` | 空实现——底层读/写/擦除都已阻塞等到 BUSY |

> ⚠️ **工程里有两个挂载入口，策略相反**：
> `LittleFS_Init()`（`lfs_port.c:172-212`）在挂载失败时会**自动 `lfs_format`**；
> `Asset_Init()`（`Asset.c:127-139`）只挂载、**绝不自动格式化**，失败返回 `-2`。
> 正式固件走的是 `Asset_Init()`。**不要**在资源缺失时误用前者，它会清空整片资源。

### 6.3 W25Q64 上的文件布局

**字库（11 个，合计 420050 字节）**

| 路径 | 字节数 | 内容 |
| --- | --- | --- |
| `/font/cn16.bin` | 120160 | 3755 个 16×16 汉字（32 字节/字） |
| `/font/cn22.bin` | 247830 | 3755 个 22×22 汉字（66 字节/字） |
| `/font/as12.bin` | 1140 | 12 号 ASCII（95 字符 × 12 行 × 1 字节） |
| `/font/as16.bin` · `as16b.bin` | 1520 ×2 | 16 号 ASCII 常规 / 加粗 |
| `/font/as22.bin` · `as22b.bin` | 4180 ×2 | 22 号 ASCII 常规 / 加粗 |
| `/font/as32.bin` · `as32b.bin` | 6080 ×2 | 32 号 ASCII 常规 / 加粗 |
| `/font/as48.bin` · `as48b.bin` | 13680 ×2 | 48 号 ASCII 常规 / 加粗 |

**图片（32 个，合计 438728 字节）**

| 路径 | 字节数（含 4 字节头） | 内容 |
| --- | --- | --- |
| `/img/boot.bin` · `/img/main.bin` | 153604 ×2 | 开机全屏图、主页全屏图（240×320） |
| `/img/err.bin` | 3204 | 错误图标（40×40） |
| `/img/wifi.bin` · `wifi_off.bin` | 804 ×2 | WiFi 已连接 / 未连接（20×20） |
| `/img/loc.bin` · `noloc.bin` | 804 ×2 | 定位图标（**当前界面已不使用**） |
| `/img/temp.bin` · `humi.bin` · `thermo.bin` | 5004 ×3 | 温度 / 湿度 / 温湿度计图标（50×50） |
| `/img/w_*.bin` | 5004 ×22 | 22 张天气图标（50×50） |

两项合计 **858778 字节**，占 8MB 容量的 10.24%。开机自检项数 = 11 个字库 + 32 张图片 = **43 项**。

> 注意 `Font_12` / `Font_22` / `Font_32` / `Font_32B` 四个描述符在当前固件里**没有引用者**
> （`Font_32` 只被裸机测试用例使用，而该目标文件被链接器丢弃），但
> `Asset_Init()` 仍会打开并自检全部 11 个字库文件——**自检通过不代表每个字库都在用**。

### 6.4 字模定位与点阵格式

**定位公式（O(1)，不查表）**

```c
/* ASCII 单字符 */
ob  = ((size / 2) + 7) / 8;              /* 每行字节数 */
off = (ch - 0x20) * ob * size;

/* GB2312 汉字 */
off = ((区码 - 0xB0) * 94 + (位码 - 0xA1)) * cn_bytes;
```

旧实现按名字线性查表，每个汉字平均要 `strcmp` 1878 次；改用区位码直接算偏移后是常数时间。

**点阵格式约定**：横向取模、行优先，每行 `(宽+7)/8` 字节，**每字节低位 bit0 对应最左侧像素**。
重新生成字库时必须沿用这套取模设置，否则整份数据错位。

汉字表顺序严格等于 GB2312 区位码顺序（啊…座），文件里是**裸数据**，没有索引头。

### 6.5 图片格式与乒乓双缓冲

**文件格式**：`宽度(2 字节, 大端) + 高度(2 字节, 大端) + RGB565 像素(小端)`。
读取时跳过 4 字节头即可；格式与 `LFS_Operation` 的 `SaveImage()` 一致，`Asset` 自检也按
`4 + width × height × 2` 核对文件大小。

**乒乓双缓冲**（`ST7789_Draw_Picture()`）

```
s_picBuf[0] (7680 B)  ──DMA1_Stream5──▶ SPI3 ──▶ ST7789     ┐
        ↑ 交替                                              │ 同时进行
s_picBuf[1] (7680 B)  ◀──SPI1 轮询读── W25Q64               ┘
```

一块正由 DMA 发送时，CPU 同时从 SPI1 读下一块。两个缓冲各 `240 × 16 行 × 2 = 7680` 字节，
在 `ST7789_Init()` 中从 FreeRTOS 堆分配（共 15KB）；**分配失败自动退化为单缓冲**，
全屏图绘制时间约翻倍，但功能不受影响。

> **SPI1 与 SPI3 同为 21MHz 是这套方案成立的前提**：读 Flash 的时间正好被屏幕发送时间覆盖。
> 若把 SPI1 降速，读取会成为瓶颈，双缓冲的收益随之消失。

### 6.6 挂载、自检与失败降级

`Asset_Init()` 的开机流程：

1. `W25Q64_Init()` → 读 JEDEC ID，**要求等于 `0xEF4017`**，否则返回 `-1`；
2. `lfs_mount()`，失败返回 `-2`（**不格式化**）；
3. 常开 11 个字库文件句柄，记录每个文件的当前偏移以跳过重复 `lfs_file_seek`
   （littlefs 的 CTZ skip-list 遍历不便宜）；
4. 逐项核对：字库按固定期望大小，图片按 `4 + w×h×2`，用 32 位掩码记录通过的图片；
5. 打印 `selfcheck: checked=43, missing=N`，有缺失返回 `-3`。

**失败安全降级**：资源缺失时渲染退化为纯色填充并在串口告警，**不阻塞任务、不重启、
不自动格式化**。`Asset_Ready()` 供 UI 判断是否降级显示。

**`Asset` 对外接口**

| 函数 | 职责 |
| --- | --- |
| `Asset_Init()` | 挂载 + 自检；返回 `0` 就绪 / `-1` 未检测到芯片 / `-2` 挂载失败 / `-3` 有缺失 |
| `Asset_Ready()` | littlefs 是否已挂载 |
| `Asset_CheckedCount()` / `Asset_MissingCount()` | 自检统计 |
| `Asset_ReadFont(fid, off, buf, len)` | 按资源 ID + 字节偏移读字模；返回 `-1` 参数/未就绪、`-2` 越界、`-3` seek 失败、`-4` 长度不符 |
| `Asset_ImageOpen(img, f)` / `Asset_ImageRead(f, buf, len)` / `Asset_ImageClose(f)` | 打开图片（自动跳过 4 字节头）并顺序读像素 |
| `Asset_ImageOk(img)` | 该图片是否通过开机自检 |

### 6.7 资源烧录（首次使用必做）

正式固件**不含**资源数据，所以首次使用必须先烧录。资源总量 858650 字节（编译口径）超过
512KB Flash，无法一次编译带入，需分三批：

| 批次 | 内容 | 写入字节 | 编译后 RO-data |
| --- | --- | --- | --- |
| 1 | 汉字库 16 + 22 | 367990 | 369296 |
| 2 | 开机全屏图 + 主页全屏图 | 307200 | 308512 |
| 3 | 9 张 ASCII 表 + 30 张图标 | 183460 | 186064 |

> "写入字节"是 `Provision_Run()` 实际写入 W25Q64 的字节数；每张图片还要多 4 字节宽高头，
> 所以批次 2、3 比"编译进 Flash 的点阵/像素数据"分别多 8 / 120 字节（批次 1 是裸数据，两者相同）。
> 三批写入合计 858650 字节，对应 32 个文件的实际体积 858778 字节。

**操作步骤**

1. 把 `User/Inc/BuildConfig.h` 的 `RESOURCE_PROVISION` 改为 `1`，`PROVISION_BATCH` 改为 `1`；
2. **Rebuild** 后烧录，上电等待串口打印 `[PROV] ... OK`；
3. 重复步骤 1–2，`PROVISION_BATCH` 依次取 `2`、`3`；
4. 三批完成后把 `RESOURCE_PROVISION` 改回 `0`，再 Rebuild 烧录正式固件。

> ⚠️ **切换批次后必须 Rebuild（不是 Build）**：否则 `Font_*.c` / `Image_*.c` 不会按新宏
> 重新预处理，会出现 `L6218E: Undefined symbol` 之类的链接错误。已出现时执行
> `Project → Clean Targets` 再 Rebuild。

> ⚠️ **批次 1 会强制重建文件系统**（清掉历史残留），所以**重跑批次 1 会清空批次 2、3 的数据**，
> 必须按 1 → 2 → 3 顺序完整执行。

> ⚠️ **`RESOURCE_DATA_IN_ROM` 的联动必须为 1**：`BuildConfig.h:52-55` 在
> `RESOURCE_PROVISION == 1` 时会把 `RESOURCE_DATA_IN_ROM` 强制重定义为 `1`，
> 这样资源数据本体与 `Font.h` / `Image.h` 中的 `extern` 声明（同受该宏保护）才会参与编译。
> 若这个联动被改坏，`Provision.c` 引用的 `Chinese_Font16_Data`、`Font_*_Table`、`gImage_*`
> 连声明都不存在，**烧录固件会在编译期直接报"标识符未声明"**。

## 7. 编译、烧录与构建模式

### 7.1 环境

| 项 | 版本 |
| --- | --- |
| IDE | Keil MDK-ARM **Plus 5.24.1** |
| 编译器 | **ARMCC V5.06 update 5（build 528）**，即 AC5（工程中 `uAC6 = 0`） |
| C 标准 | C99（`uC99 = 1`） |
| C 库 | **microlib**（链接参数含 `--library_type=microlib`） |
| 工程 | `MDK/STM32F407.uvprojx` |
| Target / 器件 | `STM32F407` / `STM32F407VETx`（Pack `Keil.STM32F4xx_DFP.2.15.0`） |

### 7.2 关键工程配置

| 配置 | 值 |
| --- | --- |
| 预处理宏 | `USE_STDPERIPH_DRIVER,STM32F40_41xxx` |
| 优化等级 | Level 3（`-O3`） |
| 警告等级 | All Warnings |
| 包含路径（10 项） | `..\User\Inc` · `..\Resource\Inc` · `..\BSP\Inc` · `..\Core` · `..\Core\Startup` · `..\STM32F4xx_StdPeriph_Driver\inc` · `..\STM32F4xx_StdPeriph_Driver\src` · `..\Third_Lib\FreeRTOS\include` · `..\Third_Lib\FreeRTOS\portable` · `..\Third_Lib\LittleFS\Inc` |
| 输出 | 目录 `.\Output\`，名字 `STM32F407` |
| 启动文件 | `Core/Startup/startup_stm32f40xx.s` |
| 分散加载 | **无外部 `.sct`**（`ScatterFile` 为空）——`MDK/Output/STM32F407.sct` 由 uVision 自动生成 |
| 生成 hex | **否**（`CreateHexFile = 0`，Output 内没有 `.hex` / `.elf`） |

自动生成的分散加载内容：

```
LR_IROM1 0x08000000 0x00080000 {          ; 512KB Flash
  ER_IROM1 0x08000000 0x00080000 { *.o (RESET, +First); *(InRoot$$Sections); .ANY (+RO) }
  RW_IRAM1 0x20000000 0x00020000 { .ANY (+RW +ZI) }   ; 128KB SRAM
}
```

> ⚠️ **CCM（64KB @ `0x10000000`）没有被使用**：它只出现在 `Cpu` 串与 `OCR_RVCT10` 声明里，
> 自动生成的 sct **没有为它定义执行域**。若要把大缓冲移进 CCM 省 SRAM，需要手写 sct。

### 7.3 工程文件分组

工程共 **9 个分组 / 100 个文件条目**：

| 分组 | 文件数 | 内容 |
| --- | --- | --- |
| USER/Config | 3 | `stm32f4xx_conf.h`、`FreeRTOSConfig.h`、`lfs_config.h`（跨目录集中列出的编译期配置头） |
| USER/Src | 9 | `main.c`、`Board.c`、`App.c`、`app_task.c`、`bare_test.c`、`Provision.c`、`stm32f4xx_it.c`、`Boot_Page.c`、`Main_Page.c` |
| RESOURCE | 12 | `Font_12/16/22/32/48.c`、`Chinese_Font16/22.c`、`Image.c`、`Image_Boot_Page.c`、`Image_Main_Page.c`、`Image_Weather.c`、`ImageTable.c` |
| BSP | 13 | 见 1.4 |
| STARTUP | 1 | `startup_stm32f40xx.s` |
| CMSIS | 7 | `stm32f4xx.h`、`system_stm32f4xx.c/.h`、`core_cm4.h`、`core_cmFunc/Instr/Simd.h` |
| STM32F4xx_StdPeriph_Driver | 42 | 标准外设库（磁盘上的 `stm32f4xx_fmc.c` **未加入工程**） |
| THIRD_LIB/FreeRTOS | 9 | 7 个内核 `.c` + `heap_4.c` + `port.c` |
| THIRD_LIB/LittleFS | 4 | `lfs.c`、`lfs_util.c`、`lfs_port.c`、`LFS_Operation.c` |

所有文件条目都只有 `FileName` / `FileType` / `FilePath`，**没有 per-file 覆盖选项**。

> **新增源文件**必须同时加入对应分组，并确认头文件目录已在包含路径中。

### 7.4 三种构建模式

`User/Inc/BuildConfig.h` 的三个宏决定 `main()` 走哪条分支：

| 宏组合 | 模式 | 行为 |
| --- | --- | --- |
| `RESOURCE_PROVISION = 1` | **资源烧录** | `Board_Peripheral_Init()` + `Usart2_Debug_Init()` → `Provision_Run()`（内部死循环）。只初始化 W25Q64 与串口，**不启动 RTOS、不初始化 LCD** |
| `USE_FREERTOS = 1`（当前） | **RTOS 运行** | `Board_Peripheral_Init()` → `App_Task_Init()` → `vTaskStartScheduler()`。正式固件 |
| `USE_FREERTOS = 0` | **裸机测试** | `Board_Peripheral_Init()` → `Test()` → `TIM5_Init()` → `BareMetal_Module_Test()`，每个测试自带死循环 |

默认值：

```c
#define USE_FREERTOS           1
#define RESOURCE_DATA_IN_ROM   0   /* 0=正式固件（资源不编译）; 1=烧录固件 */
#define RESOURCE_PROVISION     0   /* 1=进入资源烧录流程 */
#define PROVISION_BATCH        1   /* 烧录批次 1..3 */

#if (RESOURCE_PROVISION == 1)      /* 联动：烧录固件必然需要数据本体 */
#undef  RESOURCE_DATA_IN_ROM
#define RESOURCE_DATA_IN_ROM   1
#endif
```

裸机模式下再由 `BM_TEST_MODULE` 选择测试项（见第 8 章）。

### 7.5 构建产物

| 文件 | 说明 |
| --- | --- |
| `MDK/Output/STM32F407.axf` | 可执行映像（默认产物，约 916KB） |
| `MDK/Output/STM32F407.map` | 链接映射表（内存分布与符号） |
| `MDK/Output/STM32F407.htm` | 静态调用图 |
| `MDK/Output/STM32F407.build_log.htm` | 构建日志 |
| `MDK/Output/*.o` / `*.crf` / `*.d` | 各编译单元的中间产物 |

> ⚠️ **不生成 `.hex`**，且 `fromelf --bin` 的 User 命令虽已填好但 **`RunUserProg1 = 0` 未启用**，
> 所以 `.bin` 不会自动刷新。仓库里的 `MDK/Output/STM32F407.bin` 只有 1316 字节、时间戳是
> 2025-03-07，**是陈旧文件，不要直接使用**。

需要 `.bin` 时：

1. `Project → Options for Target → User`，勾选 `Run #1`（命令已填好：
   `"$K\ARM\ARMCC\bin\fromelf.exe" --bin --output="$L@L.bin" "#L"`），重新编译；或
2. 手动执行：
   `D:\Keil5\ARM\ARMCC\bin\fromelf.exe --bin --output=MDK\Output\STM32F407.bin MDK\Output\STM32F407.axf`。

### 7.6 资源占用

**当前构建**（`MDK/build.log`，0 Error 0 Warning）：

```text
Program Size: Code=52324  RO-data=3748  RW-data=412  ZI-data=126740
Total RO  Size (Code + RO Data)              56072
Total RW  Size (RW Data + ZI Data)          127152
Total ROM Size (Code + RO Data + RW Data)   56184
```

| 项 | 数值 | 占比 |
| --- | --- | --- |
| Flash（`Total RO Size`） | **56072 B（54.8KB）** | 512KB 的 10.7% |
| SRAM（`Total RW Size`） | **127152 B（124.2KB）** | 128KB 的 **97.0%**，静态余量仅 **3920 B** |

> `Total ROM Size` 56184 是 map 的口径（RW-data 按压缩后 112 字节计入）；
> 未压缩口径为 `Code + RO-data + RW-data = 56484`。

**SRAM 的主要占用**（取自 map 的符号表）

| 符号 | 字节 | 归属 |
| --- | --- | --- |
| `ucHeap` | 90112 | FreeRTOS 堆（`configTOTAL_HEAP_SIZE = 1024*88`） |
| `s_scratch` | 23040 | LCD 渲染缓冲 |
| `bitmap` | 1152 | OLED 临时位图 |
| `rx_buf` | 1024 | AT 应答缓冲 |
| `s_file` | 924 | Asset 的 11 个常开文件句柄 |
| `rx_ring` | 512 | USART1 接收环形缓冲 |
| `HEAP` / `STACK`（启动文件） | 4096 + 4096 | microlib 的堆与栈 |
| `dbg_buf` / `tx_buf` | 256 + 256 | 调试行缓冲 / HTTP 指令缓冲 |

**代码占用前列**（按目标文件）：`lfs.o` 15988 > `tasks.o` 3422 > `lcd.o` 2832 >
`app_task.o` 2660 > `main_page.o` 2446 > `app.o` 2288 > `at.o` 1688 > `asset.o` 1344。

> ⚠️ **SRAM 只剩约 3.8KB 静态余量**（堆内另有约 56KB，但 15KB 已被图片乒乓缓冲占用）。
> 新增较大静态缓冲前务必重新确认 `ZI-data`。

### 7.7 下载与调试

| 项 | 内容 |
| --- | --- |
| 调试器 | **ST-Link**（`STLink\ST-LINKIII-KEIL_SWO.dll`，SW-DP，ID `2BA01477`） |
| Flash 算法 | `STM32F4xx_512`（起始 `0x08000000`，大小 `0x80000`） |
| 调试器配置 | `MDK/DebugConfig/STM32F407_STM32F407VETx.dbgconf` |
| SVD | `STM32F407VETx$CMSIS\SVD\STM32F40x.svd` |
| 烧录前 | ESP32-C3 需预先烧录 AT 固件（见 `Documents/ESP32-C3/`），默认波特率 115200 |

> `uvprojx` 的 Utilities 页仍残留旧的 `UL2CM3`（ULINK2）配置，但 `uvoptx` 的调试页实际选择的是
> ST-Link。**以 Keil 打开工程后 `Options for Target → Debug` 下拉框显示为准。**

### 7.8 版本控制与清理

`.gitignore` 忽略：`MDK/Output/`（整个构建输出目录）、`MDK/build.log`、
Keil 中间文件（`*.o` `*.crf` `*.d` `*.axf` `*.map` `*.sct` `*.dep` `*.iex` `*.lnp`
`*.build_log.htm`）、个人界面布局（`*.uvgui.*` `*.uvguix.*`）、J-Link 日志，
以及 `MDK/.vscode/*.log` 与 `*.log.lock`。

**仍然跟踪**：`STM32F407.uvprojx`、`STM32F407.uvoptx`、`MDK/DebugConfig/`、
`MDK/.vscode/c_cpp_properties.json`、`MDK/.vscode/settings.json`、`KeilClear.bat`。

根目录 `KeilClear.bat` 递归删除 `*.bak *.lst *.obj *.crf *.o *.d *.axf *.map *.sct *.htm *.dep`
等中间产物。

> ⚠️ 它**也删除 `*.map` / `*.sct` / `*.htm`**，即 7.6 节的尺寸证据会被一并清掉（重新编译即可恢复）。
> 它**不删** `*.bin`（所以陈旧 `.bin` 会残留）、`*.uvguix.*`、`*.uvoptx`、`Output/build_*.log`。

## 8. 裸机模块测试

把 `BuildConfig.h` 的 `USE_FREERTOS` 置 `0` 即进入裸机模式；再由 `BM_TEST_MODULE` 选择测试项。
**该宏是唯一入口**，`bare_test.c` 中不再另建同名变量。

裸机 `main()` **不调用 `Board_Init()`**，因此 `BareMetal_Module_Test()` 内部会先调用
`Usart2_Debug_Init()` 初始化调试口——调度器未运行时 `fputc` 自动退化为逐字节轮询，
所以 `printf` 可以直接使用。每个测试自带死循环。

| 宏值 | 用例 | 内容 |
| --- | --- | --- |
| 1 | `BM_TEST_MODULE_OLED` | `OLED_Init` + 固定字符串 + 计数器，500ms 刷新 |
| 2 | `BM_TEST_MODULE_LIGHT` | OLED 显示 `ADC_VAL=...`（AO 模式）或 `DO_STATE=...`（DO 模式），500ms 刷新 |
| 3 | `BM_TEST_MODULE_DS1302` | 每秒读一次外部 RTC，OLED 显示日期 / `HH:MM:SS` / 读取计数，USART2 输出同样内容；含首次写入与回读校验 |
| 4 | `BM_TEST_MODULE_W25Q64` | SPI Flash 裸驱动用例（见下） |
| 5（默认） | `BM_TEST_MODULE_LFS` | littlefs 文件系统用例（依赖 W25Q64） |

> `BuildConfig.h` 中建议的测试顺序是 **W25Q64 → LFS**：底层驱动不通时文件系统必然失败，
> 先跑 W25Q64 可以把"硬件/SPI/时序问题"与"文件系统问题"分开定位。

### 8.1 W25Q64 用例

| 项 | 覆盖内容 |
| --- | --- |
| 器件识别 | JEDEC ID（`0x9F`）、Read ID（`0x90`） |
| 整扇区擦除 | 擦除后逐字节核对全 `0xFF` |
| 页内写入 | 页首 16 字节、页尾偏移 `0x0F0` 处 32 字节（跨页边界） |
| 跨页写入 | 页内 `0x200` 处 16 字节、`0x2F0` 处 256 字节 |
| 跨扇区写入 | `0x0F00` 处 512 字节（跨 4KB 扇区边界） |
| 整扇区读写 | 4KB 全扇区写入 + 回读比对 |
| 忙等待统计 | `W25Q64_GetBusyTimeoutCount()` |

测试区固定为**最后 4 个 4KB 扇区**：`W25Q64_TEST_BASE = 8MB - 4 × 4KB = 0x7FC000`。

> ⚠️ **这个"测试区"与 littlefs 的区域是重叠的**：littlefs 的 `LFS_BLOCK_COUNT = 2048`
> 且没有设置 `.offset`，即**覆盖整片 8MB、不预留任何分区**。当前资源只占约 10%，
> 文件系统不会分配到末尾 16KB，所以测试能安全通过；但这属于"靠用量低避开"，
> 而不是"有分区保护"。若将来资源增长到接近满盘，裸机测试会破坏文件系统数据。
> 要彻底规避，需要给 littlefs 设置 `.offset` 或缩小 `LFS_BLOCK_COUNT`。

### 8.2 littlefs 用例

覆盖：基本读写、`mkdir`/`stat`、追加、64KB 大文件、目录遍历、删除、卸载重挂载持久化、
`SaveFont` / `LoadFont`、`SaveImage` / `LoadImage`，以及"缺失文件应返回 `LFS_ERR_NOENT`"。

### 8.3 DS1302 裸机联调

宏集中在 `bare_test.c` 顶部：

| 宏 | 默认 | 作用 |
| --- | --- | --- |
| `DS1302_TEST_FORCE_SET` | `0` | `0` = 仅在读取失败或年份早于下限时写入基准时间；`1` = 每次上电都写入 |
| `DS1302_TEST_MIN_YEAR` | `2020` | 判定"RTC 未初始化"的年份下限 |
| `DS1302_TEST_YEAR` … `DS1302_TEST_SECOND` | `2026-09-13 12:00:00 W7` | 基准时间（星期 1 = 周一 … 7 = 周日） |
| `DS1302_TEST_HALT_LIMIT` | `3` | 连续读到相同秒值达到该次数即在界面提示 `HALT`（晶振停振 / 电池欠压） |
| `DS1302_TEST_LOG_PERIOD` | `10` | 串口日志周期（秒） |

**行为**

- 开机：打印 `DS1302 init done`；若 RTC 未初始化则写入基准时间并回读校验，打印
  `SET VERIFY OK/FAIL`；否则打印 `RTC already running, keep existing time: ...`。
- 运行时：每秒读一次并刷新 OLED（日期 / `Font_32` 的 `HH:MM:SS` / 状态行），
  每 `DS1302_TEST_LOG_PERIOD` 秒打印一行 `read OK: ... (RD=... ER=...)`。
- 读取失败：`rd_err` 累加，OLED 保留上一次有效时间并显示 `ER=n`；每秒重试，不阻塞、不死循环。
- 走时停滞：秒值连续 `DS1302_TEST_HALT_LIMIT` 次不变时显示 `HALT` 并打印一次告警。

> 注意 `CH = 1`（振荡器停）时 `DS1302_ReadTime()` 会**直接返回 `false`**，所以振荡器停走
> 会先落入 `ER=n` 分支；`HALT` 检测主要覆盖"`CH = 0` 但秒值因晶振/电池问题不前进"的场景。
>
> 又因为 `CH` 与写保护位的**上电状态均未定义**（见 DS1302 手册），`FORCE_SET = 0` 时
> 也可能自动落入"写入基准时间"分支——这顺带启动了振荡器。要验证断电保持，
> 需保证 `CH = 0` 后再断电。

**时序实测结论**：本模块的 DAT 没有外部上拉，仅靠 MCU 内部约 40k 上拉，因此把半周期放宽到
`10µs` 是必要的（实测 `ER = 0`，读数与串口时间戳同步）。**这条时序对 `delay_us()` 是绝对依赖**——
半周期只要被压缩到 250ns 以下，DS1302 就来不及在下降沿输出新位而被主机采到旧值，
表现为**随机单一位错误**（见 11.3）。

---

## 9. 需要自行配置的参数

以下值硬编码在 `User/Src/App.c` 顶部，**仓库中是明文真实凭据**，本文档按脱敏形式给出：

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
| `weather_url` | 心知天气（Seniverse）实况接口，`key` 需自行申请；`location` 支持城市拼音或经纬度 |
| `language` | 当前 `en`；改为 `zh-Hans` 可让 `text` 字段返回中文（界面用本地映射表，不受影响） |
| `unit` | `c` 表示摄氏度 |

> ⚠️ **对外发布前请先替换为占位符**（或抽到被 `.gitignore` 忽略的独立配置文件中）——
> 当前仓库里是真实可用的凭据。
>
> `location` 只影响返回的天气数据本身（城市名会填入 `weather_info.city`），
> **界面不显示城市名**——顶部状态条只有 WiFi 图标与 SSID。

## 10. 调试与排障

### 10.1 日志通道

日志走 **USART2 @ 115200-8N1**，接 USB-TTL 即可查看。

> 早期版本里 AT 层另有一组 `AT_DEBUG` 收发打印（`[EDBUG] Command:` / `[EDBUG] Response:`），
> **该开关已从 `AT.c` 中移除**，现在 AT 收发内容不再单独输出。定位 AT 问题靠
> `[NET] AT init FAILED, retry=n/m, rx len=N: <原始回复>` 这一行（见 10.3 与 11.11）。

### 10.2 上电典型输出

```
[LCD ] ping-pong buffer ready: 7680 x 2 = 15360 bytes
[ASSET] W25Q64 jedec = 0xEF4017
[ASSET] littlefs mounted: 2048 blocks x 4096 B
[ASSET] selfcheck: checked=43, missing=0
[SYS]Build Date:Sep 18 2026 00:22:24
[UI] Board init done, boot page
[NET] Init Start
[NET] AT init OK
[NET] WiFi init OK
[NET] WiFi connecting to <ssid> ...
[NET] WiFi connected: ssid=... bssid=... channel=1 rssi=-72
[NET] SNTP sync OK: 2026-09-18 00:23:10
[NET] Weather OK: Cloudy, code=4, temp=22.0
[UI] Boot net stage done: wifi=1 service=1
[UI] Enter main page
[LIGHT] task ready: DO_state=0 dark=0
[KEY] task ready: pressed=0
[SENSOR] DHT22 OK: T=26.4 H=59.2
[LIGHT] auto: level=bright, mode=day
```

> **开头 4 行为什么在 `[SYS]` 之前**：`[LCD ]` 与 `[ASSET]` 是 `Board_Init()` **内部**打印的
> （`ST7789_Init()` 与 `Asset_Init()`），而 `[SYS]Build Date` 在 `Board_Init()` **返回之后**。
> `Board_Init()` 的前四步（`Test()` / `TIM5_Init()` / `Usart2_Debug_Init()` / `Prof_Init()`）
> 都不打印，且串口本身要到第三步才初始化，所以上电后可见的**第一条**日志必然是 LCD 的
> 乒乓缓冲分配结果。
>
> 资源从未烧录时，会在 `[UI] Board init done` 前多出
> `[UI] WARN: asset layer not ready, screen shows blank blocks` 与
> `[UI] WARN: flash the provision firmware to write fonts/images`。

### 10.3 运行期日志一览

| 模块 | 日志行 | 含义 |
| --- | --- | --- |
| 网络 | `[NET] WiFi check OK: ssid=... rssi=...` | 周期检查通过 |
| | `[NET] WiFi state query FAILED` | AT 查询失败（**不等于掉线**） |
| | `[NET] WiFi lost, reconnecting...` | 模组确认未连接，开始重连 |
| | `[NET] WiFi reconnected: ssid=... rssi=...` | 重连成功 |
| | `[NET] WiFi connect FAILED` / `WiFi info FAILED` / `WiFi reconnect FAILED` | 连接或取信息失败 |
| | `[NET] WiFi reconnect FAILED: no valid info` | 重连后仍拿不到有效状态 |
| | `[NET] WiFi modem-sleep` / `[NET] WiFi wake` | 模组进入 / 退出省电档位 |
| | `[NET] WiFi sleep set FAILED` | `AT+SLEEP` 下发失败（下个周期自动重试） |
| | `[NET] LowPower exit, reset update` | 退出夜间后收到补更请求 |
| | `[NET] SNTP sync OK: ...` / `SNTP sync FAILED` | 校时结果 |
| | `[NET] Weather OK: ... , code=..., temp=...` | 天气更新成功 |
| | `[NET] Weather HTTP FAILED` / `Weather parse FAILED` | 取数失败 / 解析失败 |
| | `[NET] AT init OK` / `[NET] WiFi init OK` | 初始化成功（**只有真的成功才会打印**，见 11.11） |
| | `[NET] AT init FAILED, retry=n/3, rx len=N: ...` / `... max retry reached` | 初始化失败并重试；`rx len` 为 0 表示模组**完全没回话**，非 0 时冒号后是模组原始回复 |
| 传感器 | `[SENSOR] DHT22 OK: T=... H=...` | 采集成功 |
| | `[SENSOR] DHT22 FAIL: code=n (原因)` | 采集失败，错误码见 4.6 |
| 昼夜 | `[LIGHT] task ready: DO_state=.. dark=..` | 光敏任务启动，附上电电平 |
| | `[LIGHT] auto: level=.., mode=..` | 去抖结束后的自动判定 |
| | `[LIGHT] auto request NIGHT` / `DAY` | 自动判定确实要改变模式 |
| | `[LIGHT] key switch -> night` / `day` | 按键手动切换完成 |
| | `[LP] Sync RTC time done` / `fail` | 进入夜间前回写 DS1302 并回读校验 |
| | `[LP] Enter night: LCD off, OLED on, time_source=RTC` | 进入夜间（`SoftClock` 表示降级） |
| | `[UI] Enter day: OLED off, LCD on` | 退出夜间 |
| 性能 | `[PROF] full-screen image 240x320, 153600 bytes, ping-pong, N us` | 整屏底图绘制耗时（`Prof_Us` 实测）。`single` 表示乒乓缓冲分配失败、退化为单缓冲，见 4.11 |
| | `[PROF] main page render: N us` | `Main_Page_Display()` 整页重绘耗时 |
| 按键 | `[KEY] task ready: pressed=0` | 按键任务启动 |
| 系统 | `[SYS]Build Date:...` | 固件编译时间 |
| | `[UI] Board init done, boot page` | 板级初始化完成 |
| | `[UI] Boot net stage done: wifi=1 service=1` | 开机网络阶段结束 |
| | `[UI] Enter main page` | 进入主界面 |

### 10.4 夜间验证方法

进入夜间后连续 ≥5 分钟**不应出现**下列任何一行：

```
[NET] WiFi check OK / WiFi lost, reconnecting / WiFi reconnected
[NET] SNTP sync OK / SNTP sync FAILED
[NET] Weather OK / Weather HTTP FAILED / Weather parse FAILED
[SENSOR] DHT22 OK / DHT22 FAIL
```

正确的夜间序列应当是：

```
[LIGHT] auto: level=dark, mode=day
[LIGHT] auto request NIGHT
[LP] Sync RTC time done
[LP] Enter night: LCD off, OLED on, time_source=RTC
[NET] WiFi modem-sleep
   ... 静默（OLED 每分钟刷新一次）...
[UI] Enter day: OLED off, LCD on
[NET] WiFi wake
[NET] LowPower exit, reset update
```

> 注意 `[NET] WiFi wake` 必须**早于** `[NET] LowPower exit, reset update`——前者由档位同步
> 打印，后者是补更请求。若顺序反了，说明 5.5 的第 3 条顺序约定被破坏。

若序列中出现 `[LP] Sync RTC time fail` 且 `time_source=SoftClock`，说明 DS1302 读取失败、
夜间时间回退到软件时钟——**通电期间显示仍然正确，但断电后时间不保存**，RTC 形同虚设。
这是读时序问题，见 11.10。

### 10.5 资源层排障

| 现象 | 含义 | 处理 |
| --- | --- | --- |
| `[ASSET] ERROR: W25Q64 not detected (jedec = 0x......)` | 芯片无响应 | 检查 PA4/PA5/PA6/PA7 与供电 |
| `[ASSET] ERROR: littlefs mount failed, err = n` | 未烧录资源 | 按 6.7 执行三批烧录；**固件刻意不自动格式化** |
| 43 项全是 `[ASSET] MISSING /font/cn16.bin err=-2` | 资源从未烧录（`-2` = `LFS_ERR_NOENT`）。**挂载成功不代表资源存在**——空的旧文件系统同样能挂载 | 同上 |
| 个别 `[ASSET] MISSING` 或 `[ASSET] BADSIZE ... size=... want=...` | 对应批次未烧录或写入中断 | 重跑该批次 |
| `[ASSET] HINT: no resources on flash, flash the provision firmware first` | 上一条的显式提示 | 同上 |
| `[UI] WARN: asset layer not ready, screen shows blank blocks` | UI 启动时资源层未就绪 | 同上 |
| `[LCD ] ping-pong buffer alloc failed, fallback to single buffer` | 堆空间不足 | 已自动退化为单缓冲（全屏图约慢一倍），检查堆用量 |
| `[ERR]ST7789_DMA_Pump: DMA传输错误` / `[ERR]DMA transfer error` | SPI3 DMA 异常 | 检查 SPI3 与 DMA1_Stream5 配置 |
| 屏幕只有纯色块、无任何文字 | 资源层未就绪 | 看 `[ASSET]` 日志 |

### 10.6 光敏不切换的判读

| 现象 | 结论 |
| --- | --- |
| 没有 `[LIGHT] task ready` | 光敏任务未创建（检查 `UI_Task` 是否走到创建任务那一步） |
| 遮挡 1s 后仍无 `[LIGHT] auto:` 或 `[LIGHT] key switch` | 电平从未变化 → 接线/供电/模块问题（有 1s 轮询兜底，不会像纯中断方案那样"永远不动"） |
| `level` 始终为 `bright` | 极性相反（模块可能是"暗→DO 低"），需反转 `Light_Sensor_IsDark()` 的判定 |
| 有 `request NIGHT` 但无 `[LP] Enter night` | 问题在 `UI_Task` 的事件处理分支 |
| 手动切换后立刻被改回 | 检查 `s_baseline_dark` 是否在手动切换时一并更新（见 5.6） |

### 10.7 烧录固件日志

```
[PROV] batch = 1, fonts = 2, images = 0
[PROV] W25Q64 jedec = 0xEF4017
[PROV] batch 1: format filesystem first
[PROV] /font/cn16.bin             120160  OK
[PROV] /font/cn22.bin             247830  OK
[PROV] batch 1 done: PASS 2, FAIL 0, wrote 367990 bytes
[PROV] fs used blocks = 97 / 2048
[PROV] next: change PROVISION_BATCH, or set RESOURCE_PROVISION back to 0
```

`[PROV] ERROR: W25Q64 not detected, stopped` 表示芯片未识别；
`[PROV] ERROR: littlefs init failed, err = n, stopped` 表示格式化或挂载失败。

### 10.8 硬件异常

```
Assertion failed in file ... at line ...      ← vAssertCalled()
Stack overflow in task <name>                 ← vApplicationStackOverflowHook()
```

两者都进入死循环，便于在调试器里查看现场（实现于 `User/Src/main.c:62-75`）。
它们能打印出来，依赖 `printf` 在"调度器已运行但处于异常上下文"时仍能工作（见 4.2）。

## 11. 设计决策与踩坑记录

### 11.1 为什么没有启用 MCU 的 tickless idle

`configUSE_TICKLESS_IDLE` 保持 **`0`**。曾把它设为 `1` 并实测，结论是不划算：

- FreeRTOS 的 `vPortSuppressTicksAndSleep()` 用 `__disable_irq()`（PRIMASK）而不是
  `taskENTER_CRITICAL()` 进临界区，`port.c:568-570` 的注释写明了原因：
  *"don't use the taskENTER_CRITICAL() method as that will mask interrupts that should exit
  sleep mode"*——**正是为了让中断仍能唤醒 WFI**。
- 而本系统 **TIM5 以 1ms 周期产生中断、优先级 2**，于是每次 `__WFI` 最多睡约 1ms 就被唤醒。
- `port.c:722` 算出的 `xMaximumPossibleSuppressedTicks` 上限是
  `0xFFFFFF / (168000000 / 1000) = 99ms`，这个上限**根本用不上**。
- 换算下来每秒多出约 1000 次"进/出睡眠 + 重载 SysTick + `vTaskStepTick()`"的开销，
  换来的只是把 idle 空转的 CPU 停掉约 1ms/次，**收益远低于预期**。

> 若将来要重新评估：**先把 TIM5 的 `ARR` 拉长**（例如 999 → 9999，中断周期 1ms → 10ms，
> 同时把 `TIM5_Get_us()` 里的 `ms * 1000` 改成 `ms * 10000`），让单次睡眠能到 10ms 量级，
> 再开启 tickless。

**再往下的 Stop 模式需要三件配套**（当前都没做）：

1. Stop 下只有 EXTI 与 RTC 能唤醒，而夜间 OLED 每分钟要刷新、**DS1302 没有中断输出接到 MCU**
   （只有 RST/IO/CLK 三线），所以必须启用 MCU 内部 RTC 当唤醒定时器（需要 LSE 晶振或 LSI——
   代码里目前一处 `RCC_LSEConfig` 都没有）；
2. `vPortSuppressTicksAndSleep()` 的 tick 补偿完全基于 SysTick 计数，Stop 下 SysTick 停摆，
   该算式不成立，必须改写成用 RTC 计算实际睡眠时长；
3. Stop 退出后系统跑在 HSI(16MHz)，必须重配 HSE + PLL 回到 168MHz 并更新 `SystemCoreClock`，
   否则 `TIM5_Get_us()` 时基错乱，DHT22 与 DS1302 的时序会整体失准。

此外 Stop 前应把 OLED 的 SCL/SDA 停在高电平，避免 SSD1306 卡在半次传输。
（`PA0` 按键同时也是 `WKUP1`，若将来做 Standby 唤醒可以直接复用。）

### 11.2 两个补更事件位为什么必须分开

**症状**：退出夜间后只有一半数据被刷新。

**根因**：最初两个任务共用同一个事件位。`xEventGroupWaitBits` 是**清除式**等待——
先被唤醒的任务会把位清掉，另一个任务就永远收不到通知。

**修复**：拆成 `EV_NET_UPDATE_NOW`（bit7，只有 `Net_Task` 消费）与
`EV_DHT22_UPDATE_NOW`（bit8，只有 `DHT22_Task` 消费）。

### 11.3 OLED 数字乱跳：`delay_us()` 下溢

这是本项目最隐蔽的一个 bug，完整记录如下。

**现象**：夜间 OLED 上的数字跳到错误的值，过一会儿自己变回来。
例如 `00:13` 显示成 `01:13`、`00:14` 显示成 `10:14`、18 日显示成 20 日。
错误率约 30%，且**只跳一位**。

**排查过程**：

1. 现象最初与启用 tickless idle 同时出现，一度怀疑是它；但**回退 tickless 后仍然复现**，
   因此排除（回退后 `configUSE_TICKLESS_IDLE = 0`）。
2. 由于错误值仍在合法范围内（`01:13` 是合法时间、`20` 是合法日期），**范围校验挡不住**，
   DS1302 自身的 BCD 校验也发现不了。
3. 最终定位到 **`delay_us()` 提前返回**。

**根因**：`TIM5_Get_us()` 由 `TIM5_ms * 1000 + CNT` 组成。当 `CNT` 已经回绕（重新从小值开始）、
但 1ms 更新中断**还没来得及执行**时，`TIM5_ms` 仍是旧值，于是 `TIM5_Get_us()` 返回一个
**比实际小 1000µs** 的值——时间"倒流"了。

旧 `delay_us()` 用的是**无符号**相减：

```c
uint64_t start = TIM5_Get_us();
while ((TIM5_Get_us() - start) < us) ;     /* 旧写法 */
```

遇到时间倒流时 `TIM5_Get_us() - start` 会**下溢成约 2⁶⁴ 的巨大值**，循环条件立刻不成立，
`delay_us()` **几乎不延时就直接返回**。

DS1302 的驱动用 `delay_us(10)` 做 SCLK 半周期。一旦这个延时被压到 200ns 以下，
DS1302 就来不及在下降沿输出新位，主机采到的是旧值——表现为**随机单一位错误**，
正好与观测到的现象吻合。

**修复（三层）**

| 层 | 文件 | 改动 |
| --- | --- | --- |
| 时基 | `BSP/Src/Timer.c` | `TIM5_Get_us()` 借助 `TIM5->SR` 的 `UIF` 标志补回缺失的 1ms，**保证单调不减** |
| 延时 | `BSP/Src/Timer.c` | `delay_us()` 改用**有符号差比较** `(int64_t)(now - start) < (int64_t)us` 兜底 |
| 数据 | `BSP/Src/External_RTC.c` | `DS1302_ReadTime()` **连读两次比对**，分/时/日/月/周/年必须完全一致（不含秒） |

同样的有符号比较也应用到了 `DHT22_ReadByte()` 的位宽判别——那里若发生下溢，会把任意位
误判为 `1`，症状是校验和错误而非明显的乱码，更难察觉。

**代价**：DS1302 读取耗时从约 2ms 变成约 4ms，每 2s 一次，可忽略。

> 这个 bug 的教训是：**"时间戳相减"必须用有符号比较**。只要时间源可能回退（哪怕只有一个
> 采样窗口的宽度），无符号相减就会把微小的负差放大成天文数字。

### 11.4 按键中断函数名的大小写

`Key.h` 定义 `#define KEY_EXTI_IRQHandler EXTI0_IRQHandler`，所以 `Key.c` 里的函数必须写成
**全大写** `void KEY_EXTI_IRQHandler(void)`。写成混合大小写的 `Key_EXTI_IRQHandler` 时，
预处理器不会替换（宏替换区分大小写），于是它只是一个没人调用的普通函数，而
`EXTI0_IRQHandler` 仍指向启动文件里的 `[WEAK]` 默认实现——那串共享的 `B .` 无限循环。

**症状**：按一下按键整个系统立刻静止、串口无任何新输出，而**编译 0 Error 0 新增警告**。

**验证**：`MDK/Output/STM32F407.map` 中应出现
`startup_stm32f40xx.o(RESET) refers to key.o(i.EXTI0_IRQHandler)`。

### 11.5 光敏的 GPIO 与 EXTI 端口源必须成对修改

只改 GPIO 端口/引脚、不改 `LIGHT_SENSOR_DO_EXTI_PORT_SOURCE` / `..._PIN_SOURCE` 时，
EXTI 中断源仍挂在旧端口上，**电平变化不会产生任何中断**。

本项目实测踩过一次：GPIO 改到 PC1、EXTI 仍挂 PA1，表现为"遮挡光敏毫无反应、串口也无日志"。

### 11.6 ST7789 复位不清 GRAM

**现象**：上电先闪出上次的旧画面。

**根因**：ST7789 复位**不会清 GRAM**。

**处理**：`ST7789_Init()` 在开显示与点背光**之前**先刷黑 GRAM。

### 11.7 SSD1306 不断电就一直保留画面

**现象**：切回 FreeRTOS 固件后，OLED 一直挂着裸机测试时的画面。

**根因**：SSD1306 只要不断电就保持 GDDRAM 内容与显示开关状态。

**处理**：`Board_Init()` 上电即 `OLED_Init()` + `OLED_Display_Off()`；并且 `OLED_Init()`
内部是**先清屏、最后才 `0xAF` 开显示**，否则从开显示到清屏写完这段时间旧内容会被看到。
另外 `OLED_Clear()` 改为**按页整块写**——逐列发送需要 1024 次单字节 I2C 事务、耗时约 240ms，
按页写只要 8 次事务。

### 11.8 DS1302 的 CH 位与星期语义

- **CH（Clock Halt）位**：读时间前先判断秒寄存器的 bit7，`CH = 1`（振荡器停）时直接判定失败，
  让调用方回退软件时钟。否则读到的秒值被冻结，界面会显示一个**不动的钟**，
  而日志仍报 `time_source=RTC`——典型的"看起来正常但时间是错的"故障。
  注意 CH 与写保护位的**上电状态均未定义**（见 DS1302 手册）。
- **星期语义**：DS1302 寄存器是 1~7，旧代码写 `weekday - 1` 会得到 0，导致**周一时间同步失败**。
  现统一为 1~7，读回时 `week < 1` 直接判失败。
- **写校验**：`DS1302_SetTime()` 恒返回成功（没有写校验），因此改为**写后回读比对
  "当日分钟总数"**——既容忍写耗时造成的 2 分钟偏差，又能跨小时比较。

### 11.9 W25Q64 未焊接时的忙等待

`W25Q64_WaitBusy()` 原本是无上限忙等，芯片未焊接时程序直接卡死。

**处理**：加入 `W25Q64_BUSY_TIMEOUT` 超时计数，并提供 `W25Q64_GetBusyTimeoutCount()` 供排查。

> 注意 `W25Q64_BUSY_TIMEOUT = 0x08000000`（约 1.34 亿次轮询）是为**整片擦除**（20~100s）
> 留的余量，但页编程与扇区擦除也共用同一个常量——出异常时单次调用可能阻塞很久。

### 11.10 DS1302 读字节在 SCLK 上升沿之后采样

**现象**：进入夜间时串口稳定打出这两行，且每次进夜间都复现：

```
[LP] Sync RTC time fail
[LP] Enter night: LCD off, OLED on, time_source=SoftClock
```

**根因**：`DS1302_ReadByte()` 原本是"先把 SCLK 拉高，`delay_us(10)` 之后再读 IO"。

DS1302 只在时钟沿之后极短时间内保持输出，随后就**释放 IO（进入高阻）**；此时线电平
改由 MCU 内部约 40 kΩ 上拉决定，会升到高电平。等 10 µs 后去采样，8 位实际上全部
读成 1，即每次拿到 `0xFF`。

后果是固定且可预测的：`DS1302_ReadOnce()` 第一件事就是判 `buf[0] & 0x80`（CH 位），
读到 `0xFF` 当场返回 `false` → `DS1302_ReadTime()` 恒失败 → 回写校验打印 `fail`，
`RTC_ReadDataTime()` 回退软件时钟打印 `SoftClock`。**两条日志是同一个原因。**

**关键判据**：CH 是**锁存位而不是状态位**——只要通信正常，写进去的 CH=0 就必然读回 0，
时钟是否真的在走不会让读取失败。所以"写后回读也失败"可以**排除晶振停振**，
把矛头指向通信本身；而写入侧的时序是对的（数据在上升沿前已建立 10 µs），
因此问题只可能出在采样位置。

**处理**：把采样挪到上升沿**之前**。下降沿已经把本位数据准备好，整个 SCLK
低电平期间都是有效窗口，所以在低电平期间先读、再产生上升沿：

```c
if (DS1302_IO_Read())   /* 低电平期间先采样 */
    data |= 0x80U;
DS1302_CLK_HIGH();      /* 再产生上升沿 */
delay_us(10);
DS1302_CLK_LOW();
delay_us(10);
```

采样提前之后，两个 `delay_us` 之间的长间隔都在下降沿之后，反而让**任务抢占不再有影响**
（被更高优先级的 `dht22` 任务打断只会拉长 SCLK 周期，不会采错位）。

> 该缺陷与 11.3 的 `delay_us` 下溢是**两个独立问题**：11.3 会让半周期被压缩而采到旧位
> （表现为随机单一位错误），本节则是每次必错。修好其中一个不会掩盖另一个。

### 11.11 AT_Init() 偶发失败：等 ready 被别的 ACK 打断 + 假重试

**现象**：开机时偶发出现下面这组日志。注意"失败"之后紧跟"OK"：

```
[NET] Init Start
[NET] AT init FAILED, retry=1
[NET] AT init OK
[NET] WiFi init FAILED, retry=2
[NET] WiFi init OK
[NET] WiFi connecting to Jasmine ...
[NET] WiFi connect FAILED
```

从 `Init Start` 到 `WiFi connect FAILED` 只过了约 **120ms**——而 `AT_Init()` 内部光是
`AT_Wait_Boot(3000)` 就该耗 3s。**耗时这么短说明它不是"等超时"，而是在某一步被立刻判了失败。**

**根因有两层：**

1. **`AT_Wait_Ready()` 语义错误**。它把 `AT_Usart_Wait_Receive()` 的返回值直接和
   `AT_ACK_READY` 比较，而后者是"匹配到**任意**已知 ACK 就返回"。模组上电、或
   `AT+RESTORE` 重启途中先吐出的 `OK` / `ERROR` / `busy p...` 会让本函数**立刻**返回
   `false`，而不是继续等 `ready`。
2. **`Wireless_Init()` 的"重试"是假的**。旧写法是
   `if (!AT_Init() && retry < 5) { retry++; ... if (retry >= 5) ... }`——**没有循环**：
   `AT_Init()` 只被调用一次，`retry` 最多自增到 1，`retry >= 5` 永远不成立，
   于是失败后照样落到 `printf("[NET] AT init OK")` 并返回成功。
   `max retry reached` 是不可达的死代码。

后果比"多打一行日志"严重：`Net_Task` 拿到一个**假的 `wifi_ok = true`**，继续去发
`AT+CWJAP`，12ms 后连接失败，开机网络阶段整体降级；而周期重试只重连 WiFi，
**不会重新 `AT_Init()`**，模组可能长时间停在半初始化状态。

**处理**：

- `AT_Wait_Ready()` 改为显式循环：忽略过渡性的 `OK` / `ERROR` / `busy`，一直等到
  `ready` 或总超时。
- `AT_Send_Command()` 把 `busy p...` 当作"模组还没准备好"，在总超时内隔 100ms 重发；
  `ERROR` 与超时仍然立即失败。
- `AT_Send_Command()` 在**发送前**清一次 RX 环形缓冲（新增 `Usart1_RX_Flush()`）。
  否则上一条命令的尾巴、模组启动日志或 URC（如 `WIFI CONNECTED`）会被下一条命令
  当成自己的回复，造成"假成功"（读到旧 `OK`）或"假失败"。发送前清场不会丢本次回复——
  回复只可能在发送之后到达。
- `Wireless_Init()` 改成真正的重试循环（AT 与 WiFi 各 3 次，间隔 200ms），**失败就返回
  `false`**，并把模组原始回复一并打出来：
- **`AT+RESTORE` 从开机必经路径上摘下来**。`AT_Init()` 现在只做"外设初始化 + 反复发 `AT`
  直到拿到 `OK`"，不再每次开机恢复出厂；恢复出厂被抽成 `AT_Factory_Reset()`，
  由 `Wireless_Init()` 在**第一次初始化失败之后**才调用一次。

  这是很关键的一步：`AT+RESTORE` 会**擦掉模组保存的配置并强制重启**，重启期间模组先吐的
  是 `ready` / `busy p...` 而不是 `OK`——上面两个缺陷的触发窗口正是它制造的。平时开机不做
  恢复出厂，窗口就不存在；而模组配置真的坏了（或停在异常状态不应答）时，仍有恢复手段。
  代价是"第 1 次失败后多花约 2~7s"，只在异常路径上付出。

```
[NET] AT init FAILED, retry=1/3, rx len=<字节数>: <模组原始回复>
[NET] AT init FAILED, max retry reached
```

> **排障提示**：`rx len=0` 表示模组**完全没回话**（接线 / 供电 / 波特率问题）；
> `rx len` 非 0 时看内容——`busy p...` 是模组还没准备好（重试即可自愈），
> `ERROR` 是命令被拒，`ready` 说明 AT 命令的回复被启动横幅抢先了。
>
> 代价：AT 初始化失败一次的正常代价只是 `AT_Wait_Boot` 的 3s；第 1 次失败还会追加一次
> 恢复出厂（最多 2 + 5s）。最坏情况（模组完全不应答）3 次尝试合计约 **11~16s**，
> UI 侧等待 `EV_NET_READY` 的兜底是 30s，不会把开机流程拖死。

## 12. 已知限制与待办

### 12.1 正确性问题

| # | 问题 | 说明 |
| --- | --- | --- |
| 1 | **AO 模式 ADC 通道号错误** | `Light_Sensor.c:83,87` 用 `ADC_Channel_0`（= PA0），而光敏 AO 实际接 **PC0 = ADC123_IN10**。当前 `AO_DO_SWITCH = 0` 使该分支不编译，**切到 AO 模式前必须先改** |
| 2 | `Test_LED_GPIO_init()` 使用未初始化的结构体 | `Board.c:37` 声明 `GPIO_InitTypeDef GPIO_InitStructure;` 后，**裸机分支只赋值了 `GPIO_Pin` 与 `GPIO_Mode`**，`GPIO_OType` / `GPIO_PuPd` / `GPIO_Speed` 保持栈上垃圾值。FreeRTOS 分支赋值完整，所以当前未暴露 |
| 3 | `Parse_Weather_Response()` 不清零输出结构体 | 响应中缺失的字段会**保留调用方的旧值**（对比 `AT_Get_WiFi_Info()` 里有 `memset`） |
| 4 | 字符串渲染有两处换行判定相差 1 | `LCD.c:695` 用 `xpos + gw > WIDTH`，`LCD.c:706` 用 `(xpos - x) + gw > WIDTH - 1`，后者会让本行少放一个字符 |
| 5 | `Is_GB2312()` 依赖编译器的 `char` 符号性 | `LCD.c:592-595` 用 `char` 与 `0xA1~0xF7` 比较，ARMCC 默认 `char` 无符号才成立；换成 arm-none-eabi-gcc 会恒为 false，汉字全部无法定位 |
| 6 | `COLOR()` 宏缺最外层括号 | `LCD.h:39`，参与复杂表达式时可能因运算符优先级出错 |
| 7 | `WIDTH` / `HEIGHT` 是通用宏名 | `LCD.h:13-14`，与其它模块存在命名冲突风险 |
| 8 | `TIM5_ms` 跨上下文非原子 | 64 位 `volatile` 变量在 ISR 与任务间读写非原子；当前靠"重读 + UIF 补偿"规避，但并发写本身仍非原子 |
| 9 | FreeRTOS 分支下对未初始化的 PB2 写 ODR | `Board.c:56` 在 `USE_FREERTOS == 1` 时未初始化 PB2 却执行 `GPIO_ResetBits(GPIOB, GPIO_Pin_2)`（无实际作用，但语义混乱） |
| 10 | `Asset` 的图片掩码刚好只够 32 张 | `Asset.c:89` 用 `1UL << i` 标记，32 位掩码恰好容纳当前 32 张图片。**再加第 33 张会溢出串位** |
| 11 | 单缓冲回退依赖块大小巧合 | `LCD.c:368,378-380` 回退时 `buf[0] == buf[1] == s_scratch`，依赖 `blk = sizeof(s_scratch)/2` 恰好填满 23040 字节而不越界 |

### 12.2 无超时的等待点

以下位置都是无上限等待，硬件异常时会卡死（前两处已在 11.9 之外单独列出）：

| 位置 | 等待对象 |
| --- | --- |
| `W25Q64.c` 的 `W25Q64_RWByte()` 等 | SPI 的 TXE / RXNE，**SPI 未工作时死循环** |
| `LCD.c:216-225` / `257-271` | DMA 完成标志（只判 TE），且是纯忙等、不让出 CPU |
| `Light_Sensor.c:102-104` | AO 模式的 ADC EOC（仅 AO 分支编译） |
| `Usart.c:164-165` | DMA 流进入 DISABLE 状态 |

### 12.3 死代码与冗余

| # | 项 | 说明 |
| --- | --- | --- |
| 1 | `DS1302_ReadReg()` 未被引用 | `External_RTC.c:151-169`。**全量重编**时会产生 `warning #177-D`；增量编译不重编该文件时看不到——它是当前唯一的编译告警来源 |
| 2 | `OLED.c:25,37` 的空指针判断恒为假 | `if (&oled_i2c == NULL) return;`——取静态对象地址不可能是 `NULL` |
| 3 | `AT.h:59` 与 `AT.h:65` 重复声明 `AT_WiFi_Init` | — |
| 4 | 无调用者的 BSP 接口 | `W25Q64_IsBusy`、`W25Q64_Block32Erase`、`W25Q64_ChipErase`、`Soft_I2C_Receive_Bytes`、`Asset_MissingCount`、`Asset_CheckedCount` |
| 5 | 未被引用的资源描述符 | `Font_12`、`Font_22`、`Font_32`、`Font_32B`（`Font_32` 只被裸机用例使用，该目标文件被链接器丢弃）；`Image_location`、`Image_no_location` 已不参与界面绘制 |
| 6 | 未使用的宏 | `W25Q64_CMD_WRITE_DISABLE/READ_SR2/WRITE_SR/FAST_READ/BLOCK64_ERASE/POWER_DOWN`、`W25Q64_SR1_WEL/BP0~2/TB/SEC`、`W25Q64_SR2_QE`、`W25Q64_BLOCK_SIZE`；`External_RTC.h` 中除 `DS1302_REG_WP` 外的全部寄存器宏（源码里用的是裸字面量 `0xBE/0xBF/0x8E`） |
| 7 | `FreeRTOS/include` 下同时存在 `stack_macros.h` 与 `StackMacros.h` | 仅大小写不同，Windows 文件系统不区分大小写——跨平台检出时会有问题 |
| 8 | **`Image_Main_Page` 已烧录、已自检，但从不绘制** | 主界面底色实际由 `Main_Page_Top()` 的整屏 `ST7789_Fill_Color()` 加文字/图标构成，`Main_Page.c` 全文没有引用 `Image_Main_Page`。它仍占 **153600 B** Flash、参与 43 项开机自检，也是烧录批次 2（307200 B）的一半。**要么接到主界面上，要么从 `ImageTable.c` 与批次 2 中移除**（移除后自检项数 43 → 42，6.5/6.7 的数字要同步） |

### 12.4 工程与配置问题

| # | 问题 | 说明 |
| --- | --- | --- |
| 1 | `Documents/W25Q64资源迁移说明.md` 被引用但**不存在** | `Provision.h:20`、`BuildConfig.h:42` 两处引用；`Documents/` 下实际只有 `README.md` 与 7 个子目录 |
| 2 | `BuildConfig.h:44` 提到的 `Screen_Resource` 目录**不存在** | 资源备份实际在 `Resource/Src/Font`、`Resource/Src/Image` |
| 3 | 批次说明互相矛盾 | `Provision.h:19-20` 写"32 张图标"、`BuildConfig.h:41` 写"33 张图标"，**实际是 30 张图标**（32 项减去 2 张全屏图） |
| 4 | "资源总量约 822KB" 与自身数字不符 | 三批之和为 858650 字节（≈838.5 KiB），不是 822 KiB |
| 5 | `stm32f4xx_fmc.c` 未加入工程 | 磁盘上有该文件（62084 B），但不在任何分组内；同目录其余 42 个 `.c` 都在 |
| 6 | `MDK/Output/STM32F407.bin` 是陈旧产物 | 仅 1316 字节、时间戳 2025-03-07；因为 `CreateHexFile = 0` 且 `RunUserProg1 = 0`，`.bin` 不会自动刷新 |
| 7 | **CCM（64KB）完全未使用** | 只在 `Cpu` 串声明，自动生成的 sct 没有为它定义执行域 |
| 8 | **裸机 W25Q64 测试区与 littlefs 区域重叠** | 测试区在 `0x7FC000`（末 16KB），而 littlefs 覆盖整片 8MB、无预留分区。当前资源只占约 10%，靠用量低避开——**这是隐患而非分区保护**（见 8.1） |
| 9 | `KeilClear.bat` 不删 `*.bin` | 陈旧 `.bin` 会残留；同时它会删掉 `*.map`/`*.htm`，即尺寸证据 |
| 10 | `.gitignore` 未忽略 `Documents/` | 仓库不含 `.git` 共约 224MB，其中 `Documents/` 占 137MB（含 26MB 的烧录工具 exe、两份 10–11MB 的 AT 用户指南、两份 20–21MB 的参考手册） |
| 11 | `Image.h` 内部天气 code 注释前后不一致 | `gImage_*` 声明块给 `sleet`/`snow_flurry`/`light_snow`/`moderate_snow`/`heavy_snow`/`snowstorm` 标的是 14~19，`Image_t` 块标的是 20~25；`gImage_sleet` 注"雪"、`Image_sleet` 注"雨夹雪"。`Main_Page.c` 的 `weather_map` 与 `Image_t` 一侧一致 |
| 12 | `<InvalidFlash>1</InvalidFlash>` 残留在 `TargetStatus` 中 | `uvprojx` 的其余 4 项为 0 |

### 12.5 设计取舍（有意为之，非缺陷）

| # | 项 | 理由 |
| --- | --- | --- |
| 1 | 夜间不做 MCU 睡眠 | 见 11.1；夜间只做了模组侧省电（ESP32-C3 Modem-sleep） |
| 2 | 进入夜间可能等一个 AT 事务结束 | 刻意不打断 AT 事务，代价是暂停生效最多延迟约 10s |
| 3 | `Service_Room_Update()` 恒返回 `true` | DHT22 读取失败也会置 `EV_DHT22`，界面显示 `--`。若要区分成败需改返回值语义 |
| 4 | 凭据硬编码在 `App.c` | 见第 9 章；对外发布前必须替换 |
| 5 | 天气接口为第三方免费版 | 有调用频率限制，`key` 与配额需自行申请 |
| 6 | 依赖外网 | 无网络时天气保持默认（晴天图标 + 0.0）、白天时钟显示 `--:--`、WiFi 每 10s 重试 |
| 7 | 裸机测试项为编译期固定 | `BM_TEST_MODULE` 是宏，切换需改 `BuildConfig.h` 并重新编译 |
| 8 | 光敏 AO/DO 需重新编译切换 | `AO_DO_SWITCH` 是编译期宏，且 AO 阈值需按实际分压电路标定 |
| 9 | TIM5 中断（优先级 2）禁止调用 FreeRTOS API | 优先级数值小于 `configMAX_SYSCALL_INTERRUPT_PRIORITY`(5) |
| 10 | 昼夜切换 ACK 超时后不重试 | `App_Apply_Night()` 的 2s 等待超时后，光敏任务既不重试也不报错 |
| 11 | `s_boot` / `s_lowpower` 无临界区保护 | 仅靠事件位的先后关系保证可见性；目前读写点分离，未出问题 |
| 12 | 字库与图片完全依赖 W25Q64 | MCU 内不含任何字模与像素数据；芯片损坏或未烧录即完全无字可显，靠串口日志 + 纯色块兜底 |
| 13 | 资源烧录需要三轮编译烧录 | 资源总量超过 512KB Flash，无法单次带入 |
| 14 | 整屏底图绘制耗时（原先按带宽估算约 58ms） | 双缓冲已把 W25Q64 读取藏在 DMA 发送之后，但 SPI3 @21MHz 发送 153600 字节本身就是这个量级。**58ms 是按 0.381µs/字节的理论估算**；现由开机时的 `[PROF] full-screen image ...` 实测（见 4.11），**以实测值为准** |
| 15 | 字模读取附加 littlefs 查找开销 | 单字模纯传输约 27µs（22 号汉字）/ 56µs（48 号 ASCII），实际还要叠加 `lfs_file_seek` 的 CTZ skip-list 遍历。若实测偏高可加 4KB 块缓存 |
| 16 | `printf` 不含 `'\n'` 会长期持锁 | `Usart.c` 的整行互斥在遇到换行并发送完毕后才释放；同任务的后续 `printf` 可重入，其他任务会阻塞到 `portMAX_DELAY`。**写日志时必须带换行** |
| 17 | OLED 不支持中文 | `OLED_WriteChar` 把索引限制在可见 ASCII；夜间界面因此只画数字与日期 |

### 12.6 代码注释与事实不符之处

改动相关代码时注意，**以头文件与配置为准**：

| 位置 | 注释内容 | 实际情况 |
| --- | --- | --- |
| `W25Q64.c:5-8` | 称芯片"支持模式 1 和模式 3（模式1: CPOL=0, CPHA=1）" | 实际配置 **Mode 0**；W25Q64 只支持 Mode 0 与 Mode 3。**代码对、注释错** |
| `W25Q64.c:101-103` | 称"保守延时"对应 tRES1（典型 3µs） | 实际只是 100 次空循环，远小于 3µs |
| `Light_Sensor.h` 顶部 | 光敏引脚写作 `PA0/PA1` | 实际 AO = PC0、DO = PC1 |
| `main.c:36` | 称 `App_Task_Init()`"创建网络/传感器/UI任务并启动调度器" | 它**只创建 `ui` 一个任务**，调度器由下一行 `vTaskStartScheduler()` 启动 |
| `FreeRTOSConfig.h:45` 附近 | 称"原来的 95KB…下调到 88KB" | 文件内没有 95KB 的定义；`bare_test.c:285` 也仍写"95KB 堆"。**实际堆是 88KB** |
| `BuildConfig.h:42,44` | 引用不存在的文档与目录 | 见 12.4 第 1、2 条 |

## 13. 编码与维护约定

### 13.1 源码编码现状

检测范围：`User/`、`BSP/`、`Resource/`、`Core/`、`Third_Lib/` 下的全部 `.c` / `.h` / `.s`，
共 **104 个文件**。

| 分类 | 数量 | 说明 |
| --- | --- | --- |
| 纯 ASCII | 55 | 不含中文，与编码无关 |
| UTF-8 | 42 | — |
| **含非 UTF-8 字节** | **7** | 见下表 |
| 带 BOM | **0** | 全部无 BOM |
| 行尾混用 | **0** | 每个文件内部都是单一 EOL（CRLF **71** 个 / LF **33** 个） |

**7 个含非 UTF-8 字节的文件**

| 文件 | 非 UTF-8 行数 | 性质 |
| --- | --- | --- |
| `Core/stm32f4xx.h` | 3（L15 / L16 / L23） | 英文撇号损坏：`peripheral's` 的单引号在 GBK 下显示为 `抯`、在 UTF-8 容错下显示为 `�` |
| `Core/system_stm32f4xx.c` | 3（L351–L353） | 同类损坏，显示为 `?` / `�` |
| `Resource/Src/Font/Chinese_Font16.c` | 8（文件头注释） | 头部注释为 GB2312，数据体是纯 ASCII |
| `Resource/Src/Font/Chinese_Font22.c` | 8（文件头注释） | 同上 |
| `User/Src/main.c` | 11 / 76 | **整体 GB2312** |
| `User/Src/Main_Page.c` | 85 / 495 | **整体 GB2312**（含界面中文字符串） |
| `Resource/Src/Image/Image_Boot_Page.c` | 1（L3） | **主体是 UTF-8**，仅第 3 行有 2 个字节损坏 |

`Image_Boot_Page.c` 第 3 行的原始字节：

```
2F 2A 20   E5 BC 80   E6 9C BA   E9 A1 B5   E9 9D  3F   20  ...
 /  *  ␠     开          机          页        [缺]   ?    ␠
```

即 `面`（`E9 9D A2`）与 `接`（`E6 8E A5`）的**末字节被写成了 ASCII `?`（0x3F）**。
按语义应还原为 `/* 开机页面 未连接 等待连接 */`。该行会让严格 UTF-8 解码报非法序列，
编辑器打开时可能提示编码错误，但**不影响编译**。

**项目自身代码中的 LF 文件**（其余为 CRLF；第三方库中 FreeRTOS 的 4 个头文件与 LittleFS 的
5 个文件也是 LF）：

```
User/Inc    app_task.h · bare_test.h · BuildConfig.h · Provision.h
User/Src    App.c · app_task.c · bare_test.c · Boot_Page.c · main.c
BSP/Inc     Asset.h · External_RTC.h · Profiling.h · Timer.h · Usart.h
BSP/Src     Asset.c · DHT22.c · LCD.c · Light_Sensor.c · Profiling.c · Timer.c · Usart.c
Resource    Inc/Font.h · Inc/Image.h · Src/Image/ImageTable.c
```

**自查方法**：用 UTF-8 打开若报 encoding 错误，即为 GB2312/GBK 或含损坏字节。
`git diff` 出现整文件重写通常也是编码不一致导致的。

> ⚠️ **不要用"另存为 UTF-8"批量转换**。`Resource/Src/Font/Font_22.c` 的 `.name` 字符串
> 就是这样被写坏的，`Image_Boot_Page.c` 第 3 行的损坏很可能也是同类操作留下的。

### 13.2 中文界面字符串必须用 GB2312 写入

屏幕上显示的中文以 **GB2312 双字节**直接写入源码（如 `Main_Page.c` 的
`weather_map[].chinese` 里的 `"晴"`、`"多云"`）。`ST7789_Write_String()` 通过 `Is_GB2312()`
判定双字节，再按 `(区码-0xB0)*94 + (位码-0xA1)` 计算字模偏移。

**新增中文界面文字时必须保持 GB2312 编码写入**，否则字模定位失败、汉字显示为空白。

（OLED 不使用汉字，见 4.5。）

### 13.3 字库与图片格式约定

**字库**

- 汉字点阵集中在 `/font/cn16.bin`（16×16，32 字节/字）与 `/font/cn22.bin`（22×22，66 字节/字），
  均为 **3755 个 GB2312 一级汉字**，顺序严格等于区位码顺序（啊…座）。
- 定位公式 `offset = ((区码-0xB0) × 94 + (位码-0xA1)) × 每字字节数`，**O(1)**。
- 点阵格式：**横向取模、行优先**，每行 `(宽+7)/8` 字节，**每字节低位 bit0 对应最左侧像素**。
  **重新生成字库时必须沿用这套取模设置**，否则整份数据错位。

**图片**

- 文件格式：**4 字节大端头（宽、高）+ RGB565 小端像素**，与 `SaveImage()` /
  `Provision_SaveImage()` 的写入格式一致，`Asset` 自检按此核对文件大小。
- 新增图片后必须同步更新 `Resource/Inc/Image.h` 的 `Image_t` 定义与
  `Resource/Src/Image/ImageTable.c` 的 `g_AllImages[]`——**注意当前掩码只够 32 张**（见 12.1 第 11 条）。

**资源备份**：`Resource/Src` 下的资源数据仍完整保留，仅被
`#if (RESOURCE_DATA_IN_ROM == 1) && (PROVISION_BATCH == N)` 排除在正式固件之外。
需要重新烧录 W25Q64 时改 `BuildConfig.h` 即可，无需改动这些文件。
注意 `ImageTable.c`、`Boot_Page.c`、`Main_Page.c` **没有**该守卫，正式固件照常编译。

### 13.4 修改代码后需要同步的章节

| 改动内容 | 需同步的章节 |
| --- | --- |
| 引脚或外设配置 | 2.2、2.3、第 4 章对应小节 |
| 任务、优先级、栈 | 3.1 |
| 事件位 | 3.2 |
| `FreeRTOSConfig.h` | 3.5、3.6 |
| 周期 / 阈值宏 | 3.1、4.6、5.2 |
| 昼夜切换逻辑 | 5.5、6.7 |
| 资源文件增删 | 6.3、6.7、13.3 |
| 构建开关或工程分组 | 7.3、7.4、第 8 章 |
| 资源占用数值 | 7.6（以最新 `MDK/build.log` 为准） |
| 修复 12 章中的条目 | 从表中移除 |

### 13.5 本 README

UTF-8（无 BOM）、LF 行尾，可安全用任意 Markdown 工具渲染。
文档与源码不一致时**以源码为准**，并请顺手修正文档。

---

## 14. 参考资料索引

`Documents/` 目录下按器件分类存放了全部数据手册与工具（合计约 137MB）：

| 目录 | 内容 |
| --- | --- |
| `Documents/README.md` | 本文档 |
| `DS1302/` | DS1302 中/英文数据手册、实时时钟模块原理图（4 个 PDF） |
| `ESP32-C3/` | ESP32-C3-MINI-1 AT 固件 v4.1.1.0（`esp-at.bin`、bootloader、分区表、factory 固件、烧录配置 `download.config` / `flasher_args.json`）、`flash_download_tool_3.9.11.exe`、AT 用户指南（中英各一份，各约 10MB）、Release Note、免责声明、模组图片、`AT固件下载.txt` |
| `STM32F4/` | STM32F407VET6 数据手册、STM32F4xx 参考手册（中文，两份）、STM32F407VE 核心板原理图 V5.5 |
| `W25Q64/` | W25Q64BV 英文数据手册、W25Q64 中文手册 |
| `屏幕/` | ST7789V 规格书 V1.3、中景园 0.96" OLED 驱动芯片手册与使用文档 |
| `温湿度计/` | DHT22 数据手册、AM2302 产品规格书（中文） |
| `中文字库/` | 汉字点阵源文件 `Chinese_16.txt` / `Chinese_22.txt`（PCtoLCD2002 输出）与 `GB2312一级字库3755个汉字.txt` |

根目录另有 `项目简历-STM32F407桌面天气时钟.md`（面向求职的项目材料，数据取自源码与本文档）。

---

*本文档基于仓库当前源码重新梳理生成；`MDK/build.log`、`MDK/Output/STM32F407.map` 中的数据为
最近一次构建（`Code=52324 RO-data=3748 RW-data=412 ZI-data=126740`，0 Error 0 Warning）。
项目持续演进时，请以 `User/`、`BSP/` 源码与 `MDK/STM32F407.uvprojx` 为准。*
