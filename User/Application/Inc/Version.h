#ifndef __VERSION_H__
#define __VERSION_H__

/* App 版本号（唯一版本源）。
 * ------------------------------------------------------------
 * 三处必须同步，否则会出现"固件自认为旧版本、反复提示升级"：
 *   1. 本文件的 MAJOR/MINOR/PATCH；
 *   2. 打包命令 ota_pack.py --version 参数；
 *   3. Gitee Release 的 tag（必须与版本号一致，如 v1.0.7），
 *      且附件名必须是 fw_1.0.7.ota —— 设备按这两条直接拼下载
 *      地址，任何一处不一致都会 404（详见 BuildConfig.h）。
 *
 * APP_VERSION 编码为 major<<16 | minor<<8 | patch，
 * 写入参数区后 Bootloader 用十六进制显示、OTA 用数值比较。
 * 发新版时：先改这里 -> 重新编译 -> 打包 */

#define APP_VERSION_MAJOR        1
#define APP_VERSION_MINOR        0
#define APP_VERSION_PATCH        7

#define APP_VERSION         ((APP_VERSION_MAJOR << 16) | \
                             (APP_VERSION_MINOR << 8)  | \
                             APP_VERSION_PATCH)
#define APP_VERSION_STR     "1.0.7"

#endif /* __VERSION_H__ */
