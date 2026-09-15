#ifndef __LFS_PORT_H__
#define __LFS_PORT_H__

#include "lfs.h"
#include "W25Q64.h"
#include "lfs_config.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

extern lfs_t g_lfs;
extern const struct lfs_config g_lfs_config;

int LittleFS_Init(void);
int LittleFS_Format(void);

#endif /* __LFS_PORT_H__ */
