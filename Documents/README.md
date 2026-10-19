# STM32F407 桌面天气时钟

基于 **STM32F407VET6 + FreeRTOS** 的桌面天气时钟 / 室内环境监测终端。

白天用 2.4" SPI 彩屏显示时间、日期、实况天气和室内温湿度；夜间自动切到 0.96" OLED
只显示时间，同时暂停周期任务、让 ESP32-C3 进入 Modem-sleep。时间基准是
**SNTP 校时 + TIM5 推算的软件时钟**，夜间改用 **DS1302 外部 RTC**，断网也能走时。

## 功能

- 时钟、日期、星期显示；白天彩屏，夜间 OLED。
- 心知天气实况；定位优先用用户输入的“省+市”，失败回退公网 IP。
- DHT22 采集室内温度、湿度。
- 光敏自动切换昼夜，PA0 长按可手动切换。
- 触摸屏配网和定位输入，参数保存到 W25Q64，掉电不丢。
- XPT2046 五点触摸校准，参数同样保存在 W25Q64。
- 字库和图片全部放外部 W25Q64，MCU 内不占资源空间。

## 快速开始

1. 用 Keil 打开 `MDK/STM32F407.uvprojx`。
2. 修改 `User/Application/Inc/Secrets.h` 中的 WiFi 和天气 key。
3. 首次使用先烧录资源：`RESOURCE_PROVISION=1`，按 `PROVISION_BATCH=1/2/3` 分三次烧录。
   建议同时把 `DEBUG_SUPPORT` 设为 1，否则看不到 `[PROV]` 进度。
4. 改回 `RESOURCE_PROVISION=0`，编译并烧录正式固件。
5. 上电后按提示完成触摸校准，从主页 WiFi/定位图标进入配网和定位输入。

默认 `DEBUG_SUPPORT=0`，正式固件不初始化 USART2，串口不会打印日志；需要调试时
把它改成 1，调试串口为 USART2（PA2=TX、PA3=RX），115200-8N1。

## 硬件平台

| 项目 | 内容 |
| --- | --- |
| 主控 | STM32F407VETx，Cortex-M4F @168 MHz，512 KB Flash，128 KB SRAM + 64 KB CCM |
| 系统时钟 | HSE 25 MHz，PLL 25/336/2；APB1 42 MHz，APB2 84 MHz |
| 操作系统 | FreeRTOS V10.4.3 LTS Patch 3，5 个应用任务，heap_4 42 KB |
| 文件系统 | littlefs v2.11，挂载在 W25Q64（8 MB SPI Flash） |
| 联网 | ESP32-C3-MINI-1，AT 固件 v4.1.1.0，USART1 @115200 |
| 显示 | ST7789V 240×320 彩屏（SPI3 + DMA）/ SSD1306 128×64 OLED（软件 I2C） |
| 图形库 | LVGL 8.3.11，只用于 WiFi/定位输入界面 |
| 工具链 | Keil MDK + ARMCC V5.06 update 5（AC5），C99 |
| 外置资源 | 11 个字库 + 32 张图片 = 858778 B，全部在 W25Q64 |

### 引脚分配

| 外设 | 引脚 |
| --- | --- |
| ST7789 彩屏 | SCLK=PC10、MISO=PC11、MOSI=PC12、CS=PE2、RESET=PE3、DC=PE4、BLK=PE5 |
| XPT2046 触摸 | 与 LCD 共用 SPI3，CS=PE0，PENIRQ=PE1 |
| W25Q64 | CS=PA4、CLK=PA5、MISO=PA6、MOSI=PA7（SPI1） |
| ESP32-C3 | TX=PA9、RX=PA10（USART1） |
| 调试串口 | TX=PA2、RX=PA3（USART2，`DEBUG_SUPPORT=1` 时启用） |
| OLED | SCL=PB6、SDA=PB7（软件 I2C） |
| DHT22 | DATA=PE6 |
| DS1302 | RST=PE7、IO=PE8、CLK=PE9 |
| 光敏电阻 | DO=PC1（EXTI1）、AO=PC0（ADC1_IN10） |
| 用户按键 | PA0（下拉，按下为高，EXTI0） |
| 测试 LED | PC5 / PB2（相关代码当前已注释，未使用） |

### 外设要点

- SPI1 和 SPI3 都是 21 MHz；SPI3 在访问触摸时临时降到约 656 kHz。
- LCD 和触摸共用 SPI3，用递归互斥量保护总线。
- USART1 用于 AT，带 512 B 接收环形缓冲；USART2 只在调试模式下启用；USART3 预留未启用。
- TIM5 提供 1 ms 时基，NVIC 抢占优先级为 2，ISR 内不能调用 FreeRTOS API。

## 软件结构

```text
Project4/
├── MDK/                         Keil 工程
├── Core/                        CMSIS、启动文件、系统时钟
├── STM32F4xx_StdPeriph_Driver/  ST 标准外设库
├── Middlewares/
│   ├── FreeRTOS/                内核 + FreeRTOSConfig.h
│   ├── LittleFS/                littlefs + W25Q64 移植
│   └── Lvgl/                    LVGL + lv_conf.h + porting
├── BSP/                         板级驱动
├── User/
│   ├── Application/             main、Board、App、Provision、裸机测试
│   ├── Task/                    UI / Net / DHT22 / LightSensor / Key
│   ├── UI/                      页面、昼夜模式、LVGL 输入界面
│   └── Resource/                字库/图片描述与烧录数据
└── Documents/                   手册、工具、文档
```

> 任务层在 `User/Task/`，不是项目根的 `Task/`；旧文档里的 `Third_Lib/`、`User/Src/`
> 等路径已经不存在。

### 任务

| 任务 | 优先级 | 栈 | 职责 |
| --- | --- | --- | --- |
| `ui` | 3 | 2 KB | 开机流程、LCD 唯一写者、昼夜模式切换、事件循环 |
| `dht22` | 4 | 1 KB | 每 5 分钟采集 DHT22 温湿度 |
| `net` | 2 | 1 KB | 独占 USART1，负责 WiFi、SNTP、天气 |
| `light_sensor` | 2 | 512 B | 光照去抖、昼夜状态机、统一裁决切换请求 |
| `key` | 2 | 512 B | PA0 手势识别（单击/双击/三击/长按） |

`main()` 只创建 `ui`；`ui` 在开机流程中创建 `dht22` 和 `net`，等网络阶段结束后再创建
`light_sensor` 和 `key`。任务间通过 `Task_Common.h` 中的 14 个事件位通信，ISR 只发任务通知。

> `Task_Monitor.c` 源码仍在磁盘，但已从 Keil 工程移除，`UI_Task` 也不再启动它；
> 当前固件不会打印内存监测报告。`Task_Config.h` 里的 monitor 参数暂时未被使用。

### UI 模块

`UI_Task` 只负责开机流程和事件循环，具体界面拆成四个模块：

| 模块 | 职责 |
| --- | --- |
| `UI_Mode` | 昼夜模式门面：切换时序、主循环等待节拍、事件分派 |
| `UI_Day` | 白天 LCD 主页面、顶部状态条、时钟/天气/温湿度局部刷新 |
| `UI_Night` | 夜间 OLED、RTC 同步与分钟级刷新 |
| `UI_Input` | 触摸校准、左上角图标热区、LVGL 配网/定位界面 |

事件位中与输入界面相关的是 `EV_WIFI_KB`、`EV_LOCATION_KB`、`EV_MAIN_SCREEN`；
旧代码里的 `EV_KEYBOARD` 已改名为 `EV_WIFI_KB`。

重新用 SquareLine 导出界面时，保留工程裁剪版 `ui_helpers.c`，并补回
`Get_WiFiInfo()` / `Get_LocationInfo()` 和输入框的 `CLICK_FOCUSABLE` 等手工补丁。

## 主要机制

### 时钟

白天通过 ESP32-C3 的 SNTP 获取本地时间，`Clock_Sync()` 把时间换算成时间戳并与
`TIM5_Get_ms()` 一起保存；之后由 TIM5 推算，每 4 小时重新校时。

进入夜间前把软件时钟写入 DS1302 并回读校验；夜间优先读 DS1302，失败时回退软件时钟。
DS1302 读取采用连续两次比对，避免偶发位错误。

### 昼夜切换与低功耗

光敏任务检测光照变化并去抖 2 秒，然后向 `ui` 任务发请求；`ui` 通过 `UI_Mode` 执行：

- 进入夜间：收起输入界面 → 置低功耗标志 → 开 OLED、显示时间 → 关 LCD 和背光；
- 退出夜间：关 OLED → 开 LCD → 整页重绘 → 清除低功耗标志 → 通知网络和 DHT22 立即补更。

`net` 任务检测到低功耗状态变化后，让 ESP32-C3 在 Modem-sleep 和全速之间切换。
PA0 长按通过光敏任务统一请求切换。

### 网络与天气

`net` 任务独占 USART1。开机阶段依次初始化 AT、连接 WiFi、配置 SNTP、同步时间、
获取天气；完成后置 `EV_NET_READY`，UI 显示开机结果页。

运行期间：

- WiFi 状态每 10 分钟检查一次，失败 10 秒后重试；
- SNTP 每 4 小时同步一次；
- 天气每 1 小时更新一次；
- 夜间冻结这些周期计数，退出夜间后立即补更；
- 配网界面提交新凭据后立即重连，定位界面提交后立即按新定位取天气。

天气定位优先级：用户输入的“省+市” > 公网 IP > 服务端按来源 IP 定位。
WiFi 凭据、定位信息和触摸校准分别保存在 `/data/wifi_cfg.bin`、
`/data/location.bin`、`/data/touch_cal.bin`。

### 显示与触摸

主页面由 `UI_Day` 调用 `Main_Page.c` 裸绘：

- 顶部：WiFi/定位图标 + `[WiFi名,城市名]`；
- 中部：48 号字体时钟 + 日期；
- 下部：天气卡片和室内温湿度卡片。

LVGL 只接管点击左上角图标后的输入界面，使用 24 KB 内存池（放在 CCM），
绘制缓冲借用 LCD 的 240×48 渲染缓冲，避免额外占用主 SRAM。

触摸使用 XPT2046，五点校准后把 `kx/ky/bx/by` 写入 W25Q64；未校准时按原始坐标
比例折算，仍可操作。`Board_Init()` 在 LCD 初始化前先调用一次 `Touch_Init()` 拉高
触摸 CS，防止 XPT2046 误收 LCD 初始化命令；这次调用因为 SPI3 尚未使能会返回 false，
属于预期的 park 行为，LCD 就绪后再由 `UI_Input_Calibrate()` 成功初始化。

### 资源与文件系统

字库和图片烧录到 W25Q64 的 littlefs 中，正式固件只保留描述表，不编译数据本体。
资源层 `Asset.c` 是外部 Flash 的唯一入口：

- 字库按 GB2312 区位码或 ASCII 码直接算偏移，O(1) 读取；
- 图片按 4 字节宽高头 + RGB565 像素流式读取，配 LCD 乒乓 DMA；
- 运行数据只能写到 `/data/<name>.bin`，路径由资源层拼接；
- 挂载失败不会自动格式化，避免清空已烧录资源。

资源总量约 839 KB，超过 512 KB Flash，必须分三批烧录：字库 → 全屏图 → ASCII 表和图标。

## 构建与烧录

`BuildConfig.h` 决定构建模式：

| 模式 | 宏 | 用途 |
| --- | --- | --- |
| 正式固件 | `USE_FREERTOS=1`、`RESOURCE_PROVISION=0`、`DEBUG_SUPPORT=0` | 日常运行 |
| 调试固件 | `USE_FREERTOS=1`、`DEBUG_SUPPORT=1` | 需要串口日志时 |
| 资源烧录 | `RESOURCE_PROVISION=1`、`PROVISION_BATCH=1..3` | 首次写 W25Q64 |
| 裸机测试 | `USE_FREERTOS=0`、`BM_TEST_MODULE=...` | 硬件/驱动排查（见已知限制） |

资源烧录顺序：

| 批次 | 内容 | 大小 |
| --- | --- | --- |
| 1 | 汉字库 16 + 22 | 367990 B |
| 2 | 开机全屏图 + 主页全屏图 | 307208 B |
| 3 | 9 张 ASCII 表 + 30 张图标 | 183580 B |

批次 1 会格式化文件系统，所以必须按 1 → 2 → 3 烧录；重跑批次 1 会清掉后两批。
`Provision.c` 的进度全部走 `DBG_PRINTF`，默认 `DEBUG_SUPPORT=0` 时看不到日志。
打开 `DEBUG_SUPPORT=1` 后还会链接 printf、USART2 DMA 和调试互斥量，代码体积会增大；
具体尺寸以重新编译后的 `Program Size` 为准。

### 凭据配置

实际凭据文件是 `User/Application/Inc/Secrets.h`，需要配置：

- `WIFI_SSID` / `WIFI_PASSWORD` / `WIFI_MAC`
- `WEATHER_KEY`（心知天气）
- `AMAP_KEY`（预留，当前未使用）

`Secrets.h` 已被根目录 `.gitignore` 忽略，不会提交。仓库里只有这一份文件，
没有 `Secrets.example.h`；新环境需要按这些宏名自行创建或填写。

源码编码以 UTF-8 为主；`main.c`、`Main_Page.c`、`system_stm32f4xx.c` 和
`Chinese_Font*.c` 为 GB2312，改动时不要整体转成 UTF-8。

## 已知限制

| 问题 | 说明 |
| --- | --- |
| 默认没有串口日志 | `DEBUG_SUPPORT=0` 时 `DBG_PRINTF` 为空、USART2 不初始化；烧录资源或跑裸机测试时先改成 1。 |
| 裸机模式当前会链接失败 | `main.c` 的裸机分支仍调用 `Test()`，但 `Board.c` 里的 `Test()` 已注释；需要恢复 LED 测试函数或删掉该调用。 |
| 监控任务未编译 | `Task_Monitor.c` 仍在磁盘，但不在 Keil 工程里，`UI_Task` 也不调用；内存报告不会输出。 |
| 中文定位显示空白 | LVGL 输入是 UTF-8，主页面字库按 GB2312 查表；`Utf8_Gb2312.c` 未加入工程。临时可用英文城市名。 |
| 顶部条可能残留第二行 | `Main_Page_Net_Update()` 仍在 y=28 画定位文本，`UI_TopBar_Draw()` 只清 y=5~24。 |
| 光敏 AO 通道号错误 | 代码用 `ADC_Channel_0`，实际 PC0 应为 `ADC_Channel_10`；当前使用 DO 模式，不受影响。 |
| 部分等待无超时 | LCD SPI/DMA、W25Q64 字节读写是忙等，硬件异常时可能卡死。 |
| 资源注释过时 | 部分注释仍写 822 KB、183460、33 张图标，实际为 858778 B、183580、32 张图。 |

## 文档

- 本文档：项目总览、结构、任务、UI 模块、机制、构建和限制。
- `MDK/STM32F407.uvprojx`：Keil 工程。
- `MDK/Output/STM32F407.map`：编译链接后生成的 map，可用于查看实际内存占用。
- 器件手册按类别放在 `Documents/` 下：STM32F4、W25Q64、DS1302、ESP32-C3、
  屏幕、温湿度计、ATGM336H、中文字库和 Tools。
