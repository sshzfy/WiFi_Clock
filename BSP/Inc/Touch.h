/* ============================================================
 * 触摸屏驱动: XPT2046/TSC2046 四线电阻屏控制器
 * ------------------------------------------------------------
 * 与 ST7789 共用 SPI3 的 SCK/MISO/MOSI, 片选各自独立:
 *   LCD : PE2 (ST7789_CS) / 背光 PE5 / DC PE4 / RESET PE3
 *   触摸: PE0 (CS)        / PENIRQ PE1 (低电平 = 按下)
 * ============================================================ */
#ifndef __TOUCH_H__
#define __TOUCH_H__

#include "main.h"
#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "Timer.h"
#include "Usart.h"
#include "LCD.h"
#include "Asset.h"

/* 触摸屏引脚 (SCK/MISO/MOSI 与 ST7789 共用, 由 LCD 的 GPIO 初始化配成复用) */
#define TOUCH_SPI             SPI3
#define TOUCH_SPI_PORT        GPIOC
#define TOUCH_SCLK_PIN        GPIO_Pin_10
#define TOUCH_MISO_PIN        GPIO_Pin_11
#define TOUCH_MOSI_PIN        GPIO_Pin_12

#define TOUCH_PORT           GPIOE
#define TOUCH_CS_PIN         GPIO_Pin_0
#define TOUCH_IRQ_PIN        GPIO_Pin_1

/* 参数定义 */
#define TOUCH_WIDTH                  240U // 触摸屏宽度, 与 LCD 一致
#define TOUCH_HEIGHT                 320U // 触摸屏高度, 与 LCD 一致
#define TOUCH_MAX_SAMPLES            10U  // 每次取坐标的采样次数(至少 3 次才能去掉最大最小值)
#define TOUCH_SAMPLE_SPAN_MAX        300U // 一次采样里(最大-最小)的允许跨度, 超出判为滑动/干扰
#define TOUCH_CMD_GAP_US             2U   // 两条转换命令之间的间隔, 给通道切换后的内部采集留时间

/* 校准等待参数 */
#define TOUCH_CAL_PRESS_TIMEOUT_MS          30000U // 等待点击一个校准点的超时
#define TOUCH_CAL_RELEASE_TIMEOUT_MS        5000U  // 等待松手的超时
#define TOUCH_CAL_POLL_MS                   10U    // 轮询步长

/* ================ 校准参数的存储 ================
 * 存放位置: 外部 Flash(W25Q64) littlefs 的运行数据区 /data/touch_cal.bin,
 * 读写全部走资源层接口, 本驱动不直接碰 Flash:
 *     Asset_SaveData(TOUCH_CAL_NAME, &data, sizeof(data));
 *     Asset_LoadData(TOUCH_CAL_NAME, &data, sizeof(data)); 
 * ================================*/
#define TOUCH_CAL_NAME           "touch_cal" // 数据名(资源层自动补 /data/ 前缀与 .bin 后缀)
#define TOUCH_CAL_MAGIC          0x544F5543U // 'TOUC': 判断文件内容是不是本驱动写的
#define TOUCH_CAL_VERSION        1U          // 布局版本: 结构一改就 +1, 旧数据自动作废

/* 触摸屏控制字: S=1, A2:A0 通道, MODE=0(12位), SER/DFR=0(差分), PD1:PD0=00
 * 这两个是 XPT2046 手册里的通道号(101 / 001), 不代表屏幕的横竖方向 */
#define TOUCH_CMD_X        0xD0U // 通道 101
#define TOUCH_CMD_Y        0x90U // 通道 001

/* ================ 屏幕横竖方向与通道的对应 ================
 * 本模块实测(四点校准数据):
 *   沿屏幕横向移动时变化的是通道 001(0x90), 纵向移动时变化的是通道 101(0xD0)
 * 若换屏后校准报 "invalid X span", 说明这两个通道反了, 把下面两行对调即可。 
 * ================================*/
#define TOUCH_CMD_SCREEN_X        TOUCH_CMD_Y // 屏幕横向
#define TOUCH_CMD_SCREEN_Y        TOUCH_CMD_X // 屏幕纵向

/* 片选信号操作宏 */
#define TOUCH_CS_HIGH() GPIO_SetBits(TOUCH_PORT, TOUCH_CS_PIN)
#define TOUCH_CS_LOW() GPIO_ResetBits(TOUCH_PORT, TOUCH_CS_PIN)

/* 触摸状态结构体 */
typedef struct
{
    uint16_t x;        // 原始X坐标(0~4095)
    uint16_t y;        // 原始Y坐标(0~4095)
    int16_t x_display; // 校准后的显示X坐标(有符号: 校准时可能算出屏幕外的负值);
    int16_t y_display; // 校准后的显示Y坐标
    bool pressed;      // 触摸状态: true-按下, false-释放
} TouchState_t;

/* 校准参数结构体: 屏幕坐标 = k * 原始坐标 + b */
typedef struct
{
    float kx;        // X方向比例系数
    float ky;        // Y方向比例系数
    int16_t bx;      // X方向偏移量
    int16_t by;      // Y方向偏移量
    bool calibrated; // 校准标志: true-已校准, false-未校准
} CalibrationParams_t;

/* 落到 /data/touch_cal.bin 的定长结构 */
typedef struct
{
    uint32_t magic;   // TOUCH_CAL_MAGIC
    uint16_t version; // 布局版本 TOUCH_CAL_VERSION
    uint16_t size;    // 本结构体字节数
    float kx;         // X 比例系数
    float ky;         // Y 比例系数
    int16_t bx;       // X 偏移量
    int16_t by;       // Y 偏移量
    uint16_t spare;   // 预留(同时把结构体凑成 4 字节整数倍)
} TouchCalData_t;

extern TouchState_t touch_state;
extern CalibrationParams_t calibration_params;
extern const uint16_t cal_points[5][2];

void Touch_Park(void);
bool Touch_Init(void);
bool Touch_IsPressed(void);
bool Touch_ReadRaw(uint16_t *x, uint16_t *y);
bool Touch_ReadFiltered(uint16_t *x, uint16_t *y);
bool Touch_GetPoint(TouchState_t *touch);

typedef void (*touch_calpointcb_t)(uint16_t x, uint16_t y);
void Touch_SetPoints_RegisterCallback(touch_calpointcb_t cb);

bool Touch_Calibrate(void);

#endif /* __TOUCH_H__ */
