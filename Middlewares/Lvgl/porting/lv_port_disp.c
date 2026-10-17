/**
 * @file lv_port_disp.c
 *
 * LVGL 显示端口: 把 LVGL 画好的 RGB565 区域直接写进 ST7789V 的 GRAM。
 *
 * 关键约定(改这里前先看清楚):
 *   1. 绘制缓冲不另外开数组, 而是借用 LCD 模块的整行渲染缓冲(240 x 48 像素)。
 *      本工程主 SRAM 只剩约 2KB, 另开一块 10 行的缓冲就要 4800 字节, 直接爆内存;
 *      那块缓冲平时只被 LCD_Write_String() 独占使用, 而刷屏与字符串渲染
 *      都由 uiTask 串行发起, 不会互相踩。
 *   2. 缓冲必须留在 SRAM(不能放 CCM): 最终由 DMA1_Stream5 读走, 而 DMA 访问不了 CCM。
 *   3. 字节序交给 LCD_Write_Gram(): SPI 按半字、高字节先发, 与屏幕上的裸绘内容一致,
 *      所以 lv_conf.h 里 LV_COLOR_16_SWAP 保持 0, 这里不需要做交换。
 */
#include "lv_port_disp.h"
#include "BuildConfig.h"
#include "LCD.h"

/*********************
 *      DEFINES
 *********************/
#define DISP_HOR_RES WIDTH
#define DISP_VER_RES HEIGHT

/**********************
 *  STATIC VARIABLES
 **********************/
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static volatile bool s_flush_enabled = true;

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * @brief LVGL 渲染完成回调: 把一个矩形区域搬进 LCD
 * @note  这个函数运行在调用 lv_timer_handler() 的任务里(本工程是 uiTask, LCD 的唯一写者)
 */
static void disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    if (s_flush_enabled)
    {
        LCD_Draw_Pixels((uint16_t)area->x1, (uint16_t)area->y1,
                        (uint16_t)area->x2, (uint16_t)area->y2,
                        (const uint16_t *)color_p);
    }

    /* 必须告知 LVGL 本次搬运结束, 否则它不会再发起下一帧 */
    lv_disp_flush_ready(disp_drv);
}

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_port_disp_init(void)
{
    uint32_t capacity_px = 0U;
    uint16_t *buf = LCD_RenderBuffer(&capacity_px);

    /* 单缓冲: 屏幕没有独立的显存, DMA 发送期间也没法并行渲染,
     * 加第二块缓冲只是白占内存, 没有收益。 */
    lv_disp_draw_buf_init(&s_draw_buf, buf, NULL, capacity_px);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = DISP_HOR_RES;
    s_disp_drv.ver_res = DISP_VER_RES;
    s_disp_drv.flush_cb = disp_flush;
    s_disp_drv.draw_buf = &s_draw_buf;

    if (lv_disp_drv_register(&s_disp_drv) == NULL)
    {
        DBG_PRINTF("[UI] ERROR: lv_disp_drv_register FAILED, LVGL mem pool too small?\r\n");
        return;
    }

    DBG_PRINTF("[UI] LVGL disp ready: %dx%d, draw buf %lu px\r\n",
           (int)DISP_HOR_RES, (int)DISP_VER_RES, (unsigned long)capacity_px);
}

void disp_enable_update(void)
{
    s_flush_enabled = true;
}

void disp_disable_update(void)
{
    s_flush_enabled = false;
}
