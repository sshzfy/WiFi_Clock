#ifndef __BOOT_UPDATE_H__
#define __BOOT_UPDATE_H__

#include <stdbool.h>

bool Boot_Update_FromFile(const char *path);
bool Boot_Rollback_FromFile(const char *path);

#endif /* __BOOT_UPDATE_H__ */
