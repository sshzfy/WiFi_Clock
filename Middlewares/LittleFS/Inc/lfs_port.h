#ifndef __LFS_PORT_H__
#define __LFS_PORT_H__

#include "lfs.h"
#include "lfs_config.h"
#include "BuildConfig.h"
#include "W25Q64.h"

#if (USE_FREERTOS == 1)
#include "FreeRTOS.h"
#include "semphr.h"
#endif

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

extern lfs_t g_lfs;
extern const struct lfs_config g_lfs_config;

int LittleFS_Init(void);
int LittleFS_Format(void);

int LittleFS_Mount(void);
int LittleFS_Unmount(void);
bool LittleFS_IsMounted(void);

void LittleFS_Lock(void);
void LittleFS_Unlock(void);

#endif /* __LFS_PORT_H__ */
