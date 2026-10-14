# WiFi 键盘配网：问题与修复总结

> 范围：LVGL 软键盘输入 WiFi 名称/密码（键盘界面 + 配网链路 + 开机结果页）。
> 记录本轮排查发现的**全部问题**、根因与处理状态，便于后续维护和回归。
> 结论先行：原始上报的 2 个现象共 7 条根因，另有 14 条连带缺陷被发现并修复；
> 5 条既有问题有意保留；4 项需在硬件上实测确认。

---

## 一、原始现象与根因

### 现象 1：没输入过 WiFi，开机结果页显示 Disconnected，之后又自己连上

| # | 根因 | 说明 |
| --- | --- | --- |
| 1.1 | `g_ssid` / `g_password` 是 `char *` 且初值为 `NULL` | `AT_Connect_WiFi()` 见到 `NULL` 直接返回失败，结果页据此判成"断开" |
| 1.2 | 判定口径错位 | 结果页反映的只是"本次 `AT+CWJAP` 有没有成功"，而不是模组状态。ESP32-C3 把上次成功的 AP 存在**模组自己的 NVS** 里，且 `AT+CWAUTOCONN` 默认开启，上电会自动重连；netTask 随后的周期查询读到 `connected=1` 才是真实状态 |
| 1.3 | 连带：`GetInfo()` 对空指针做 `strncpy`，且 `sizeof(指针)` 等于 4 | 键盘就算能弹出来，一按确认就会写空指针（HardFault）；即使不崩，凭据也只能拷进 3 个字节 |

**处理**：凭据改为定长数组（SSID 33 B / 密码 65 B），存取集中到 `WiFi_Config_Set/Save/Load`；
没有凭据时**不再发空 SSID 的 `CWJAP`**，改成只查询模组状态并最多等 8 秒，让结果页显示真实状态；
没配置过又没连上时显示 `[WiFi] Not set, tap icon`（提示点左上角图标配网），而不是笼统的 Disconnect。

### 现象 2：点 WiFi 图标后键盘调不出来

四层原因叠加，任一层单独存在都出不来：

| # | 根因 | 严重度 |
| --- | --- | --- |
| 2.1 | 全工程没有 `lv_init()` / `lv_port_disp_init()` / `lv_port_indev_init()` / `lv_timer_handler()` 调用，两个 porting 文件还是官方 `#if 0` 模板。`ui_init()` 里 `lv_disp_get_default()` 返回 `NULL`，紧接着的主题初始化与载屏都在空指针上操作 | 致命，多半直接跑飞 |
| 2.2 | `lv_textarea` 默认**不带** `LV_OBJ_FLAG_CLICK_FOCUSABLE`，点击输入框不产生 `LV_EVENT_FOCUSED`；而"显示键盘"正挂在这个事件上，键盘又初始化成 `HIDDEN` | 键盘永远不显示 |
| 2.3 | 主循环 500 ms 才轮询一次触摸，而一次点击只有 100~200 ms，大概率整段落在两次采样之间被漏掉 | 点了没反应 |
| 2.4 | 判定区域按 `(0,0)` 起算，与实际图标 `(5,5)` 起 20×20 错位 | 点图标右下半边无效 |

**处理**：写出真实的显示/输入端口并在 `uiTask` 中集成（见第四节）；
输入框补 `CLICK_FOCUSABLE`；进入键盘时直接显示键盘并聚焦 SSID 框（不再依赖"先点输入框"）；
触摸轮询节拍改为 50 ms；判定区域与图标对齐。

---

## 二、实现过程中发现的连带缺陷

### A. 会导致崩溃或功能失效

| # | 问题 | 影响 | 状态 |
| --- | --- | --- | --- |
| A1 | `GetInfo()` 写空指针 + `sizeof` 截断（同 1.3） | HardFault | 已修 |
| A2 | 每次点图标都调 `ui_init()` | 重复创建主题与整套控件，LVGL 的 24 KB 堆逐步耗尽 | 已修：屏幕只建一次，之后复用 |
| A3 | 在 LVGL 事件回调里 `_ui_screen_change()` / 删屏 | 在处理事件的过程中删掉正在处理的对象，会崩 | 已修：回调只置事件位，切换由 `uiTask` 统一做 |
| A4 | 主 SRAM 只剩约 2 KB | 按常规写法给 LVGL 单独开绘制缓冲（10 行 = 4800 B）直接链接失败 | 已修：绘制缓冲借用 LCD 的 240×48 渲染缓冲；24 KB 内存池放 CCM |

### B. 状态机与交互

| # | 问题 | 影响 | 状态 |
| --- | --- | --- | --- |
| B1 | `EV_MAIN_SCREEN` 没进 `xEventGroupWaitBits` 的等待掩码 | `pdTRUE` 只清掩码内的位，该位永远清不掉，主界面之后每轮循环都在整屏重绘 | 已修：加入掩码 |
| B2 | 退出键盘后 LVGL 内部仍认为当前屏是键盘界面 | 再次进入不重绘，主界面裸绘内容被覆盖成花屏 | 已修：每次进入强制整屏 `invalidate` |
| B3 | 主界面右上角的裸触摸区域也置 `EV_MAIN_SCREEN` | 误点一次 = 整屏重绘 + WiFi/SNTP/天气补更风暴 | 已修：主界面不再判那块（那是键盘界面上 X 的位置） |
| B4 | `EV_MAIN_SCREEN` 无条件补更：同时置 `EV_NET_UPDATE_NOW` 与 `EV_WEATHER` | 取消输入也会触发 SNTP（最长 15 s）与天气 HTTP；且 UI 代 netTask 置"天气已更新"语义错误 | 已修：不再代置 `EV_WEATHER`；补更先收窄为"仅在从键盘退回时"，后彻底去掉（见 B6） |
| B5 | 无凭据时每 10 秒发一次空 SSID 的 `AT+CWJAP` | 必然失败，还会打断模组自己的自动重连 | 已修：无凭据时只查询不上报重连 |
| B6 | 退出键盘时误置 `EV_NET_UPDATE_NOW`——该位专指"退出夜间立即补更"（由 `UI_Task_EnterDay()` 与 `Key_Task` 的唤醒路径使用） | 在键盘上按 X 取消后，串口仍打出 `[NET] LowPower exit, reset update`，并立刻做一次 WiFi 查询 + SNTP + 天气 HTTP：日志与实际原因对不上，网络动作也是白做 | 已修：退出键盘不再置任何补更位。提交了凭据的场景已由 `EV_WIFI_CRED` 覆盖（重连 + 补 SNTP/天气）；按 X 取消则本来就不该有网络动作 |
| B7 | 两处叠加导致"触摸完全失灵"：① `LCD_TouchTest()` 无条件把 `calibrated` 清零做强制校准（调试期遗留）；② LVGL 端口里"未校准就不上报按下"（为修 M6 的误触而加） | 每次上电都必须重新校准；一旦本次没点完（超时/漏点）或 Flash 里没有校准数据，`calibrated` 恒为 `false` → **LVGL 侧触摸全部被上报成抬起，主界面点 WiFi 图标也没反应**，整屏点哪都不动 | 已修：改为按需校准（读到 `/data/touch_cal.bin` 就直接用，读不到才校准，成功后写回 Flash）；未校准时改为按原始坐标等比折算而不是屏蔽触摸；新增 `TOUCH_FORCE_CALIBRATE` 开关（`BuildConfig.h`）供换屏后强制重校 |

### C. 容量与并发

| # | 问题 | 影响 | 状态 |
| --- | --- | --- | --- |
| C1 | LVGL 内存池放在 CCM（`0x10000000`）但从未使能 CCM 时钟 | 池一旦被访问就是 HardFault（`CCMDATARAMEN` 复位值为 1，只是没显式写） | 已修：`Board.c` 显式使能 |
| C2 | `g_ssid` / `g_password` 由 uiTask 写、netTask 读，无保护 | 理论上有撕裂窗口 | 已修：写侧在 `taskENTER_CRITICAL()` 内整体替换（单核下临界区屏蔽任务切换，读侧无需再包一层） |
| C3 | UI 任务栈 4 KB | LVGL 渲染与按键处理都在该任务里，栈偏紧 | 已调整为 6 KB（1536 words），仍建议实测水位 |

---

## 三、代码审查发现的问题与处理

| # | 问题 | 判定 | 处理 |
| --- | --- | --- | --- |
| R1 | 开机只等 `EV_NET_READY` 30 s，且不检查返回值 | 成立，且新增的 8 s 等待让它更容易踩到——超时后拿全 0 的结果画页面，**又回到"状态不一致"** | 上限提到 60 s，检查返回值，超时打印告警 |
| R2 | `s_lvgl_tick_ms` 只在初始化时赋值，二次进入未重置 | 成立：隔几小时再进键盘会一次 `lv_tick_inc(巨大值)`，把所有 LVGL 定时器推到超期（动画跳变、光标闪烁错乱） | 进入键盘时重新对表 |
| R3 | 首次进入依赖 `ui_init()` 内部那句 `lv_disp_load_scr` | 成立：SquareLine 重新生成就可能改掉，然后停在别的屏 | 改为无条件 `lv_disp_load_scr(ui_keyboard)` |
| R4 | 触摸未校准时报原始坐标（0~4095），钳位后成为恒定 (239,319) | 成立：正好落在键盘右下角回车/O K 附近，可能凭空提交 | 未校准时直接上报 `RELEASED` |
| R5 | 空 SSID 提交后静默退回主页 | 成立：用户会以为确认键坏了 | `GetInfo()` 改为返回 `bool`；为空时不提交、不退出，`SSID` 标签变红显示 `SSID required`，下次进入自动复位 |
| R6 | 回车键无反应 | 成立：`✓` 把 `READY` 发给键盘对象，回车发给输入框，原来只接了键盘 | 抽出统一提交函数，键盘对象与两个输入框都接上 |
| R7 | `LCD_Draw_Pixels()` 未整段持锁（窗口命令与像素数据之间有锁空档） | 成立，当前无实际后果但与文件约定不符 | 外层持锁包住 `SetWindow` + `Write_Gram`（递归互斥量，嵌套安全） |
| R8 | `s_scratch` 长期借给 LVGL，与 `LCD_Write_String()` 的互斥只靠分支顺序 | 成立 | 增加 `s_scratch_busy` 标志，撞上就打印告警（真并发必然花屏，有日志好查） |
| R9 | `lv_disp_drv_register()` 返回值未检查 | 成立：CCM 池耗尽会静默失败（`LV_ASSERT_HANDLER` 是 `while(1)`） | 已修：判空，失败时打印明确报错（排查用的内存池监控打印后来已移除） |
| R10 | `Task_Config.h` 注释"net 与 UI 同级(2)"与实际（UI=3、net=2）不符 | 成立 | 注释改正 |

> **未采纳 R11（`g_ssid` 跨任务读写建议加双缓冲）**：审查读到的是加保护之前的版本。
> 现在写侧已在临界区内整体替换，单核下临界区屏蔽任务切换，netTask 读不到半写状态，
> 加双缓冲属于过度设计。

---

## 四、目标架构（改完之后的样子）

主页面（时钟/日期/天气/室内温湿度）**仍是裸绘**，LVGL 只接管键盘界面，两者共用一块 SPI 屏、严格错开：

1. 白天主循环按 `UI_TOUCH_POLL_MS`（50 ms）轮询触摸，命中 WiFi 图标区域即置 `EV_KEYBOARD`；
2. `UI_Task` 收到后：首次进入执行 `lv_init()` → `lv_port_disp_init()` → `lv_port_indev_init()` →
   `ui_init()`，之后每次只重新载屏 + 复位输入状态 + 强制整屏重绘；
3. 键盘界面激活期间主循环改成 **10 ms** 心跳，每拍 `lv_tick_inc()` + `lv_timer_handler()`
   （LVGL 的触摸读取与刷屏都在这一句里），主页面的局部刷新一律跳过；
4. 提交（`✓` 或回车）→ `WiFi_Config_Set()`（只改 RAM，临界区内整体替换）并置 `EV_WIFI_CRED`；
   `Net_Task` 收到立即用新凭据重连，不等 10 min 周期；
5. `EV_MAIN_SCREEN` → 关闭键盘界面 → 凭据落盘 `/data/wifi_cfg.bin` → 整屏重画主页面。

**几条不能破的约束**：

| 约束 | 原因 |
| --- | --- |
| LVGL 只在 `uiTask` 里初始化与驱动 | 刷屏走 LCD 的 SPI3 通路，LCD 的"唯一写者"约定不能破 |
| LVGL 绘制缓冲借用 LCD 的整行缓冲 | 主 SRAM 静态余量只有约 1.5 KB；且缓冲必须在 SRAM——CCM 里的内存 DMA 访问不到 |
| LVGL 内存池放 CCM 且显式使能 CCM 时钟 | 24 KB 不占主 SRAM |
| 输入框必须带 `CLICK_FOCUSABLE` | 否则点击不产生 `LV_EVENT_FOCUSED`，键盘永远不显示 |
| 每次进入键盘前 `lv_indev_reset(NULL, NULL)` | 上一轮残留的按下/焦点状态会让这次的 `FOCUSED` 不再发出 |
| 键盘屏幕只建一次、反复复用 | 每次重建都会再分配一套主题与控件，24 KB 堆会被吃光 |
| 凭据落盘放在 `UI_Task` 而非 LVGL 回调里 | littlefs 写时复制要擦一个块，几十 ms 会卡住界面交互 |

---

## 五、有意保留 / 未处理

| 项 | 原因 |
| --- | --- |
| `LCD.c` 换行判定用 `WIDTH-1`、循环头用 `WIDTH`（末列提前 1 像素换行） | 既有代码，可能是刻意的右边距，不在本次问题范围 |
| `UI_Task_IsLowPower()`、`disp_enable_update/disable_update()` 无调用者 | 预留/移植层标准接口，保留 |
| `LCD.h` 中 `LCD_Write_String` 形参名与实现不一致 | 无害 |
| 未开启 `INCLUDE_uxTaskGetStackHighWaterMark` | 改的是内核配置；需要时置 1 并在关闭键盘处打印即可 |

---

## 六、待硬件实测确认

1. **键盘链路**：点图标 → 出键盘（SSID 框已聚焦）→ 输入 → 回车与 `✓` 都能提交 →
   串口出现 `[NET] WiFi credentials updated` / `WiFi connected`；空 SSID 按 `✓` 应留在界面并显示红色 `SSID required`。
2. **LVGL 内存池**：需要确认时在 `lv_port_disp_init()` 里临时加一次 `lv_mem_monitor()` 打印，
   反复进出键盘十几次看 free 是否稳定在同一量级（判断有无泄漏）。
3. **UI 任务栈水位**：开启 `INCLUDE_uxTaskGetStackHighWaterMark` 后打印 `uxTaskGetStackHighWaterMark(NULL)`，
   确认 6 KB 有余量。
4. **开机一致性**：断电重启，结果页状态应与主界面 WiFi 图标一致，串口不应出现 `boot net stage timeout`。

---

## 七、改动文件一览

| 文件 | 改动要点 |
| --- | --- |
| `BSP/Inc/LCD.h`、`BSP/Src/LCD.c` | 新增 `LCD_Draw_Pixels()` / `LCD_RenderBuffer()`；`s_scratch` 显式 4 字节对齐；整段持锁；渲染缓冲占用告警 |
| `Middlewares/Lvgl/porting/lv_port_disp.c/.h` | 显示端口真实实现（借用渲染缓冲、flush 走 LCD 块写）；注册失败判空；打印内存池余量 |
| `Middlewares/Lvgl/porting/lv_port_indev.c/.h` | 输入端口真实实现（XPT2046 → 指针设备）；未校准不上报按下 |
| `User/Task/Src/UI_Task.c` | LVGL 初始化与 10 ms 心跳；键盘界面开/关与状态复位；主循环改造；触摸节拍 50 ms 与判定区域对齐；开机超时 60 s；退出键盘时落盘 |
| `User/Task/Inc/Task_Config.h` | UI 栈 6 KB；新增触摸轮询节拍、开机网络超时；优先级注释改正 |
| `User/Task/Inc/Task_Common.h` | 新增 `EV_WIFI_CRED` |
| `User/Task/Src/Net_Task.c` | 处理 `EV_WIFI_CRED`：立即用新凭据重连并补 SNTP/天气 |
| `User/UI/Src/ui_keyboard.c` | 输入框补 `CLICK_FOCUSABLE`/单行/长度上限/密码打码；回调不再切屏删屏；`✓` 与回车统一提交 |
| `User/UI/Src/ui_events.c`、`User/UI/Inc/ui_events.h` | `GetInfo()` 重写为返回 `bool`，空 SSID 给红字提示 |
| `User/Application/Src/App.c`、`User/Application/Inc/App.h` | 凭据数组化与持久化（`WiFi_Config_Set/Save/Load/IsSet/IsDirty`）；无凭据时的查询等待；周期检查不再空发 `CWJAP` |
| `User/Application/Src/Board.c` | 显式使能 CCM 时钟 |
| `User/UI/Src/Boot_Page.c` | 未配置凭据时显示 `[WiFi] Not set, tap icon` |
| `Documents/README.md` | 任务/事件位表更新，新增 4.3 节（配网流程与约束） |
| `.gitignore` | 补充 `MDK/*.log`（临时编译日志） |

> **遗留安全提醒（未擅自处理）**：`.gitignore` 里忽略的是 `User/Inc/Secrets.h`，
> 但目录已迁至 `User/Application/Inc/`，规则失效——**真实的 `Secrets.h`（含 WiFi 密码与
> 天气接口 key）目前处于被 git 跟踪的状态**。建议补规则并执行
> `git rm --cached User/Application/Inc/Secrets.h`（文件保留在磁盘，编译不受影响）。
