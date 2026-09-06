#ifndef __LOG_H__
#define __LOG_H__

/* ================ 操作日志 ================
 * Log_Op: 一次性整行输出(经 printf → USART2 DMA), 无时间戳。
 *  - 单行原子输出(复用 printf 行互斥), 不穿插。
 *  - 需自行以 "\r\n" 结尾。
 * 调用时机: 任务上下文(不可在 ISR 中调用, 与 printf 一致)。
 */
void Log_Op(const char *fmt, ...);

#endif /* __LOG_H__ */
