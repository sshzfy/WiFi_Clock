/**
 * @file lv_port_indev.c
 *
 * LVGL 输入端口: 把触摸屏驱动的坐标喂给 LVGL 的指针设备。
 *
 * 说明:
 *   - 触摸读取由本回调在 lv_timer_handler() 内按 LV_INDEV_DEF_READ_PERIOD(15ms) 触发,
 *     也就是跑在 uiTask 里; 触摸驱动内部自己会抢/放 SPI3 总线锁, 与刷屏不会互相打断。
 *   - Touch_GetPoint() 一帧的滤波采样约 0.8ms, 松开或采样不可信时返回 false,
 *     LVGL 会把这一帧当作"抬起"(和真实手指抬起走同一条路径)。
 *   - 没校准过时 Touch_GetPoint() 给的是 0~4095 的原始坐标, 这里统一钳到屏幕范围内,
 *     避免 LVGL 收到超范围坐标。
 */
#include "lv_port_indev.h"
#include "Touch.h"

/**********************
 *  STATIC VARIABLES
 **********************/
static lv_indev_t *s_indev_touchpad = NULL;

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    TouchState_t ts;
    lv_coord_t x;
    lv_coord_t y;

    LV_UNUSED(indev_drv);

    if (!Touch_GetPoint(&ts))
    {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (calibration_params.calibrated)
    {
        x = (lv_coord_t)ts.x_display;
        y = (lv_coord_t)ts.y_display;
    }
    else
    {
        /* 还没校准: Touch_GetPoint() 给的是 0~4095 的原始坐标。
         * 既不能直接钳位(所有点都会落到右下角, 也就是键盘 O K / 回车键附近), 也不能
         * 干脆不报按下(那样触摸会彻底没反应, 用户连校准入口都进不去)。
         * 这里按比例折算到屏幕上: 位置当然不准, 但它不会恒定砸在功能键上,
         * 触摸仍有反馈, 主线流程不会因为一次校准没做完就整个废掉。 */
        x = (lv_coord_t)(((uint32_t)ts.x * (uint32_t)TOUCH_WIDTH) / 4096U);
        y = (lv_coord_t)(((uint32_t)ts.y * (uint32_t)TOUCH_HEIGHT) / 4096U);
    }

    if (x < 0)
        x = 0;
    else if (x >= (lv_coord_t)TOUCH_WIDTH)
        x = (lv_coord_t)TOUCH_WIDTH - 1;

    if (y < 0)
        y = 0;
    else if (y >= (lv_coord_t)TOUCH_HEIGHT)
        y = (lv_coord_t)TOUCH_HEIGHT - 1;

    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
}

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_port_indev_init(void)
{
    static lv_indev_drv_t indev_drv;

    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touchpad_read;
    s_indev_touchpad = lv_indev_drv_register(&indev_drv);
}

/**
 * @brief 取触摸输入设备句柄
 * @return 输入设备指针(未初始化时为 NULL)
 * @note  需要给输入设备挂 group 或临时停用触摸时用
 */
lv_indev_t *lv_port_indev_get(void)
{
    return s_indev_touchpad;
}
