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
| 固件规模 | `Code 137276 · RO-data 20068 · RW-data 1004 · ZI-data 128532`（最近一次构建，含 LVGL） |
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
资源层      BSP/Asset.c   littlefs 挂载与自检、字库/图片只读通道、运行数据(/data)键值存取
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
├── BSP/                        板级驱动（Inc 16 个头 / Src 16 个源，见 2.3）
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
| `Board.c` | 板级初始化：时钟、GPIO、各外设时钟使能、测试 LED、资源层（`Asset_Init`） |
| `App.c` | 服务层：WiFi / SNTP / 天气 / 温湿度 / 软件时钟；共享数据的整块赋值 |
| `app_task.c` | 任务装配层：只负责创建 `ui` 任务；任务实现见下行 |
| `Page.h` + `Boot_Page.c` / `Main_Page.c` | 界面绘制与页面布局常量（`Page.h` 是界面的公共头）；主页面与开机页是**裸绘**的 |
| `UI_Mode.c/.h` + `UI_Day.c/.h` + `UI_Night.c/.h` + `UI_Input.c/.h` | 界面层：昼夜模式门面（切换时序与主循环节拍）、白天 LCD 主页面与顶部条、夜间 OLED 时钟、LVGL 输入界面（含触摸校准与左上角热区） |
| `ui.c` / `ui_events.c` + `screens/ui_WiFi.c` / `screens/ui_location.c` | SquareLine 生成的 LVGL 输入界面（配网 / 定位）与事件实现；只有这两屏交给 LVGL 渲染（见 4.3、4.4） |
| `ui_helpers.c` | SquareLine 的控件辅助函数；**保留工程裁剪版**——新版会引用本工程未启用的控件导致链接失败 |
| `Provision.c` | 资源烧录：把源码内备份的字库/图片写入 W25Q64 |
| `bare_test.c` | 裸机模块测试用例（W25Q64 / littlefs / DS1302 / 光敏 / OLED / GNSS） |
| `stm32f4xx_it.c` | 中断桩 |

**任务层（`Task/Src`）**

| 文件 | 职责 |
| --- | --- |
| `Task_Common.c/.h` | 共享底座：事件组 `Task_Events()` 的创建与获取、低功耗标志 `g_lowpower` |
| `Task_Config.h` | 任务参数集中配置：优先级、栈大小、各周期、去抖与按键时序 |
| `UI_Task.c/.h` | `ui` 任务入口：板级初始化、开机画面与结果页、事件等待与分派；昼夜切换的 ACK |
| `Net_Task.c/.h` | `net` 任务：AT/WiFi/SNTP/天气，对外只暴露启动与开机结果读取 |
| `DHT22_Task.c/.h` | `dht22` 任务：单总线温湿度周期采集 |
| `LightSensor_Task.c/.h` | `light_sensor` 任务：光照去抖与昼夜状态机；对外暴露"请求切换昼夜" |
| `Key_Task.c/.h` | `key` 任务：PA0 手势状态机；长按调用光敏的请求接口，不直接改其状态 |

**板级驱动（`BSP/Src`）**

| 文件 | 职责 |
| --- | --- |
| `Asset.c` | 资源层：挂载 littlefs、开机自检、字库/图片只读通道（O(1) 字模 + 图片分块流式读）、运行数据键值存取 |
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

**图形库（`Middlewares/Lvgl`，LVGL v8.3.11）**

只用于 WiFi 键盘界面（见 4.3），主页面不受影响。

| 文件 | 职责 |
| --- | --- |
| `lv_conf.h` | 配置：16 位色、24KB 内存池（放在 CCM）、只开键盘/文本框/默认主题用到的组件 |
| `porting/lv_port_disp.c` | 显示端口：把 LVGL 渲染好的 RGB565 区域交给 `LCD_Draw_Pixels()`；绘制缓冲借用 LCD 的整行缓冲 |
| `porting/lv_port_indev.c` | 输入端口：`Touch_GetPoint()` 的坐标 → LVGL 指针设备 |

### 2.4 关键约定

1. **LCD 只由 `ui` 任务写**（`UI_Day` / `UI_Input` 都在该任务内）。其他任务只更新数据并置事件位，避免多任务同时操作 SPI3 造成撕裂。
2. **USART1（AT）只由 `Net_Task` 使用**。AT 是"请求—等待"模式，第二个使用者必然导致串扰。
3. **共享数据整块替换**。`wifi_info`、`weather_info`、`room_info` 与软件时钟都在临界区内
   整体赋值，消费者读不到半写状态。
4. **中断只做通知**，去抖、手势、显示等耗时逻辑一律下沉到任务。
5. **头文件的类型定义归它所属的模块**：AT 解析出的结构体在 `AT.h`，界面布局类型在 `Page.h`。
6. **任务之间只通过接口通信**：跨任务请求走各任务头文件里的函数（如
   `LightSensor_Task_RequestToggleNight()`），除 `Task_Common` 外不共享全局变量。
7. **外部 Flash 只由 `Asset.c` 一个模块访问**。字库与图片烧录后**只读**（常开字库句柄 +
   O(1) 定位），运行数据只能写到 `/data/<name>.bin`（名字由该层拼接，越不出 `/data`）；
   所有访问共用同一次挂载与同一把 `LittleFS_Lock()`。

---

## 3. 任务与并发模型

### 3.1 任务一览

| 任务 | 优先级 | 栈 | 职责 | 循环节拍 |
| --- | --- | --- | --- | --- |
| `UI_Task` / `"ui"` | 3 | 6KB | `Task_Common_Init()` 建事件组、`Board_Init()`、开机流程、**LCD 唯一写者**；昼夜切换经 `UI_Mode` 执行、**LVGL 键盘界面由 `UI_Input` 承载**（`lv_timer_handler()` 与触摸读取都在这个任务里） | 白天 50ms（触摸轮询）/ 夜间 2000ms / 键盘界面 10ms |
| `DHT22_Task` / `"dht22"` | 4 | 2KB | 单总线温湿度采集，成功后置 `EV_DHT22` | 5min |
| `Net_Task` / `"net"` | 2 | 4KB | **独占 USART1/AT**：联网、SNTP、天气；1s 心跳推进三个周期计数器 | 1s |
| `LightSensor_Task` / `"light_sensor"` | 2 | 2KB | 光照判定、2s 去抖、发起昼夜切换并等 ACK | 事件驱动 + 1s 兜底 |
| `Key_Task` / `"key"` | 2 | 4KB | PA0 手势状态机（单击/双击/三击/长按） | 事件驱动 |

**创建时机本身构成依赖关系**：`main()` 只创建 `ui`；`UI_Task` 里先建 `dht22` 与 `net`，
等 `EV_NET_READY`（≤60s）并显示结果页后，才创建 `light_sensor` 与 `key`——
这样**昼夜切换只可能发生在开机流程之后**。这个上限给得宽是有原因的：AT 初始化重试 +
`CWJAP`（最长 10s）+ SNTP（最长 15s）+ 天气 HTTP，没配置凭据时还要等模组自己重连（最多 8s），
30s 在慢网下会提前放弃，于是拿一份空结果去画结果页——显示成 Disconnect 而模组其实已连上。

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
| bit7 | `EV_NET_UPDATE_NOW` | `UI_Mode` → `Net_Task` | 退出夜间后立即补网络更新 |
| bit8 | `EV_DHT22_UPDATE_NOW` | `UI_Mode` → `DHT22_Task` | 退出夜间后立即补温湿度采集 |
| bit9 | `EV_KEYBOARD` | `UI_Input`（触摸判定） → `UI_Input` | 点了 WiFi 图标，调出配网界面 |
| bit10 | `EV_MAIN_SCREEN` | 输入界面回调 → `UI_Input` | 确认/取消，退回主界面 |
| bit11 | `EV_WIFI_CRED` | `UI_Input`（界面回调） → `Net_Task` | 提交了新的 WiFi 凭据，请重连 |
| bit12 | `EV_LOCATION_KB` | `UI_Input`（触摸判定） → `UI_Input` | 点了定位图标，调出定位输入界面 |
| bit13 | `EV_LOCATION_CRED` | `UI_Input`（界面回调） → `Net_Task` | 提交了新的定位信息，立即按新定位取天气 |

> **bit7 与 bit8 必须分开**。`xEventGroupWaitBits` 是清除式等待，两个任务共用一个位时，
> 先被唤醒的会把位清掉，另一个永远收不到通知。

### 3.3 同步手段

| 机制 | 用途 |
| --- | --- |
| 事件组 `Task_Events()` | 上表 12 个事件位，任务间主要通信方式（`Task_Common.c` 持有句柄，不暴露变量） |
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

天气定位（Service_Weather_Update）：
  定位界面输入的省+市（优先）──取不到/没输入──▶ 公网 IP ──查不到──▶ 字面量 ip（服务端按来源 IP）

DHT22 ──▶ DHT22_ReadData ──▶ room_info ──▶ 室内温湿度卡片

光敏 EXTI1 ──通知──▶ LightSensor_Task（2s 去抖）──▶ 事件组 ──▶ UI_Mode 切屏
按键 EXTI0 ──通知──▶ Key_Task（手势识别）──▶ 单击请求 ──▶ LightSensor_Task（统一裁决）

夜间的三个时间点：
  进入夜间前  软件时钟 ──▶ DS1302_SetTime()（写后回读校验）
  夜间        DS1302_ReadTime() ──成功──▶ OLED 显示；失败则回退软件时钟
  退出夜间    UI_Mode（ui 任务）──▶ EV_NET_UPDATE_NOW / EV_DHT22_UPDATE_NOW ──▶ 立即补更

资源（字库/图片，全部来自 W25Q64）：
  W25Q64 ──SPI1 21MHz──▶ Asset 层 ──┬─▶ 字模：区位码直接算偏移，O(1)
                                    └─▶ 图片：分块流式读取 + DMA1_Stream5 乒乓 ──SPI3──▶ ST7789
  Asset 层 ──▶ littlefs ──▶ W25Q64（运行数据 /data/<name>.bin 的键值式存取；字库与图片只读）
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
2. `UI_Mode`（由 `ui` 任务调用）是唯一执行者：关彩屏与背光、只留 OLED、冻结网络与温湿度周期计数、
   让 ESP32-C3 进入 Modem-sleep；
3. 执行完置 `EV_LOWPOWER_ACK`，`LightSensor_Task` 收到后才允许下一次判定；
4. 退出夜间时先唤醒模组，再用 **bit7 / bit8 两个独立事件位**通知两个任务立即补更。

按键手势也走同一入口——`Key_Task` 只调用 `LightSensor_Task_RequestToggleNight()` 发出请求，
最终由 `LightSensor_Task` 统一裁决（单击/三击尚未分配）。

### 4.3 输入界面（LVGL）：WiFi 配网与定位

主页面（时钟/日期/天气/室内温湿度）仍是**裸绘**的，LVGL 只接管"点了左上角图标之后"的输入界面：

| 主页图标 | LVGL 屏幕 | 输入内容 | 事件位 | 存到 |
| --- | --- | --- | --- | --- |
| WiFi 图标 `(5,5)` 20×20 | `ui_WiFi` | SSID + 密码 | `EV_KEYBOARD` | `/data/wifi_cfg.bin` |
| 定位图标 `(5,26)` 20×20 | `ui_location` | 省+市（一行文本） | `EV_LOCATION_KB` | `/data/location.bin` |

两者共用同一块 SPI 屏，因此必须严格错开：

1. 白天主循环按 `UI_TOUCH_POLL_MS`（50ms）轮询 `UI_Input_PollTouch()`，按上表的坐标判定点到哪个
   图标，命中就置对应事件位；轮询节拍必须明显小于一次点击的持续时间（约 100~200ms），
   否则整段点击会落在两次采样之间，表现成"点了没反应"；
2. `UI_Input_HandleEvent()` 收到后调 `UI_Input_Open(屏幕)`：首次进入时 `lv_init()` →
   `lv_port_disp_init()` → `lv_port_indev_init()` → `ui_init()`，之后每次只是
   `lv_disp_load_scr(目标屏幕)` + 复位输入状态 + 强制整屏重绘；
3. 输入界面激活期间主循环改成 **10ms** 心跳，每拍喂一次 `lv_tick_inc()` 并调
   `lv_timer_handler()`（LVGL 的触摸读取与刷屏都发生在这一句里），主页面的局部刷新全部跳过；
4. 界面上的"确认"把输入交给 `WiFi_Config_Set()` / `Location_Config_Set()`（只改 RAM，
   临界区内整体替换）。WiFi 凭据还会置 `EV_WIFI_CRED`，`Net_Task` 收到后立即用新凭据
   `AT_Connect_WiFi()` 重连，无需等 10min 的周期检查。输入为空时**不提交也不退出**，把标签
   改成红色提示留在原地（静默退回主页会让人以为确认键坏了）。软键盘的"✓"把 `READY` 发给
   键盘对象、"回车"发给输入框，所以两处都接了同一个提交函数；
5. `UI_Input_HandleEvent()` 处理 `EV_MAIN_SCREEN` 时关闭界面、把改动过的内容落盘、再整屏重画主页面。
   **退出界面本身不触发网络补更**，只有"确实提交了新内容"才通知 netTask：WiFi 凭据走
   `EV_WIFI_CRED`（重连 + 补 SNTP/天气），定位信息走 `EV_LOCATION_CRED`（只补一次天气）。
   按 X 取消则本来就不该有网络动作；`EV_NET_UPDATE_NOW` 专指"退出夜间立即补更"，
   借来当"回主界面顺便刷一下"会让 netTask 打出 LowPower 相关日志、语义对不上。

**主页面顶部状态条**（一行式，由 `UI_Day.c` 的 `UI_TopBar_Draw()` 绘制）：

```
[WiFi][定位]                    [WiFi名,城市名]   ← 图标 (5,5) / (26,5)；信息右对齐到 x=235
                    时钟从 y=46 开始
```

- 左上角并排两个 20×20 图标：WiFi 在 `(5,5)`、定位在 `(26,5)`，与触摸热区
  （`UI_Input.c` 的 `TOUCH_X_WIFI` / `TOUCH_X_LOC`）一一对应；
- 右上角一条信息，格式 `[WiFi名,城市名]`，**两个名字各自最多 10 字符**（整条最长 23 字符
  = 184 像素，右对齐到 x=235，左边不会压到定位图标），超长由 `snprintf` 截断；宽度按
  ASCII 字符算（16 号字单字符宽 = `Font_16B.size / 2`），**中文暂不支持**——城市名是中文时
  文字会偏左，但不会越出屏幕；
- 图标取自 W25Q64 里的 `wifi` / `nowifi` / `loc` / `noloc` 图，未连接或未设置定位时用
  对应的 off 版，信息里相应字段显示 `--`；
- `Main_Page.c` 里的旧 `Main_Page_Net_Update()` 仍是"双行"画法（该文件是 GB2312，不便改），
  每次 `Main_Page_Display()` 之后由 `UI_TopBar_Draw()` 清屏重画成一行覆盖掉，视觉上只有一行。

**定位信息的下游用途**：它不只是画在状态条上 —— `Service_Weather_Update()` 的定位优先级是
"**用户输入 > 网络 IP**"：先用输入的省+市取一次天气；取不到（响应里没有 `results`，多为城市名
写错）或从未输入过，就回退到网络 IP（先查公网 IP 用 `location=<IP>`，查不到再用字面量 `ip`，
由服务端按请求来源 IP 定位）。输入内容按 UTF-8 逐字节做百分号编码后再拼进 URL，所以中文、
空格都能安全传参。

**几个必须守住的点**（改动时容易踩）：

| 点 | 原因 |
| --- | --- |
| **`LCD_Init()` 之前必须先 `Touch_Park()`** | SPI3 由 LCD 初始化，那一刻触摸 CS(PE0) 还是复位后的浮空输入；XPT2046 会把 LCD 的初始化时钟当成自己的命令收进去，状态机错位后**只有掉电才能恢复**——表现就是"Keil 下载完触摸失灵、拔电源才正常"（下载只复位 STM32，触摸控制器一直有电） |
| LVGL 只在 `uiTask` 里初始化与驱动 | 刷屏走 LCD 的 SPI3 通路，LCD 的"唯一写者"约定不能破 |
| LVGL 的绘制缓冲借用 LCD 的整行缓冲（240×48） | 主 SRAM 只剩约 2KB，另开 10 行缓冲（4800B）直接爆内存；缓冲必须在 SRAM，CCM 里的内存 DMA 访问不到 |
| LVGL 的 24KB 内存池放在 CCM（`LV_MEM_ADR = 0x10000000`） | 不占主 SRAM；`Board.c` 里显式使能了 CCM 时钟 |
| 输入框必须带 `LV_OBJ_FLAG_CLICK_FOCUSABLE` | 否则点击不产生 `LV_EVENT_FOCUSED`，键盘永远不会显示 |
| 每次进入界面前 `lv_indev_reset(NULL, NULL)` | 上一轮残留的按下/焦点状态会让这次的 `LV_EVENT_FOCUSED` 不再发出 |
| 两个屏幕都只建一次、反复复用 | 每次 `ui_init()` 都会再分配一个主题与整套控件，24KB 堆会被逐步吃光 |
| 落盘放在 `UI_Input_HandleEvent()` 里而不是 LVGL 事件回调里 | littlefs 写入要几十 ms，放在回调里会卡住界面交互 |
| `ui_helpers.c` 保留工程裁剪版，不要直接用 SquareLine 的新版 | 新版带arc/dropdown/img/slider/spinbox 等辅助函数，而本工程为省 Flash 关掉了这些控件，会链接失败（见 4.4） |

### 4.4 用 SquareLine 重新导出界面时的注意事项

界面由 SquareLine Studio 1.6.2 生成（LVGL 8.3.11），导出目录结构与工程略有差异，
重新导出后**不要直接整体覆盖**，按下面处理：

| 导出内容 | 处理方式 |
| --- | --- |
| `ui.c` / `ui.h` | 直接覆盖（`ui.h` 里的 `#include "screens/xxx.h"` 与工程的 `User/UI/Inc/screens/` 对应） |
| `screens/ui_WiFi.c` / `ui_location.c` | 覆盖到 `User/UI/Src/screens/`，但要把 `#include "../ui.h"` 改成 `#include "ui.h"`，并**重新打上手工补丁**（见下） |
| `screens/*.h` | 直接覆盖到 `User/UI/Inc/screens/` |
| `ui_helpers.c` | **不要覆盖**，保留工程版（原因见上表最后一行） |
| `ui_helpers.h` | 可覆盖（多出的声明只要不被调用就没有影响） |
| `ui_events.c` | 覆盖后要**补回 `Get_WiFiInfo()` / `Get_LocationInfo()` 的实现**（导出的是空壳） |
| `components/ui_comp_hook.c` | 空文件，工程不需要 |

屏幕源文件上需要重打的手工补丁（两处屏幕一样）：

1. 输入框加 `LV_OBJ_FLAG_CLICK_FOCUSABLE` + `lv_textarea_set_one_line()` + `lv_textarea_set_max_length()`，
   密码框再加 `lv_textarea_set_password_mode()`；
2. 两个回调里**删掉 `_ui_flag_modify(<屏幕>, LV_OBJ_FLAG_HIDDEN, ADD)`** —— 屏幕的进出由
   `uiTask` 统一处理（它还要重画主页面），在 LVGL 事件回调里隐藏正在处理事件的对象会出问题；
3. 输入框再补一个 `LV_EVENT_READY` 分支（软键盘"回车"把 READY 发给输入框而非键盘对象）。

**没有配置过凭据时**，开机阶段不去发 `AT+CWJAP`（空 SSID 必然失败，结果页就会一路显示
Disconnect，而模组其实可能已经用它自己 NVS 里记着的 AP 连上了）。这里改成只查询、最多等
8 秒，让结果页显示模组的**真实**状态；没配置过且没连上时显示 `[WiFi] Not set, tap icon`，
提示点左上角图标去配网。

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

### 5.1 资源层的读写接口（`Asset.c`）

资源层只有 `Asset.c` 一个模块，管两类数据，共用同一次挂载与同一把递归互斥量
`LittleFS_Lock()`——W25Q64 挂在 SPI1 上，一个任务擦 4KB 扇区要几十毫秒，
不串行化必然读写交错、数据出错。

**一、只读资源：字库与图片（烧录后只读）**

| 接口 | 用途 |
| --- | --- |
| `Asset_ReadFont(fid, off, buf, len)` | 取字模：句柄常开、位置记忆，偏移由区位码/ASCII 码直接算出，O(1) |
| `Asset_ImageOk(img)` | 该图片是否通过开机自检（存在且大小相符） |
| `Asset_ImageOpen` / `Asset_ImageRead` / `Asset_ImageClose` | 图片分块流式读取，配 LCD 的乒乓 DMA |
| `Asset_ImageInfo(path, &w, &h)` | 只读 4 字节文件头取宽高，不读像素 |

正式固件**没有**运行期写字库/图片的接口：资源写坏屏上就是花屏或白屏，要换资源请重新
烧录 Provision 固件（见 6.2）。

**二、运行数据：`/data/<name>.bin`**

| 接口 | 用途 |
| --- | --- |
| `Asset_SaveData(name, buf, len)` | 覆盖写（不存在则创建）；写时复制，中途掉电保留旧值 |
| `Asset_LoadData(name, buf, buf_size)` | 整块读出，返回实际字节数；放不下报 `ASSET_ERR_SPACE`，不会只读一半 |
| `Asset_DataSize` / `Asset_HasData` / `Asset_DeleteData` | 查大小 / 判存在 / 删除 |

名字只允许字母、数字、`_`、`-`（最长 32），路径由资源层拼成 `/data/<name>.bin`——
调用者**无法**越出 `/data` 写到字库或图片上，这是这层"安全"的关键一环：

```c
Asset_Init();                                         // 上电一次（Board_Init 已调用）
Asset_SaveData("calib", &cfg, sizeof(cfg));           // 存
int n = Asset_LoadData("calib", &cfg, sizeof(cfg));   // 取，返回实际字节数
```

**三、约定**

- 返回值统一为 `Asset_Err_t`：`0` 成功，负数为错误码（`ASSET_ERR_PARAM` / `ASSET_ERR_STATE` /
  `ASSET_ERR_RANGE` / `ASSET_ERR_NOENT` / `ASSET_ERR_SPACE` / `ASSET_ERR_IO` / `ASSET_ERR_FORMAT`），
  失败必打一行 `[ASSET]` 日志；`Asset_Init()` 另有一个 `ASSET_ERR_MISSING`——文件系统可用但
  字库/图片不全（此时运行数据仍能正常读写）；
- 挂载失败**绝不自动格式化**（那会清空全部已烧录资源），只提示重新烧录；`Asset_Init()` 可重复调用；
- 资源不可用时读接口安全返回失败，渲染层退化为填背景色，不会死机也不会重启；
- 每次读/写都持锁，SPI1 不会交错；一张全屏图是分多次读出来的（两次读之间会放锁），
  但资源烧录后只读，不存在"读到一半被改写"的窗口；
- 裸机测试 `BM_TEST_MODULE_LFS` 里的 T12~T18 覆盖运行数据存取、数据名校验与图片读取。

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

> 批量烧录走 Provision；正式固件**不提供运行期写字库/图片的接口**（写坏屏上就是花屏），
> 只有运行数据（配置、缓存、校准值）能在运行期用 `Asset_SaveData()` 存进 `/data/`（见 5.1）。

### 6.3 需要自行配置的参数

| 参数 | 位置 | 说明 |
| --- | --- | --- |
| WiFi 凭据 | `User/Inc/Secrets.h` 的 `WIFI_SSID` / `WIFI_PASSWORD` / `WIFI_MAC` | 要连接的 2.4GHz WiFi；`WIFI_MAC` 填 `NULL` 表示不绑定 BSSID |
| 心知天气 key | `User/Inc/Secrets.h` 的 `WEATHER_KEY` | `App.c` 里的天气 URL 与初值都由它拼出 |
| 天气定位 | 运行期从主页定位图标进入输入（省+市） | 优先用它取天气；没输入或取不到时回退网络 IP 定位 |
| `AMAP_KEY` | `User/Inc/Secrets.h` | 高德"Web服务"key（逆地理编码预留用） |
| `HSE_VALUE` | `Core/stm32f4xx.h` | 必须与实际晶振一致 |

上表前三项都集中在 `User/Inc/Secrets.h`，该文件已被根目录 `.gitignore` 忽略，仓库里只有模板
`User/Inc/Secrets.example.h`。新克隆的工程先复制一份再填写：

```bat
copy User\Inc\Secrets.example.h User\Inc\Secrets.h
```

源码（`App.c` / `AT.c`）里只引用宏定义，不要再写回明文凭据。

---

## 7. 编码与维护约定

- **源码编码**：UTF-8（无 BOM），每个文件内部行尾统一。例外：`User/Application/Src/main.c` 与
  `User/UI/Src/Main_Page.c` 是 **GB2312**——界面中文要直接送进 GB2312 字库，改动时不能用
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
| 设计取舍 | 用户输入的定位每次天气更新都先试一次：城市名写错时每轮多一次失败的 HTTP 请求（1h 一次，失败响应很快），换来"改对输入即生效" |
| 正确性 | `Asset` 的图片掩码只有 32 位：超过 32 张的图片仍参与自检，但 `Asset_ImageOk()` 对它们恒返回 false（代码里已挡住 `1UL << i` 的溢出） |
| 健壮性 | `W25Q64_RWByte()` 等 SPI 等待、`LCD.c` 的 DMA 完成等待都是**无超时忙等**，硬件异常会卡死 |
| 健壮性 | DHT22 的 5 字节全 0 时校验和恰好也是 0，能骗过校验返回"成功"（界面显示 `0.0`）；`DHT22_ReadData()` 已把全 0 判为 `all-zero` 失败并触发重试 |
| 健壮性 | `LCD_TouchTest()` 里 `Touch_Init()` 失败会 `for(;;);` 死循环，uiTask 从此冻结（触摸无反应、界面不再刷新）。日志停在 `[Touch] FAIL: ...`，之后不再有新行 |
| 健壮性 | `TIM5_ms` 是 64 位 `volatile`，ISR 与任务间读写非原子（当前靠重读 + UIF 补偿规避） |
| 冗余 | `Image_Main_Page`（153600 B）已烧录、参与开机自检，但界面从未绘制它 |
| 冗余 | `DS1302_ReadReg()` 未被引用；全量重编共 17 条告警，其余 16 条都来自 LVGL 自身源码（枚举混用、符号转换等），本项目代码零告警 |
| 容量 | 主 SRAM 已用到 `RW-data + ZI-data = 129536 / 131072` 字节，**只剩约 1.5KB**。LVGL 的绘制缓冲因此借用 LCD 的整行缓冲、24KB 内存池放进独立的 64KB CCM；再随手加静态缓冲会直接链接失败 |
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
