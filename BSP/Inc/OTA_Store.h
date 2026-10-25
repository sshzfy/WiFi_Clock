#ifndef __OTA_STORE_H__
#define __OTA_STORE_H__

#include <stdint.h>

int OTA_Store_EnsureDir(void);
int OTA_Store_OpenWrite(const char *path);
int OTA_Store_Write(const uint8_t *buf, uint32_t len);
int OTA_Store_Close(void);

int OTA_Store_Rename(const char *old_path, const char *new_path);
int OTA_Store_Remove(const char *path);
int OTA_Store_FileSize(const char *path, uint32_t *size);

uint32_t OTA_Store_FreeBytes(void);

#endif
