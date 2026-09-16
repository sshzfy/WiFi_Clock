#ifndef __LFS_OPERATIONATION_H__
#define __LFS_OPERATIONATION_H__

#include "W25Q64.h"
#include "lfs_port.h"
#include "Image.h"
#include "Font.h"

#define IMAGE_BYTES_PER_PIXEL   2U              // 每像素 2 字节
#define IMAGE_PIXEL_MAX         (1024U * 1024U) // 允许的最大像素数(1M 像素 = 2MB), 用于拒绝畸形文件头
#define HEADER_SIZE             4U              // 头部大小，用于存储文件头信息

int SaveImage(const char *filename, const uint8_t *data, uint16_t w, uint16_t h);
int LoadImage(const char *filename, uint16_t *w, uint16_t *h, uint8_t *buf, uint32_t buf_size);
int SaveFont(const char *filename, const uint8_t *data, uint32_t size);
int LoadFont(const char *filename, uint8_t *buf, uint32_t buf_size);

#endif /* __LFS_OPERATIONATION_H__ */
