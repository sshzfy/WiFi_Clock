#include "Touch.h"
#include "Asset.h" // 校准参数存在外部 Flash 的运行数据区: /data/touch_cal.bin

/* 去掉一个最大值和一个最小值之后还要有样本可平均 */
#if (TOUCH_MAX_SAMPLES < 3U)
#error "TOUCH_MAX_SAMPLES must be >= 3"
#endif

static uint8_t Touch_SpiReadWriteByte(uint8_t tx);
static void Touch_SpiClearRx(void);
static uint16_t Touch_ReadAd(uint8_t cmd);
static void Touch_GPIO_Init(void);
static void Touch_Idle(uint32_t ms);
static bool Touch_WaitState(bool pressed, uint32_t timeout_ms);
static bool Touch_LoadCalibration(void);
static bool Touch_SaveCalibration(void);
static bool Touch_ScaleValid(float k);
static int16_t Touch_RoundToI16(float v);

static bool s_touch_ready = false;              // SPI3 已就绪(即 Touch_Init 成功过)
static touch_calpointcb_t s_calpoint_cb = NULL; // 校准点的绘制回调(可选)

TouchState_t touch_state = {0};               // 最近一次 Touch_GetPoint() 的结果; 也可用 Touch_GetPoint(NULL) 直接读它
CalibrationParams_t calibration_params = {0}; // 当前生效的校准参数

/* 校准点坐标(屏幕坐标) */
const uint16_t cal_points[5][2] = {
    {20, 20},                              // 左上角
    {TOUCH_WIDTH - 20, 20},                // 右上角
    {TOUCH_WIDTH - 20, TOUCH_HEIGHT - 20}, // 右下角
    {20, TOUCH_HEIGHT - 20},               // 左下角
    {TOUCH_WIDTH / 2, TOUCH_HEIGHT / 2}    // 中心点(只用来校验, 不参与拟合)
};

/* ================ 内部接口 ================ */
static uint8_t Touch_SpiReadWriteByte(uint8_t tx)
{
    while (SPI_I2S_GetFlagStatus(TOUCH_SPI, SPI_I2S_FLAG_TXE) == RESET)
        ;
    SPI_I2S_SendData(TOUCH_SPI, tx);

    while (SPI_I2S_GetFlagStatus(TOUCH_SPI, SPI_I2S_FLAG_RXNE) == RESET)
        ;
    return (uint8_t)SPI_I2S_ReceiveData(TOUCH_SPI);
}

/* 开始一次触摸事务前清空 SPI3 的接收通道。
 * LCD 是全双工发送且从不读 DR, 所以 RXNE 一直被上一次刷屏留下的数据占着;
 * 不清掉的话这里读到的第一个字节就是旧数据, 24 位时序会整体错位一字节,
 * 算出来的坐标完全不对。顺便用"读 DR 再读 SR"的序列清掉 OVR 溢出标志。 */
static void Touch_SpiClearRx(void)
{
    volatile uint16_t dummy = 0U; // 读取的 dummy 数据

    /* 清空接收通道 */
    while (SPI_I2S_GetFlagStatus(TOUCH_SPI, SPI_I2S_FLAG_RXNE) != RESET)
        dummy = SPI_I2S_ReceiveData(TOUCH_SPI); // 读取并忽略数据

    /* 读 DR, 再读 SR, 清 OVR */
    dummy = TOUCH_SPI->DR;
    (void)SPI_I2S_GetFlagStatus(TOUCH_SPI, SPI_I2S_FLAG_RXNE);
    (void)dummy; // 丢弃数据
}

/* 触摸与 LCD 共用 SPI3, 靠 LCD 模块的总线锁串行化:
 *   LCD_Bus_Acquire() = 拿 SPI3 独占锁 + 等 LCD 的 DMA 发完 + 切 8 位 + 降到 656kHz
 *   LCD_Bus_Release() = 恢复 21MHz + 放锁
 * 拿到锁之后没有别的任务会碰 SPI3, 所以事务内可以放心用 delay_us。
 * 注意 printf 绝不能放进事务里: 它会等 DMA 完成并可能阻塞。 */

/* 读一次 AD 结果(12 位): 命令字节 + 2 个数据字节共 24 个时钟,
 * 其中命令字节回读的数据无效, 有效数据是 bit11..bit0 */
static uint16_t Touch_ReadAd(uint8_t cmd)
{
    uint8_t byte_h, byte_l;

    (void)Touch_SpiReadWriteByte(cmd);
    byte_h = Touch_SpiReadWriteByte(0x00);
    byte_l = Touch_SpiReadWriteByte(0x00);

    return (uint16_t)(((uint16_t)byte_h << 4) | ((uint16_t)byte_l >> 4));
}

static void Touch_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    GPIO_StructInit(&GPIO_InitStructure);

    /* CS: 空闲拉高, 不选中触摸时把 MISO 让给总线 */
    GPIO_SetBits(TOUCH_PORT, TOUCH_CS_PIN);
    GPIO_InitStructure.GPIO_Pin = TOUCH_CS_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(TOUCH_PORT, &GPIO_InitStructure);

    /* PENIRQ: XPT2046 的 PENIRQ 是开漏输出, 低电平表示按下 */
    GPIO_InitStructure.GPIO_Pin = TOUCH_IRQ_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(TOUCH_PORT, &GPIO_InitStructure);
}

/** @brief 延时: 调度器已在跑就让出 CPU, 裸机/调度器未启动时退化为忙等
 * @param ms 延时时间, 单位毫秒 */
static void Touch_Idle(uint32_t ms)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
        vTaskDelay(pdMS_TO_TICKS(ms));
    else
        delay_ms(ms);
}

/** @brief 等 PENIRQ 变成期望的状态; 超时返回 false, 避免死等饿死其它任务
 * @param pressed 期望的状态: true 表示按下, false 表示松开
 * @param timeout_ms 超时时间, 单位毫秒
 * @return true 成功, false 超时 */
static bool Touch_WaitState(bool pressed, uint32_t timeout_ms)
{
    uint64_t t0 = TIM5_Get_ms();

    while (Touch_IsPressed() != pressed)
    {
        if ((TIM5_Get_ms() - t0) > (uint64_t)timeout_ms)
            return false;

        Touch_Idle(TOUCH_CAL_POLL_MS);
    }

    return true;
}

/** @brief 比例系数的合理性检查: 0 / NaN / 离谱的值都判为无效(NaN 与任何数比较均为 false)
 * @param k 比例系数
 * @return true 合理, false 无效 */
static bool Touch_ScaleValid(float k)
{
    return ((k > 0.0001f) || (k < -0.0001f)) && (k < 100.0f) && (k > -100.0f);
}

/** @brief 四舍五入后转 int16(偏移量只有整数精度)
 * @param v 浮点数
 * @return 整数部分 */
static int16_t Touch_RoundToI16(float v)
{
    return (int16_t)((v >= 0.0f) ? (v + 0.5f) : (v - 0.5f));
}

/** @brief 从外部 Flash 读回校准参数
 * @return true 读到一组可用参数; false 没存过/内容不合法(保持未校准状态)
 * @note 读的是 /data/touch_cal.bin, 必须已挂载 littlefs(Asset_Init 之后) */
static bool Touch_LoadCalibration(void)
{
    TouchCalData_t data;
    int n;

    calibration_params.calibrated = false;

    /* 资源层没起来(比如没检测到 W25Q64): 只能按未校准处理 */
    if (!Asset_Ready())
        return false;

    memset(&data, 0, sizeof(data));

    /* 还没存过时返回负错误码(资源层不刷日志), 这里直接当"未校准" */
    n = Asset_LoadData(TOUCH_CAL_NAME, &data, (uint32_t)sizeof(data));
    if (n != (int)sizeof(TouchCalData_t))
        return false;

    /* 对 Flash 读回的校准数据做“魔数 + 版本 + 长度”三重校验 */
    if ((data.magic != TOUCH_CAL_MAGIC) ||
        (data.version != TOUCH_CAL_VERSION) ||
        (data.size != (uint16_t)sizeof(TouchCalData_t)))
    {
        printf("[TOUCH] calibration data mismatch, recalibrate\r\n");
        return false;
    }
    /* 校验比例系数是否合理 */
    if ((!Touch_ScaleValid(data.kx)) || (!Touch_ScaleValid(data.ky)))
    {
        printf("[TOUCH] calibration data invalid, recalibrate\r\n");
        return false;
    }

    calibration_params.kx = data.kx;
    calibration_params.ky = data.ky;
    calibration_params.bx = data.bx;
    calibration_params.by = data.by;
    calibration_params.calibrated = true;

    return true;
}

/** @brief 把校准参数写到外部 Flash: /data/touch_cal.bin
 * @return true 已落盘; false 资源层不可用或写入失败(参数只在 RAM 里有效)
 * @note littlefs 写入是写时复制: 中途掉电保留旧值, 不会写出半个文件;
 *       擦除单位是 4KB 扇区(几十 ms), 不像内部 Flash 那样要擦 128KB */
static bool Touch_SaveCalibration(void)
{
    TouchCalData_t data;
    int err;

    /* 填充字节也置 0, 落盘内容确定 */
    memset(&data, 0, sizeof(data));
    data.magic = TOUCH_CAL_MAGIC;
    data.version = TOUCH_CAL_VERSION;
    data.size = (uint16_t)sizeof(TouchCalData_t);
    data.kx = calibration_params.kx;
    data.ky = calibration_params.ky;
    data.bx = calibration_params.bx;
    data.by = calibration_params.by;

    /* 写入校准参数 */
    err = Asset_SaveData(TOUCH_CAL_NAME, &data, (uint32_t)sizeof(data));
    if (err != ASSET_OK)
    {
        printf("[TOUCH] save calibration failed, err = %d\r\n", err);
        return false;
    }

    return true;
}

/* ================ 对外接口 ================ */
bool Touch_Init(void)
{
    Touch_GPIO_Init();

    /* SPI3 由 LCD_Init() 初始化: 没初始化就通信会卡在等 TXE, 这里提前拦下来 */
    if ((TOUCH_SPI->CR1 & SPI_CR1_SPE) == 0U)
    {
        s_touch_ready = false;
        printf("[TOUCH] SPI3 not enabled, call LCD_Init() first\r\n");
        return false;
    }

    s_touch_ready = true;

    /* 校准参数在外部 Flash 上: 资源层没挂载好时读不到也存不了,
     * 仍然可以读原始坐标, 只是要重新校准 */
    if (!Asset_Ready())
        printf("[TOUCH] asset layer not ready, calibration unavailable\r\n");

    /* 从 Flash 读回校准参数 */
    (void)Touch_LoadCalibration();
    printf("[TOUCH] init done, calibrated=%d\r\n", (int)calibration_params.calibrated);
    if (calibration_params.calibrated)
    {
        printf("[TOUCH] kx=%.4f ky=%.4f bx=%d by=%d\r\n",
               calibration_params.kx, calibration_params.ky,
               (int)calibration_params.bx, (int)calibration_params.by);
    }
    else
    {
        printf("[TOUCH] no calibration data, run Touch_Calibrate()\r\n");
    }

    return true;
}

/**
 * @brief  检测是否有触摸动作
 * @retval true-有触摸, false-无触摸
 */
bool Touch_IsPressed(void)
{
    return (GPIO_ReadInputDataBit(TOUCH_PORT, TOUCH_IRQ_PIN) == Bit_RESET);
}

/**
 * @brief  读取触摸屏的原始坐标(一次 CS 周期内读完 X/Y)
 * @param  x: 指向X轴坐标的指针
 * @param  y: 指向Y轴坐标的指针
 * @retval true-读取成功, false-未初始化或参数为空
 */
bool Touch_ReadRaw(uint16_t *x, uint16_t *y)
{
    if ((x == NULL) || (y == NULL) || (!s_touch_ready))
        return false;

    LCD_Bus_Acquire();

    /* 清空接收缓冲区
     * 拉低 CS 选中触摸
     * 读取x, y轴坐标
     * 提升 CS 释放触摸 */
    Touch_SpiClearRx();
    TOUCH_CS_LOW();
    *x = Touch_ReadAd(TOUCH_CMD_SCREEN_X); // 屏幕横向
    delay_us(TOUCH_CMD_GAP_US);
    *y = Touch_ReadAd(TOUCH_CMD_SCREEN_Y); // 屏幕纵向
    TOUCH_CS_HIGH();

    LCD_Bus_Release();

    return true;
}

/**
 * @brief  读取触摸屏的滤波坐标(多次采样, 去掉最大最小值后平均)
 * @param  x: 指向X轴坐标的指针
 * @param  y: 指向Y轴坐标的指针
 * @retval true-坐标可信, false-采样途中松手/抖动过大/未初始化
 */
bool Touch_ReadFiltered(uint16_t *x, uint16_t *y)
{
    uint16_t xs[TOUCH_MAX_SAMPLES]; // X轴坐标样本
    uint16_t ys[TOUCH_MAX_SAMPLES]; // Y轴坐标样本
    uint32_t x_sum = 0U, y_sum = 0U;
    bool released = false; // 是否松手

    if ((x == NULL) || (y == NULL) || (!s_touch_ready))
        return false;

    /* 一次事务里连续采完: CS 全程拉低, 省掉反复升降 CS 带来的唤醒等待,
     * 也只抢一次总线锁(总线锁持有时间约 0.8ms, 对刷屏没有可感影响) */
    LCD_Bus_Acquire();
    Touch_SpiClearRx();
    TOUCH_CS_LOW();

    for (uint16_t i = 0U; i < TOUCH_MAX_SAMPLES; i++)
    {
        xs[i] = Touch_ReadAd(TOUCH_CMD_SCREEN_X); // 屏幕横向
        delay_us(TOUCH_CMD_GAP_US);
        ys[i] = Touch_ReadAd(TOUCH_CMD_SCREEN_Y); // 屏幕纵向
        delay_us(TOUCH_CMD_GAP_US);

        /* 采样途中松手: 这批数据不可信 */
        if (!Touch_IsPressed())
        {
            released = true;
            break;
        }
    }

    TOUCH_CS_HIGH();
    LCD_Bus_Release();

    /* 采样途中松手: 这批数据不可信 */
    if (released)
        return false;

    /* 冒泡排序(样本只有十来组, 开销可忽略) */
    for (uint16_t i = 0U; i < (TOUCH_MAX_SAMPLES - 1U); i++)
    {
        for (uint16_t j = 0U; j < (TOUCH_MAX_SAMPLES - 1U - i); j++)
        {
            if (xs[j] > xs[j + 1U])
            {
                uint16_t temp = xs[j];
                xs[j] = xs[j + 1U];
                xs[j + 1U] = temp;
            }
            if (ys[j] > ys[j + 1U])
            {
                uint16_t temp = ys[j];
                ys[j] = ys[j + 1U];
                ys[j + 1U] = temp;
            }
        }
    }

    /* 跨度太大说明手指在滑动或干扰严重, 这一帧丢掉 */
    if (((uint16_t)(xs[TOUCH_MAX_SAMPLES - 1U] - xs[0]) > TOUCH_SAMPLE_SPAN_MAX) ||
        ((uint16_t)(ys[TOUCH_MAX_SAMPLES - 1U] - ys[0]) > TOUCH_SAMPLE_SPAN_MAX))
        return false;

    /* 去掉最大值和最小值, 取平均 */
    for (uint16_t i = 1U; i < (TOUCH_MAX_SAMPLES - 1U); i++)
    {
        x_sum += xs[i];
        y_sum += ys[i];
    }

    /* 滤波并取均值后的坐标 */
    *x = (uint16_t)(x_sum / (TOUCH_MAX_SAMPLES - 2U));
    *y = (uint16_t)(y_sum / (TOUCH_MAX_SAMPLES - 2U));

    return true;
}

/**
 * @brief  获取触摸点信息(按下状态 + 坐标)
 * @param  touch: 存储触摸信息的结构体指针, 传 NULL 时写入全局 touch_state
 * @retval true-本次拿到有效坐标, false-未按下或坐标不可信
 */
bool Touch_GetPoint(TouchState_t *touch)
{
    TouchState_t *dst = (touch != NULL) ? touch : &touch_state;
    uint16_t x_raw, y_raw;

    dst->pressed = Touch_IsPressed();

    /* 未按下或未初始化: 直接返回 */
    if ((!dst->pressed) || (!s_touch_ready))
    {
        dst->x = 0U;
        dst->y = 0U;
        dst->x_display = 0;
        dst->y_display = 0;
        return false;
    }

    /* 读取滤波后的坐标
     * 采样途中松手或抖动过大: 这一帧不更新坐标 */
    if (!Touch_ReadFiltered(&x_raw, &y_raw))
    {
        dst->x = 0U;
        dst->y = 0U;
        dst->x_display = 0;
        dst->y_display = 0;
        return false;
    }

    dst->x = x_raw;
    dst->y = y_raw;

    /* 计算校准后的坐标 */
    if (calibration_params.calibrated)
    {
        /* 用 int32 中转: 屏幕坐标可能是负的(还没钳位), 直接塞进 int16 会溢出 */
        int32_t xd = (int32_t)(calibration_params.kx * (float)x_raw + (float)calibration_params.bx);
        int32_t yd = (int32_t)(calibration_params.ky * (float)y_raw + (float)calibration_params.by);

        /* 坐标范围限制 */
        if (xd < 0)
            xd = 0;
        else if (xd >= (int32_t)TOUCH_WIDTH)
            xd = (int32_t)TOUCH_WIDTH - 1;

        if (yd < 0)
            yd = 0;
        else if (yd >= (int32_t)TOUCH_HEIGHT)
            yd = (int32_t)TOUCH_HEIGHT - 1;

        dst->x_display = (int16_t)xd;
        dst->y_display = (int16_t)yd;
    }
    else
    {
        /* 没校准过就给出原始坐标, 至少能看到有响应 */
        dst->x_display = (int16_t)x_raw;
        dst->y_display = (int16_t)y_raw;
    }

    return true;
}

/** @brief 设置校准点的绘制回调(界面层用来在屏幕上画出要点击的位置)
 * @param cb 回调函数; 传 NULL 表示只走串口提示 */
void Touch_SetPoints_RegisterCallback(touch_calpointcb_t cb)
{
    s_calpoint_cb = cb;
}

/**
 * @brief  五点校准触摸屏, 成功后把参数写入 Flash
 * @retval true-校准完成并保存, false-超时/采样异常/保存失败
 * @note   需要串口按提示点击: 依次为左上、右上、右下、左下、中心
 */
bool Touch_Calibrate(void)
{
    uint16_t raw_x[5] = {0};
    uint16_t raw_y[5] = {0};
    uint8_t i = 0U;
    float lx, rx, ty, by;
    float kx, ky, bx_f, by_f;
    float x_span, y_span;

    if (!s_touch_ready)
    {
        printf("[TOUCH] not initialized, call Touch_Init() first\r\n");
        return false;
    }

    printf("[TOUCH] calibration start: click the 5 points in order\r\n");

    while (i < 5U)
    {
        printf("[TOUCH] click point %u at (%u,%u)\r\n", (unsigned)(i + 1U),
               (unsigned)cal_points[i][0], (unsigned)cal_points[i][1]);

        /* 让界面把这个校准点画出来(通常清屏 + 画十字), 用户照着点 */
        if (s_calpoint_cb != NULL)
            s_calpoint_cb(cal_points[i][0], cal_points[i][1]);

        /* 等按下; 带超时, 避免死等 */
        if (!Touch_WaitState(true, TOUCH_CAL_PRESS_TIMEOUT_MS))
        {
            printf("[TOUCH] calibration timeout: no press detected\r\n");
            return false;
        }
        /* 等触摸稳定 */
        Touch_Idle(20);

        if (!Touch_ReadFiltered(&raw_x[i], &raw_y[i]))
        {
            printf("[TOUCH] sample rejected, click the same point again\r\n");
            continue; // i 不递增, 重点这个点
        }

        printf("[TOUCH] point %u raw = (%u,%u)\r\n", (unsigned)(i + 1U),
               (unsigned)raw_x[i], (unsigned)raw_y[i]);

        /* 等松开; 带超时, 避免死等 */
        if (!Touch_WaitState(false, TOUCH_CAL_RELEASE_TIMEOUT_MS))
        {
            printf("[TOUCH] calibration timeout: release not detected\r\n");
            return false;
        }
        /* 等松手稳定 */
        Touch_Idle(20);

        i++;
    }

    /* 左右/上下两端的原始坐标平均(用 float 算, 避免整数除法丢掉 0.5 个 LSB) */
    lx = ((float)raw_x[0] + (float)raw_x[3]) * 0.5f; // 左上+左下
    rx = ((float)raw_x[1] + (float)raw_x[2]) * 0.5f; // 右上+右下
    ty = ((float)raw_y[0] + (float)raw_y[1]) * 0.5f; // 左上+右上
    by = ((float)raw_y[2] + (float)raw_y[3]) * 0.5f; // 右下+左下

    /* 分母是两端原始坐标之差: 太小说明接线/采样异常, 直接判失败。
     * 若是"一个方向几乎不变、另一个方向变化很大", 那就是屏幕横竖与
     * XPT2046 通道接反了, 直接点名提示, 免得靠猜。 */
    x_span = rx - lx;
    y_span = by - ty;

    if (((x_span < 100.0f) && (x_span > -100.0f)) ||
        ((y_span < 100.0f) && (y_span > -100.0f)))
    {
        printf("[TOUCH] invalid span: X=%.1f Y=%.1f, calibration failed\r\n", x_span, y_span);
        if ((x_span < 100.0f) && (x_span > -100.0f))
            printf("[TOUCH] hint: X barely moves while Y does -> swap TOUCH_CMD_SCREEN_X / SCREEN_Y\r\n");
        else
            printf("[TOUCH] hint: Y barely moves while X does -> swap TOUCH_CMD_SCREEN_X / SCREEN_Y\r\n");
        return false;
    }

    /* 比例系数: 两端之间对应 (宽/高 - 2*20) 像素 */
    kx = ((float)TOUCH_WIDTH - 40.0f) / x_span;
    ky = ((float)TOUCH_HEIGHT - 40.0f) / y_span;

    /* 偏移量: 让最左/最上的原始坐标落在屏幕的 20 像素处 */
    bx_f = 20.0f - (kx * lx);
    by_f = 20.0f - (ky * ty);

    if ((!Touch_ScaleValid(kx)) || (!Touch_ScaleValid(ky)) ||
        (bx_f < -32768.0f) || (bx_f > 32767.0f) ||
        (by_f < -32768.0f) || (by_f > 32767.0f))
    {
        printf("[TOUCH] calibration out of range, failed\r\n");
        return false;
    }

    calibration_params.kx = kx;
    calibration_params.ky = ky;
    calibration_params.bx = Touch_RoundToI16(bx_f);
    calibration_params.by = Touch_RoundToI16(by_f);
    calibration_params.calibrated = true; // 校准成功, 标记为已校准

    /* 用中心点校验一下(它不参与拟合): 误差大说明轴交换/方向反了 */
    printf("[TOUCH] center check: expect (%u,%u) got (%.1f,%.1f)\r\n",
           (unsigned)(TOUCH_WIDTH / 2U), (unsigned)(TOUCH_HEIGHT / 2U),
           (kx * (float)raw_x[4] + bx_f), (ky * (float)raw_y[4] + by_f));

    printf("[TOUCH] kx=%.4f ky=%.4f bx=%d by=%d\r\n",
           calibration_params.kx, calibration_params.ky,
           (int)calibration_params.bx, (int)calibration_params.by);

    if (!Touch_SaveCalibration())
    {
        printf("[TOUCH] save failed, calibration kept in RAM only\r\n");
        return false;
    }

    printf("[TOUCH] calibration saved to /data/%s.bin\r\n", TOUCH_CAL_NAME);

    return true;
}
