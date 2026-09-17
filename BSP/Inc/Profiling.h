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

void Prof_Init(void);
uint32_t Prof_Cycles(void);
uint32_t Prof_Us(uint32_t start);

#endif /* __PROFILING_H__ */
