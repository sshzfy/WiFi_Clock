#ifndef __VERSION_H__
#define __VERSION_H__

/* App 版本号（唯一版本源）*/
#define APP_VERSION_MAJOR        1
#define APP_VERSION_MINOR        0
#define APP_VERSION_PATCH        1

#define APP_VERSION        ((APP_VERSION_MAJOR << 16) | \
                            (APP_VERSION_MINOR << 8)  | \
                             APP_VERSION_PATCH)
#define APP_VERSION_STR        "1.0.1"

#endif /* __VERSION_H__ */
