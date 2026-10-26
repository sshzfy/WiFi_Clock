#ifndef __BOOT_LFS_H__
#define __BOOT_LFS_H__

#include <stdint.h>
#include <stdbool.h>

int Boot_Lfs_Mount(void);
bool Boot_Lfs_IsMounted(void);

int Boot_Lfs_FileOpen(const char *path);
int Boot_Lfs_FileRead(void *buf, uint32_t len);
int Boot_Lfs_FileClose(void);

int Boot_Lfs_FileSize(const char *path, uint32_t *size);

#endif /* __BOOT_LFS_H__ */
