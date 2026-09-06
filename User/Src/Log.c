#include "Log.h"
#include <stdio.h>
#include <stdarg.h>

/**
 * @brief 打印一条操作日志(整行原子输出, 无时间戳)
 *
 * 注意: 与 printf 用法一致, 需要自行以 "\r\n" 结尾才会换行;
 * 整行由 Usart.c 的行互斥保护, 不会被多任务打印穿插。
 *
 * @param fmt 格式化串(建议ASCII)
 * @return None
 */
void Log_Op(const char *fmt, ...)
{
    char line[196];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    printf("%s", line);
}
