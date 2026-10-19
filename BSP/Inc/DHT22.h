#ifndef __DHT22_H__
#define __DHT22_H__

#include "main.h"
#include "stm32f4xx.h"
#include "Timer.h"
#include "FreeRTOS.h"
#include "task.h"

/* DHT22 引脚 */
#define DHT22_PIN        GPIO_Pin_6
#define DHT22_PORT       GPIOE

/* DHT22_ReadData 返回码(0=成功) */
#define DHT22_OK                  0 // 成功 
#define DHT22_ERR_NO_ACK          1 // 总线未拉低, 传感器无应答
#define DHT22_ERR_NO_HIGH         2 // 应答后未拉高(信号畸变)
#define DHT22_ERR_NO_LOW          3 // 数据起始未拉低(信号畸变)
#define DHT22_ERR_TIMEOUT         4 // 位级读取超时(信号中断)
#define DHT22_ERR_CHECKSUM        5 // 校验和不符(数据受干扰)
#define DHT22_ERR_ALL_ZERO        6 // 数据全0: 5字节全0时校验和也是0会骗过校验, 实为传感器没回数据

/* 位判别阈值: 位"0"高电平26~28us, 位"1"高电平70us, 取两者中点40us,
 * 可容忍十几us的中断延迟抖动 */
#define DHT22_BIT_THRESHOLD_US      40U 

typedef struct
{
    float temperature; // 温度, 单位:摄氏度, 保留1位小数
    float humidity;    // 湿度, 单位:%, 保留1位小数
    bool valid;        // 数据有效标志位
} DHT22_Data_t;

bool DHT22_Init(void);
uint8_t DHT22_ReadData(DHT22_Data_t *data);
const char *DHT22_ErrString(uint8_t code);

#endif /* __DHT22_H__ */
