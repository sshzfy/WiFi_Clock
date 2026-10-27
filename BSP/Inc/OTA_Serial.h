#ifndef __OTA_SERIAL_H__
#define __OTA_SERIAL_H__

#include <stdint.h>

void OTA_Serial_Init(void);

uint16_t OTA_Serial_Available(void);
int OTA_Serial_ReadByte(void);
uint16_t OTA_Serial_Read(uint8_t *buf, uint16_t len);
void OTA_Serial_Flush(void);
uint16_t OTA_Serial_OverflowCount(void);

void OTA_Serial_Send(const uint8_t *buf, uint16_t len);

#endif /* __OTA_SERIAL_H__ */
