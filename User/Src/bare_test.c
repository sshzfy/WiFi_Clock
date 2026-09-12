#include "bare_test.h"

#if (USE_FREERTOS == 0)

/* ================ 模块测试 ================ */

BM_TEST_MODULE_t BM_TEST_MODULE = BM_TEST_MODULE_LIGHT; // 测试模块选项

/* ================ OLED 测试 ================ */
static void OLED_BareMetal_Test(void)
{
    OLED_Init();
    OLED_Clear();

    OLED_Write_String(0, 0, "OLED TEST OK", &Font_16);

    uint32_t cnt = 0;
    char buf[16];

    while (1)
    {
        cnt++;
        snprintf(buf, sizeof(buf), "CNT=%lu  ", (unsigned long)cnt);
        OLED_Write_String(0, 24, buf, &Font_16);
        delay_ms(500);
    }
}

/* ================ 光敏传感器测试 ================ */

#define LIGHT_THRESHOLD_ON 800   // 开灯阈值 (光线变暗)
#define LIGHT_THRESHOLD_OFF 1000 // 关灯阈值 (光线变亮)

static void Light_Sensor_BareMetal_Test(void)
{
    OLED_Init();
    OLED_Clear();
    Light_Sensor_Init();

    OLED_Write_String(0, 0, "SENSOR TEST OK", &Font_16);

    while (1)
    {
        char buf[16];
#if AO_DO_SWITCH == 1
        uint16_t adc_val = Light_Sensor_Read();
        snprintf(buf, sizeof(buf), "ADC_VAL=%d", adc_val);
#else
        snprintf(buf, sizeof(buf), "DO_STATE=%d", Light_Sensor_DO_State);
        // snprintf(buf, sizeof(buf), "12:20");
#endif
        OLED_Write_String(0, Font_16.size, buf, &Font_16);
        delay_ms(500);
    }
}

void BareMetal_Module_Test(void)
{
    switch (BM_TEST_MODULE)
    {
    case BM_TEST_MODULE_OLED:
        OLED_BareMetal_Test();
        break;
    case BM_TEST_MODULE_LIGHT:
        Light_Sensor_BareMetal_Test();
        break;
    default:
        while (1)
            ;
    }
}

#endif /* USE_FREERTOS == 0 */
