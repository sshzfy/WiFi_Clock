#ifndef __PROFILING_H__
#define __PROFILING_H__

#include "main.h"
#include "stm32f4xx.h"
#include <stdint.h>

/* ============================================================
 * 基于 DWT->CYCCNT 的微秒级计时
 * ------------------------------------------------------------
 * 只用于测量渲染耗时(资源迁移前后对比), 不占用定时器, 不产生中断。
 * 使用前必须调用 Prof_Init()。
 * ============================================================ */

void     Prof_Init(void);         /* 使能 DWT 周期计数器, 只需调用一次 */
uint32_t Prof_Cycles(void);       /* 读取当前周期计数 */
uint32_t Prof_Us(uint32_t start); /* 从 start 时刻到现在的微秒数 */

#endif /* __PROFILING_H__ */
