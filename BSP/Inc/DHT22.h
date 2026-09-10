#ifndef __DHT22_H__
#define __DHT22_H__

#include "main.h"
#include "Timer.h"

#define DHT22_Pin GPIO_Pin_6
#define DHT22_Port GPIOE

#define DHT22_DATA_OUT_H GPIO_SetBits(DHT22_Port, DHT22_Pin)
#define DHT22_DATA_OUT_L GPIO_ResetBits(DHT22_Port, DHT22_Pin)
#define DHT22_READ_DATA GPIO_ReadInputDataBit(DHT22_Port, DHT22_Pin)

/* DHT22_ReadData 返回码(0=成功) */
#define DHT22_OK            0 /* 成功 0000000*/
#define DHT22_ERR_NO_ACK    1 /* 总线未拉低, 传感器无应答 */
#define DHT22_ERR_NO_HIGH   2 /* 应答后未拉高(信号畸变) */
#define DHT22_ERR_NO_LOW    3 /* 数据起始未拉低(信号畸变) */
#define DHT22_ERR_TIMEOUT   4 /* 位级读取超时(信号中断) */
#define DHT22_ERR_CHECKSUM  5 /* 校验和不符(数据受干扰) */

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
