# STM32F407 桌面天气时钟

> 基于 **STM32F407VET6 + FreeRTOS** 的桌面天气时钟 / 室内环境监测终端。
> 白天用 2.4" SPI 彩屏显示时钟、日期、实况天气与室内温湿度；夜间自动切到 0.96" OLED 只显示时间，
> 同时冻结周期任务、让 ESP32-C3 进入 Modem-sleep。
> 时间基准是 **SNTP 校时 + TIM5 推算的软件时钟**，夜间改用 **DS1302 外部 RTC**，断网也能正常走时。

| 项目 | 内容 |
| --- | --- |
| 主控 | STM32F407VETx（Cortex-M4F @168MHz，512KB Flash，128KB SRAM + 64KB CCM） |
| 操作系统 | FreeRTOS V10.4.3 LTS Patch 3：5 个常驻应用任务 + 可选 monitor、tick 1kHz、heap_4、42KB 堆 |
| 文件系统 | littlefs v2.11，挂载在 W25Q64（8MB SPI Flash） |
| 联网 | ESP32-C3-MINI-1（AT 固件 v4.1.1.0），USART1 @115200 |
| 显示 | ST7789V 240×320 彩屏（SPI3 + DMA）/ SSD1306 128×64 OLED（软件 I2C） |
| 图形库 | LVGL 8.3.11，只用于 WiFi/定位输入界面 |
| 工具链 | Keil MDK + ARMCC V5.06 update 5（AC5）/ C99 |
| 外置资源 | 11 个字库 + 32 张图片 = 858778 B（≈839KB），全部在 W25Q64；MCU 内点阵/像素数据 **0 字节** |
| 调试输出 | 默认 `DEBUG_SUPPORT=0` 关闭；打开后走 USART2（PA2/PA3） |

---

## 1. 硬件平台

### 1.1 主控与时钟

| 项目 | 参数 |
| --- | --- |
| MCU | STM32F407VETx（Cortex-M4F，512KB Flash，128KB SRAM，另 64KB CCM） |
| 系统时钟 | 168MHz（HSE 25MHz，PLL_M=25 / PLL_N=336 / PLL_P=2） |
| 总线时钟 | AHB 168MHz · APB1 42MHz · APB2 84MHz |
| 中断分组 | `NVIC_PriorityGroup_4`（4 位抢占、0 位子优先级） |

### 1.2 引脚分配

| 外设 | 信号 → 引脚 |
| --- | --- |
| ST7789 彩屏 | SCLK=PC10 · MISO=PC11 · MOSI=PC12（SPI3/AF6）· CS=PE2 · RESET=PE3 · DC=PE4 · BLK=PE5 |
| XPT2046 触摸 | 与 LCD 共用 SPI3，CS=PE0 · PENIRQ=PE1 |
| W25Q64 | CS=PA4 · CLK=PA5 · MISO=PA6 · MOSI=PA7（SPI1/AF5） |
| ESP32-C3 | TX=PA9 · RX=PA10（USART1/AF7） |
| 调试串口 | TX=PA2 · RX=PA3（USART2，`DEBUG_SUPPORT=1` 时启用） |
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
| USART2 | 115200-8N1，调试模式下 TX 走 DMA1_Stream6/Ch4，无中断 |
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
应用层      User/Application/      main / Board / App / app_task / Provision / bare_test / stm32f4xx_it
界面层      User/UI/               UI_Mode / UI_Day / UI_Night / UI_Input / Boot_Page / Main_Page / LVGL 屏幕
任务层      User/Task/             UI / Net / DHT22 / LightSensor / Key + Task_Common / Task_Config
资源层      BSP/Asset.c           littlefs 挂载与自检、字库/图片只读通道、运行数据(/data)键值存取
板级驱动    BSP/                  LCD · OLED · Touch · AT · DHT22 · External_RTC · Light_Sensor ·
                                  Key · Timer · Usart · W25Q64 · I2C · Profiling
操作系统    Middlewares/FreeRTOS/  内核 + portable/FreeRTOSConfig.h
文件系统    Middlewares/LittleFS/ littlefs v2.11 + lfs_port + lfs_operation
图形库      Middlewares/Lvgl/     LVGL 8.3.11 + lv_conf.h + porting
资源数据    User/Resource/        字库与图片的源码内备份（正式固件不编译）
硬件抽象    Core/ + SPL           CMSIS + STM32F4 标准外设库
```

依赖方向**自上而下单向**：应用层 → 界面/任务层 → 资源层/板级驱动 → FreeRTOS/littlefs → 硬件抽象。
BSP 层只操作硬件，不含业务逻辑，也不反向调用 `User/`。

### 2.2 目录结构

```text
Project4/
├── MDK/                         Keil 工程（STM32F407.uvprojx）
├── Core/                        CMSIS + 启动文件 + 系统时钟
├── STM32F4xx_StdPeriph_Driver/  STM32F4 标准外设库
├── Middlewares/
│   ├── FreeRTOS/                内核 + include/ + portable/
│   ├── LittleFS/                littlefs v2.11 + lfs_port + lfs_operation
│   └── Lvgl/                    LVGL 8.3.11 + lv_conf.h + porting/
├── BSP/
│   ├── Inc/                     Asset · LCD · Touch · OLED · AT · DHT22 · External_RTC ·
│   │                            Light_Sensor · Key · Timer · Usart · W25Q64 · I2C · Profiling
│   └── Src/                     对应 .c
├── User/
│   ├── Application/             main / Board / App / app_task / Provision / bare_test / stm32f4xx_it
│   ├── Task/                    UI_Task / Net_Task / DHT22_Task / LightSensor_Task / Key_Task +
│   │                            Task_Common / Task_Monitor / Task_Config
│   ├── UI/                      UI_Mode / UI_Day / UI_Night / UI_Input / Boot_Page / Main_Page /
│   │                            ui.c / ui_events.c / ui_helpers.c / screens/
│   └── Resource/                Font.h / Image.h + 字库/图片源码
└── Documents/                   本说明、器件手册、工具
```

### 2.3 模块职责

**应用层（`User/Application`）**

| 文件 | 职责 |
| --- | --- |
| `main.c` | 三种构建模式的唯一入口；断言与栈溢出钩子 |
| `Board.c` | 板级初始化：外设时钟、TIM5、USART2（调试模式）、触摸 CS park、LCD、Asset、OLED |
| `App.c` | 服务层：WiFi / SNTP / 天气 / 温湿度 / 软件时钟、凭据与定位存储 |
| `app_task.c` | 任务装配层：只在启动调度器前创建 `ui` 任务 |
| `Provision.c` | 资源烧录：把源码备份的字库/图片写入 W25Q64 |
| `bare_test.c` | 裸机模块测试：OLED / 光敏 / DS1302 / W25Q64 / littlefs / TOUCH |
| `stm32f4xx_it.c` | 异常与中断桩 |

**界面层（`User/UI`）**

| 文件 | 职责 |
| --- | --- |
| `UI_Mode.c/.h` | 昼夜模式门面：切换时序、主循环等待节拍、事件分派 |
| `UI_Day.c/.h` | 白天 LCD 主页面、顶部状态条、时钟/天气/温湿度局部刷新 |
| `UI_Night.c/.h` | 夜间 OLED、RTC 同步与分钟级刷新 |
| `UI_Input.c/.h` | 触摸校准、左上角图标热区、LVGL 配网/定位界面 |
| `Boot_Page.c` / `Main_Page.c` | 开机页与主页面裸绘；`Page.h` 保存布局常量 |
| `ui.c` / `ui_events.c` / `ui_helpers.c` / `screens/` | SquareLine 生成的 LVGL 界面与事件；`ui_helpers.c` 保留工程裁剪版 |

**任务层（`User/Task`）**

| 文件 | 职责 |
| --- | --- |
| `Task_Common.c/.h` | 事件组 `Task_Events()`、低功耗标志 `g_lowpower`、事件位 |
| `Task_Config.h` | 优先级、栈大小、SNTP/天气/WiFi/DHT22 周期、去抖与按键时序 |
| `UI_Task.c/.h` | `ui` 任务：开机流程、事件循环、LCD 唯一写者 |
| `Net_Task.c/.h` | `net` 任务：AT/WiFi/SNTP/天气，对外提供开机结果 |
| `DHT22_Task.c/.h` | `dht22` 任务：单总线温湿度周期采集 |
| `LightSensor_Task.c/.h` | `light_sensor` 任务：光照去抖、昼夜状态机、统一裁决切换请求 |
| `Key_Task.c/.h` | `key` 任务：PA0 手势状态机（单击/双击/三击/长按） |
| `Task_Monitor.c/.h` | 内存监测任务（可选）：周期打印堆用量与各任务栈峰值，开关见 `BuildConfig.h` 的 `MONITOR_TASK_ENABLE` |

**板级驱动（`BSP/Src`）**

| 文件 | 职责 |
| --- | --- |
| `Asset.c` | 资源层：挂载 littlefs、自检、字库/图片只读通道、运行数据键值存取 |
| `LCD.c` | ST7789：SPI3 + DMA、字模渲染、图片流式读取与乒乓双缓冲、总线锁 |
| `Touch.c` | XPT2046：原始/滤波坐标、五点校准、校准参数存取 |
| `OLED.c` | SSD1306：命令/数据、按页清屏、ASCII 夜钟显示 |
| `AT.c` | ESP32-C3 AT 指令、WiFi/SNTP/HTTP、天气/IP 响应解析 |
| `ATGM336H.c` | 北斗+GPS NMEA 解析；预留，未加入 Keil 工程 |
| `Utf8_Gb2312.c` | UTF-8 → GB2312 一级汉字转码；预留，未加入 Keil 工程 |
| `DHT22.c` | 单总线时序、校验和、全零数据识别、错误码 |
| `External_RTC.c` | DS1302 读写、突发传输、双读比对、写保护 |
| `Light_Sensor.c` | 光敏 DO（EXTI1）/ AO（ADC1 + 模拟看门狗） |
| `Key.c` | PA0 按键，EXTI0 双边沿上报，不做手势识别 |
| `Timer.c` | TIM5 1ms 基准、`TIM5_Get_us()`、`delay_us` / `delay_ms` |
| `Usart.c` | USART1 RX 环形缓冲；USART2 printf（仅调试模式，DMA + 整行互斥） |
| `W25Q64.c` | SPI Flash：读/页编程/扇区擦除/忙等待超时统计 |
| `I2C.c` / `Profiling.c` | 软件 I2C、DWT CYCCNT 计时 |

### 2.4 关键约定

1. **LCD 只由 `ui` 任务写**（`UI_Day` / `UI_Input` 都在该任务内）。其他任务只更新数据并置事件位。
2. **USART1（AT）只由 `Net_Task` 使用**。AT 是“请求—等待”模式，第二个使用者必然导致串扰。
3. **共享数据整块替换**：`wifi_info`、`weather_info`、`room_info`、软件时钟都在临界区内整体赋值。
4. **中断只做通知**：TIM5 只累加毫秒；USART1 只入环形缓冲；EXTI/ADC 只发任务通知。
5. **类型定义归所属模块**：AT 结构体在 `AT.h`，页面布局在 `Page.h`，任务参数在 `Task_Config.h`。
6. **任务之间只通过接口通信**：跨任务请求走各任务头文件函数，除 `Task_Common` 外不共享全局变量。
7. **外部 Flash 只由 `Asset.c` 访问**：字库/图片只读；运行数据只能写到 `/data/<name>.bin`。
8. **SPI3 和 littlefs 都有递归互斥量**：LCD/触摸共用 SPI3；W25Q64 的整棵文件树共用 `LittleFS_Lock()`。
9. **LVGL 只在 `ui` 任务内初始化与驱动**；`ui_helpers.c` 不要直接整体覆盖为 SquareLine 新版。

---

## 3. 任务与并发模型

### 3.1 任务一览

| 任务 | 优先级 | 栈 | 职责 | 循环节拍 |
| --- | --- | --- | --- | --- |
| `UI_Task` / `"ui"` | 3 | 2KB | 开机流程、**LCD 唯一写者**；昼夜切换经 `UI_Mode` 执行，LVGL 由 `UI_Input` 承载 | 白天 50ms（触摸轮询）/ 夜间 2000ms / 键盘界面 10ms |
| `DHT22_Task` / `"dht22"` | 4 | 1KB | 单总线温湿度采集，成功后置 `EV_DHT22` | 5min |
| `Net_Task` / `"net"` | 2 | 1KB | **独占 USART1/AT**：WiFi、SNTP、天气；1s 心跳推进三个周期计数器 | 1s |
| `LightSensor_Task` / `"light_sensor"` | 2 | 512B | 光照判定、2s 去抖、发起昼夜切换并等 ACK | 事件驱动 + 1s 兜底 |
| `Key_Task` / `"key"` | 2 | 512B | PA0 手势状态机（单击/双击/三击/长按） | 事件驱动 |
| `Monitor_Task` / `"monitor"` | 1 | 512B | 内存监测（可选，`MONITOR_TASK_ENABLE=1` 时创建）：堆用量与各任务栈峰值，夜间静默 | 5s / 夜间 60s |

**优先级不是随手定的**：`DHT22` 高于 `UI`，因为单总线对微秒级时序敏感；`net` 降到 2，
因为一次 AT 事务最长 10s，优先级过高会把显示饿死。`net` / `light_sensor` / `key` 同为 2，
且 `configUSE_TIME_SLICING = 0`（同级不轮转），因此三者都必须能主动阻塞。
`monitor` 固定为最低的 1：它只读任务快照并打印，任何实时任务都能抢占它。

### 3.2 事件位

统一定义在 `User/Task/Inc/Task_Common.h`，全部为 `1UL << n`：

### 3.3 同步手段

| 机制 | 用途 |
| --- | --- |
| 事件组 `Task_Events()` | 上表 14 个事件位，任务间主要通信方式 |
| 任务通知（计数语义） | ISR → 任务的光敏/按键边沿；按键“切换昼夜”请求 |
| 临界区 `taskENTER_CRITICAL()` | 软件时钟与共享结构体的整块赋值（`App.c`） |
| 互斥量 | 调试串口整行输出、LCD/触摸共用的 SPI3 递归锁、littlefs 文件树递归锁 |

### 3.4 数据流

```text
ESP32-C3 ──USART1──▶ AT.c ──▶ wifi_info ──▶ 顶部状态条 / 开机结果页
                       │
                       ├─▶ AT_SNTP_Get_Time ─▶ Clock_Sync ─▶ 软件时钟（TIM5 推算）
                       │                                      └─▶ 主界面 / OLED 时钟
                       ├─▶ AT_Get_IP ─▶ 公网 IP ─▶ 天气 URL 的 location=<IP>
                       └─▶ AT_Get_HTTP ─▶ Parse_Http_Response ─▶ weather_info ─▶ 天气卡片

天气定位（Service_Weather_Update）：
  定位界面输入的省+市（优先）──取不到/没输入──▶ 公网 IP ──查不到──▶ 字面量 ip

DHT22 ──▶ DHT22_ReadData ──▶ room_info ──▶ 室内温湿度卡片

光敏 EXTI1 ──通知──▶ LightSensor_Task（2s 去抖）──▶ 事件组 ──▶ UI_Mode 切屏
按键 EXTI0 ──通知──▶ Key_Task（手势识别）──▶ 长按请求 ──▶ LightSensor_Task（统一裁决）

夜间：
  进入夜间前  软件时钟 ──▶ DS1302_SetTime()（写后回读校验）
  夜间        DS1302_ReadTime() ──成功──▶ OLED 显示；失败则回退软件时钟
  退出夜间    UI_Mode ──▶ EV_NET_UPDATE_NOW / EV_DHT22_UPDATE_NOW ──▶ 立即补更

资源：
  W25Q64 ──SPI1 21MHz──▶ Asset 层 ──┬─▶ 字模：区位码直接算偏移，O(1)
                                    └─▶ 图片：分块流式读取 + DMA1_Stream5 乒乓 ──SPI3──▶ ST7789
```

---

## 4. 主要机制

### 4.1 时钟

- **白天**：SNTP 校时得到本地时间，`Clock_Sync()` 用民用历算法换算成时间戳，与当前
  `TIM5_Get_ms()` 一起保存在临界区内；之后靠 TIM5 自行推算，每 4h 再校一次。
- **夜间**：进入夜间前把软件时钟写入 DS1302（写后回读校验），夜间只读 DS1302，失败回退软件时钟。
  DS1302 读取连续两次比对，避免偶发位错误。
- **周期**：WiFi 状态检查 10min（失败重试 10s）· SNTP 4h（重试 5s）· 天气 1h（重试 60s）。
  掉线自动重连，重连成功后立即补一次 SNTP 与天气。

### 4.2 昼夜切换与低功耗

采用“**请求—执行—ACK**”握手，避免两个任务各自改状态：

1. `LightSensor_Task` 判定光照变化并去抖 2s 后置 `EV_LOWERPOWER` / `EV_WAKEUP`；
2. `UI_Mode`（由 `ui` 任务调用）是唯一执行者：
   - 进入夜间：收起输入界面 → 置低功耗标志 → 开 OLED、显示时间 → 关 LCD 和背光；
   - 退出夜间：关 OLED → 开 LCD → 整页重绘 → 清除低功耗标志；
3. 执行完置 `EV_LOWPOWER_ACK`，`LightSensor_Task` 收到后才允许下一次判定；
4. 退出夜间时用 **bit7 / bit8 两个独立事件位**通知网络和 DHT22 立即补更。

`net` 任务检测到低功耗状态变化后，让 ESP32-C3 在 Modem-sleep 和全速之间切换。
PA0 长按也走同一入口：`Key_Task` 只调用 `LightSensor_Task_RequestToggleNight()`，
由光敏任务统一裁决。

### 4.3 输入界面（LVGL）：WiFi 配网与定位

主页面（时钟/日期/天气/室内温湿度）仍是**裸绘**的，LVGL 只接管“点了左上角图标之后”的输入界面：

两者共用同一块 SPI 屏，因此必须严格错开：

1. 白天主循环按 `UI_TOUCH_POLL_MS`（50ms）轮询 `UI_Input_PollTouch()`，按 `UI_Input` 的热区表
   判定点到哪个图标，命中就置对应事件位；轮询节拍必须明显小于一次点击的持续时间（约 100~200ms）；
2. `UI_Input_HandleEvent()` 收到后调 `UI_Input_Open(屏幕)`：首次进入时 `lv_init()` →
   `lv_port_disp_init()` → `lv_port_indev_init()` → `ui_init()`，之后每次只是
   `lv_disp_load_scr(目标屏幕)` + 复位输入状态 + 强制整屏重绘；
3. 输入界面激活期间主循环改成 **10ms** 心跳，每拍喂一次 `lv_tick_inc()` 并调
   `lv_timer_handler()`；主页面的局部刷新全部跳过；
4. 界面上的“确认”把输入交给 `WiFi_Config_Set()` / `Location_Config_Set()`（只改 RAM）。
   WiFi 凭据还会置 `EV_WIFI_CRED`，`Net_Task` 收到后立即用新凭据 `AT_Connect_WiFi()` 重连；
   定位信息置 `EV_LOCATION_CRED`，立即按新定位取天气。输入为空时不提交也不退出，把标签改成
   红色提示留在原地；
5. `UI_Input_HandleEvent()` 处理 `EV_MAIN_SCREEN` 时关闭界面、把改动过的内容落盘、再由
   `UI_Mode` 重画主页面。按 X 取消不触发网络补更；只有确实提交了新内容才发对应事件。

**定位信息的下游用途**：`Service_Weather_Update()` 的定位优先级是“**用户输入 > 网络 IP**”：
先用输入的(省+)市取一次天气；取不到或从未输入过，就回退到网络 IP。输入内容按 UTF-8 逐字节百分号编码后拼进 URL。

### 4.4 用 SquareLine 重新导出界面时的注意事项

界面由 SquareLine Studio 1.6.2 生成（LVGL 8.3.11），重新导出后**不要直接整体覆盖**：

| 导出内容 | 处理方式 |
| --- | --- |
| `ui.c` / `ui.h` | 可覆盖；`ui.h` 里的 `#include "screens/xxx.h"` 与工程目录对应 |
| `screens/ui_WiFi.c` / `ui_location.c` | 覆盖到 `User/UI/Src/screens/`；把 `#include "../ui.h"` 改成 `#include "ui.h"`，并重打手工补丁 |
| `screens/*.h` | 覆盖到 `User/UI/Inc/screens/` |
| `ui_helpers.c` | **不要覆盖**，保留工程裁剪版 |
| `ui_helpers.h` | 可覆盖（未调用的声明不影响链接） |
| `ui_events.c` | 覆盖后补回 `Get_WiFiInfo()` / `Get_LocationInfo()` 实现（导出的是空壳） |
| `components/ui_comp_hook.c` | 空文件，工程不需要 |

屏幕源文件需要重打的手工补丁：

1. 输入框加 `LV_OBJ_FLAG_CLICK_FOCUSABLE` + `lv_textarea_set_one_line()` + `lv_textarea_set_max_length()`，
   密码框再加 `lv_textarea_set_password_mode()`；
2. 两个回调里删掉 `_ui_flag_modify(<屏幕>, LV_OBJ_FLAG_HIDDEN, ADD)`；
3. 输入框补 `LV_EVENT_READY` 分支（软键盘“回车”把 READY 发给输入框）。

### 4.5 显示与触摸

- 白天主页面由 `UI_Day` 调用 `Main_Page.c` 裸绘；LCD 为 240×320 RGB565，SPI3 + DMA。
- 夜间 `UI_Night` 使用 SSD1306 128×64 OLED，只显示 ASCII 时间和日期。
- LVGL 使用 24KB 内存池（CCM `0x10000000`），绘制缓冲借用 LCD 的 240×48 渲染缓冲；
  `LV_COLOR_16_SWAP=0`，flush 直接交给 `LCD_Draw_Pixels()`。
- 触摸为 XPT2046，五点校准后把 `kx/ky/bx/by` 写入 `/data/touch_cal.bin`；未校准时按原始坐标
  比例折算。`Board_Init()` 在 LCD 初始化前先调用一次 `Touch_Init()` 拉高 CS，防止 XPT2046
  误收 LCD 初始化命令；这次调用因 SPI3 尚未使能会返回 false，属于预期的 park 行为，
  LCD 就绪后再由 `UI_Input_Calibrate()` 成功初始化。

---

## 5. 资源存储：W25Q64 + littlefs

字模与图片不放在 MCU Flash 里，而是烧到 W25Q64 上由 littlefs 管理，MCU 内只保留描述表。

**字库（11 个，合计 420050 B）**：`/font/cn16.bin`（3755 个 16×16 汉字）、`cn22.bin`（22×22），
以及 `as12/16/16b/22/22b/32/32b/48/48b.bin` 九张 ASCII 表。

**图片（32 个，合计 438728 B）**：`boot.bin` / `main.bin` 两张全屏图（各 153604 B）、
`err.bin`、`wifi.bin` / `wifi_off.bin`、`loc.bin` / `noloc.bin`、`temp.bin` / `humi.bin` /
`thermo.bin`、22 张天气图标 `w_*.bin`。图片带 4 字节宽高头，格式为 RGB565 小端。

两项合计 **858778 字节**（占 8MB 的 10.2%），开机自检项数 = 11 + 32 = **43 项**。

**字模定位是 O(1) 的**，不查表：ASCII 用 `(ch - 0x20) * ob * size`，汉字用
`((区码-0xB0)*94 + (位码-0xA1)) * cn_bytes`。点阵为横向取模、行优先，每字节 bit0 对应最左像素。

---

## 9. 参考资料

`Documents/` 下按器件分类存放了数据手册与工具：

| 目录 | 内容 |
| --- | --- |
| `DS1302/` | DS1302 中/英文数据手册、模块原理图 |
| `ESP32-C3/` | AT 固件 v4.1.1.0、烧录工具与配置、AT 用户指南 |
| `STM32F4/` | STM32F407VET6 数据手册、参考手册、核心板原理图 |
| `W25Q64/` | W25Q64BV 数据手册（中/英） |
| `屏幕/` | ST7789V 规格书、SSD1306 OLED 手册、TSC2046 触摸手册 |
| `温湿度计/` | DHT22 / AM2302 数据手册与规格书 |
| `ATGM336H/` | ATGM336H-5N 模块手册 |
| `中文字库/` | 汉字点阵源文件与 GB2312 一级字库表 |
| `Tools/` | `gen_gb2312_table.py`（生成转码表）、`verify_utf8_gb2312.py`（校验） |

`MDK/Output/STM32F407.map` 是编译链接后生成的 map，可用于查看实际内存占用。
