#ifndef __ATGM336H_H__
#define __ATGM336H_H__

#include "main.h"
#include "Usart.h"

/* ============================================================
 * ATGM336H-5N 北斗+GPS 定位模块: 只取经纬度, 位置变了才通知
 * ------------------------------------------------------------
 * 串口由 Usart.c 提供: USART3 @9600, RX中断 + 512字节环形缓冲
 *   PB10 = USART3_TX → 模块 RXD (可悬空, 只有下发配置命令时才接)
 *   PB11 = USART3_RX ← 模块 TXD
 *
 * 模块固定1Hz发送, 每秒约12条语句(约615字节), 这个频率改不了;
 * 但其中 $xxRMC 每秒只有1条 —— 每秒至多解析1次字段(十几微秒),
 * 因此不必再为"多久解析一次"设节流。
 *
 *   - 字节必须持续收走 —— 512字节缓冲约0.83秒就会满, 不读就丢,
 *     所以 ATGM336H_Poll() 必须**至少每300ms调用一次**;
 *   - 每秒都解析RMC, 但 ATGM_EV_UPDATE 只在**位置真的变了或定位
 *     状态翻转**时置位: 静止时不打扰上层, 移动时立刻响应。
 *
 * 刷屏/上报的周期由调用方掌握 —— 每收到一个UPDATE就printf是毫秒级
 * 开销(115200bps下一行约3ms), 那才是真正需要节流的地方。
 *
 * 线程安全: 中断只写 Usart.c 的环形缓冲; gps_info 只由 ATGM336H_Poll()
 *           写。因此只要只有一个任务调用Poll, 读结构体就无需临界区。
 * ============================================================ */

/* 位置变化判定: 经纬度变化超过这个量(度)才算"位置更新了"。
 * 1e-6度 ≈ 0.11米, 0.05度 ≈ 5.5km, 仅在城市变化时才触发通知。 */
#define ATGM_MOVE_EPS 0.05f

#define ATGM_DEBUG_ECHO 0  // 联调开关: 1 = 把"位置有变化的那条"RMC原文打到调试串口
#define ATGM_LINE_MAX 128U // 单行最大长度: NMEA最长的GSV帧约80字节, 这里留足余量

/* ATGM336H_Poll() 的返回值, 可按位或 */
#define ATGM_EV_RMC 0x01U    // 收到一个校验通过的RMC帧
#define ATGM_EV_UPDATE 0x02U // 位置变化或定位状态翻转, 可重新读取

/* 定位结果。时间/卫星数等一律不提供, 只有经纬度 */
typedef struct
{
    bool valid;      // 定位是否有效(RMC的status == 'A')
    float latitude;  // 十进制度, 南纬为负
    float longitude; // 十进制度, 西经为负
} ATGM_Info_t;

void ATGM336H_Init(void);
uint8_t ATGM336H_Poll(void);
bool ATGM336H_GetInfo(ATGM_Info_t *out);

#endif /* __ATGM336H_H__ */
