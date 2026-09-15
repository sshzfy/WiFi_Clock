#include "LFS_Operation.h"

/**
 * @brief 保存 16 位大端读写, 保证存/取使用同一字节序
 * @param p 输出缓冲区，至少 2 字节
 * @param v 待写入的 16 位整数
 * */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);    // 高字节
    p[1] = (uint8_t)(v & 0xFFU); // 低字节
}

/**
 * @brief 从 2 字节缓冲区按大端格式读取 16 位整数。
 * @param p 输入缓冲区，至少 2 字节
 *
 * @return 解析出的 16 位整数
 * */
static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/**
 * @brief 保存图像到 littlefs 文件系统。
 *
 * 文件格式：
 *   +--------+--------+----------------------------+
 *   | width  | height | 像素数据 (RGB565)           |
 *   | 2 字节 | 2 字节 | width*height*2 字节         |
 *   +--------+--------+----------------------------+
 *
 * @param path  目标文件路径
 * @param image 图像结构体指针
 *
 * @return 0 成功；其他值为 littlefs 错误码
 */
int SaveImage(const char *path, const Image_t *image)
{
    lfs_file_t file;             // 文件句柄
    uint8_t header[HEADER_SIZE]; // 文件头缓冲区：width(2) + height(2)
    uint32_t data_size;          // 像素数据大小
    lfs_ssize_t ret;             // 单次读写返回的实际字节数，负数表示错误
    int err;                     // 错误码

    if ((path == NULL) || (image == NULL) || (image->data == NULL))
    {
        printf("SaveImage: invalid argument\n");
        return LFS_ERR_INVAL;
    }

    data_size = (uint32_t)image->width * (uint32_t)image->height * IMAGE_BYTES_PER_PIXEL; // 像素数据大小

    /* 打开文件，写入模式，创建并截断文件 */
    err = lfs_file_open(&g_lfs, &file, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err != LFS_ERR_OK)
    {
        printf("SaveImage: lfs_file_open failed, err = %d\n", err);
        return err;
    }

    /* 文件头: width(2) + height(2), 大端 */
    put_u16(&header[0], image->width);
    put_u16(&header[2], image->height);
    ret = lfs_file_write(&g_lfs, &file, header, HEADER_SIZE);
    if (ret != (lfs_ssize_t)HEADER_SIZE)
    {
        printf("SaveImage: write header failed, ret = %d\n", (int)ret);
        lfs_file_close(&g_lfs, &file);
        return (ret < 0) ? (int)ret : LFS_ERR_IO;
    }

    /* 写入像素数据 */
    ret = lfs_file_write(&g_lfs, &file, image->data, data_size);
    if (ret != (lfs_ssize_t)data_size)
    {
        printf("SaveImage: write data failed, ret = %d, want = %u\n", (int)ret, (unsigned)data_size);
        lfs_file_close(&g_lfs, &file);
        return (ret < 0) ? (int)ret : LFS_ERR_IO;
    }

    /* close 内部会 sync, 可能在这里才暴露空间不足等错误 */
    err = lfs_file_close(&g_lfs, &file);
    if (err != LFS_ERR_OK)
    {
        printf("SaveImage: lfs_file_close failed, err = %d\n", err);
        return err;
    }

    return 0;
}

/**
 * @brief 从 littlefs 文件系统加载图像。
 *
 * 流程：
 *   1. 读取文件头，得到 width / height；
 *   2. 校验宽高合法性，避免畸形文件导致溢出；
 *   3. 检查目标缓冲区大小；
 *   4. 读取像素数据。
 *
 * @param path     源文件路径
 * @param w        输出参数：图像宽度
 * @param h        输出参数：图像高度
 * @param buf      输出缓冲区
 * @param buf_size 输出缓冲区大小
 *
 * @return 0 成功；其他值为 littlefs 错误码
 */
int LoadImage(const char *path, uint16_t *w, uint16_t *h, uint8_t *buf, uint32_t buf_size)
{
    lfs_file_t file;
    uint8_t header[HEADER_SIZE];
    uint32_t data_size;
    lfs_ssize_t ret;
    uint16_t width;
    uint16_t height;
    int err;

    if ((path == NULL) || (w == NULL) || (h == NULL) || (buf == NULL))
    {
        printf("LoadImage: invalid argument\n");
        return LFS_ERR_INVAL;
    }

    /* 打开文件，只读模式 */
    err = lfs_file_open(&g_lfs, &file, path, LFS_O_RDONLY);
    if (err != LFS_ERR_OK)
    {
        printf("LoadImage: lfs_file_open failed, err = %d\n", err);
        return err;
    }

    /* 读取文件头, 校验文件头大小 */
    ret = lfs_file_read(&g_lfs, &file, header, HEADER_SIZE);
    if (ret != (lfs_ssize_t)HEADER_SIZE)
    {
        printf("LoadImage: read header failed, ret = %d\n", (int)ret);
        lfs_file_close(&g_lfs, &file);
        return LFS_ERR_INVAL;
    }

    width = get_u16(&header[0]);
    height = get_u16(&header[2]);

    /* 先校验文件头再算尺寸, 否则 width*height 可能溢出 */
    if ((width == 0U) || (height == 0U) ||
        ((uint32_t)width * (uint32_t)height > IMAGE_PIXEL_MAX))
    {
        printf("LoadImage: bad header, w = %u, h = %u\n", (unsigned)width, (unsigned)height);
        lfs_file_close(&g_lfs, &file);
        return LFS_ERR_INVAL;
    }

    /* 计算像素数据字节数，并检查目标缓冲区是否足够大 */
    data_size = (uint32_t)width * (uint32_t)height * IMAGE_BYTES_PER_PIXEL;
    if (data_size > buf_size)
    {
        printf("LoadImage: data_size(%u) > buf_size(%u)\n", (unsigned)data_size, (unsigned)buf_size);
        lfs_file_close(&g_lfs, &file);
        return LFS_ERR_NOSPC;
    }

    /* 读取像素数据 */
    ret = lfs_file_read(&g_lfs, &file, buf, data_size);
    if (ret != (lfs_ssize_t)data_size)
    {
        printf("LoadImage: read data failed, ret = %d, want = %u\n", (int)ret, (unsigned)data_size);
        lfs_file_close(&g_lfs, &file);
        return (ret < 0) ? (int)ret : LFS_ERR_IO;
    }

    lfs_file_close(&g_lfs, &file);

    /* 所有数据读取成功后再写出宽高，避免失败时留下不完整结果 */
    *w = width;
    *h = height;

    return 0;
}

/**
 * @brief 保存字体二进制数据到 littlefs 文件系统。
 *
 * @param path 目标文件路径
 * @param data 字体数据指针
 * @param size 数据字节数
 *
 * @return 0 成功；其他值为 littlefs 错误码
 *
 * @note 数据本体不解析，原样写入。
 */
int SaveFont(const char *path, const uint8_t *data, uint32_t size)
{
    lfs_file_t file;
    lfs_ssize_t ret;
    int err;

    if ((path == NULL) || (data == NULL) || (size == 0U))
    {
        printf("SaveFont: invalid argument\n");
        return LFS_ERR_INVAL;
    }

    /* 只写 + 创建 + 截断 */
    err = lfs_file_open(&g_lfs, &file, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err != LFS_ERR_OK)
    {
        printf("SaveFont: lfs_file_open failed, err = %d\n", err);
        return err;
    }

    /* 写入字体数据 */
    ret = lfs_file_write(&g_lfs, &file, data, size);
    if (ret != (lfs_ssize_t)size)
    {
        printf("SaveFont: write failed, ret = %d, want = %u\n", (int)ret, (unsigned)size);
        lfs_file_close(&g_lfs, &file);
        return (ret < 0) ? (int)ret : LFS_ERR_IO;
    }

    err = lfs_file_close(&g_lfs, &file);
    if (err != LFS_ERR_OK)
    {
        printf("SaveFont: lfs_file_close failed, err = %d\n", err);
        return err;
    }

    return 0;
}

/**
 * @brief 从 littlefs 文件系统读取字体二进制数据。
 *
 * 流程：
 *   1. 打开文件；
 *   2. 用 lfs_file_size 获取文件实际大小；
 *   3. 若大于缓冲区，返回 LFS_ERR_NOSPC，不做部分读取；
 *   4. 读取全部内容。
 *
 * @param path     源文件路径
 * @param buf      输出缓冲区
 * @param buf_size 输出缓冲区大小
 *
 * @return >= 0 实际读取字节数；< 0 littlefs 错误码
 */
int LoadFont(const char *path, uint8_t *buf, uint32_t buf_size)
{
    lfs_file_t file;
    lfs_ssize_t ret;
    lfs_size_t size;
    int err;

    if ((path == NULL) || (buf == NULL))
    {
        printf("LoadFont: invalid argument\n");
        return LFS_ERR_INVAL;
    }

    /* 只读打开文件 */
    err = lfs_file_open(&g_lfs, &file, path, LFS_O_RDONLY);
    if (err != LFS_ERR_OK)
    {
        printf("LoadFont: lfs_file_open failed, err = %d\n", err);
        return err;
    }

    /* 获取文件实际大小 */
    size = lfs_file_size(&g_lfs, &file);
    if (size > buf_size)
    {
        printf("LoadFont: size > buf_size, size = %u, buf_size = %u\n",
               (unsigned)size, (unsigned)buf_size);
        lfs_file_close(&g_lfs, &file);
        return LFS_ERR_NOSPC;
    }

    /* 读取字体数据 ，一次性读取整个文件 */
    ret = lfs_file_read(&g_lfs, &file, buf, size);
    if (ret != (lfs_ssize_t)size)
    {
        printf("LoadFont: read failed, ret = %d, want = %u\n", (int)ret, (unsigned)size);
        lfs_file_close(&g_lfs, &file);
        return (ret < 0) ? (int)ret : LFS_ERR_IO;
    }

    lfs_file_close(&g_lfs, &file);

    return (int)size; // 返回实际读取字节数
}
