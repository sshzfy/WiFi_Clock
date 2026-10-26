#ifndef __OTA_NET_H__
#define __OTA_NET_H__

/* ============================================================
 * OTA_Net —— ESP-AT 模块上的 SSL/TCP + HTTP 流式客户端
 * ------------------------------------------------------------
 * 为什么不用现成的 AT+HTTPCLIENT？
 *   HTTPCLIENT 把整个响应缓存在模组里一次性吐出，固件 150 KB
 *   远超其缓冲；本模块用 AT+CIPSTART(SSL) + AT+CIPSEND 手写
 *   HTTP 请求，边收边处理（写 littlefs / 算 CRC / 刷进度）。
 *
 * 依赖约定：
 *   - USART1 环形缓冲（Usart.c）提供字节流；
 *   - 本模块的所有函数必须在同一任务内串行调用（下载期间
 *     不要再发其他 AT 命令，否则应答会互相串扰）。
 * ============================================================ */

#include <stdint.h>
#include <stdbool.h>

/* ---- 错误码 ---- */
typedef enum
{
    OTA_NET_OK = 0,        /* 成功（状态码 2xx 且数据收完） */
    OTA_NET_ERR_PARAM,     /* 参数非法 */
    OTA_NET_ERR_CONNECT,   /* TCP/SSL 连接失败 */
    OTA_NET_ERR_SEND,      /* 请求发送失败 */
    OTA_NET_ERR_TIMEOUT,   /* 等待应答/数据超时 */
    OTA_NET_ERR_HTTP,      /* 状态码非 2xx（3xx 见 REDIRECT） */
    OTA_NET_ERR_PARSE,     /* 响应头解析失败 */
    OTA_NET_ERR_CLOSED,    /* 数据未收完连接被对端关闭 */
    OTA_NET_ERR_ABORT,     /* 数据回调请求中止 */
    OTA_NET_ERR_REDIRECT,  /* 重定向次数超限 */
} ota_net_err_t;

/* ---- 解析后的 HTTP 响应头 ---- */
typedef struct
{
    int      status_code;       /* 200/302/404... */
    uint32_t content_length;    /* Content-Length；0 = 未知 */
    char     location[512];     /* 302/301 的 Location，未遇到则为空串 */
    bool     chunked;           /* Transfer-Encoding: chunked */
} ota_http_resp_t;

/* ---- 数据回调：返回 false 立即中止下载 ---- */
typedef bool (*ota_net_sink_t)(const uint8_t *data, uint32_t len, void *arg);

/**
 * @brief  按主机/端口/协议发 GET，响应体流式交给回调
 * @param  host    主机名或 IP（不含协议前缀）
 * @param  port    端口（443/80/自定义）
 * @param  use_ssl true = AT+CIPSTART="SSL"，false = 明文 TCP
 * @param  path    路径与查询串（以 / 开头）
 * @param  resp    输出：解析后的响应头（可为 NULL）
 * @param  sink    数据回调（可为 NULL，NULL 表示丢弃响应体）
 * @param  arg     回调透传参数
 * @return OTA_NET_OK = 2xx 且收完；其余为错误码
 * @note   连接成功后 Connection: close，收完即断。
 */
ota_net_err_t OTA_Net_HttpGet(const char *host, uint16_t port, bool use_ssl,
                              const char *path,
                              ota_http_resp_t *resp,
                              ota_net_sink_t sink, void *arg);

/**
 * @brief  按完整 URL 发 GET，自动处理 http/https 与 302 跳转
 * @param  url  完整 URL（如 https://github.com/a/b.ota 或
 *              http://192.168.1.10:8000/fw_106.ota）
 * @param  final_resp 输出：最后一跳的响应头
 * @param  sink/arg  数据回调（只喂最后一跳的响应体）
 * @return 同 OTA_Net_HttpGet
 * @note   GitHub 的 release 下载链接会 302 到
 *         objects.githubusercontent.com，自动跟随（最多 3 跳）；
 *         302 响应体不会喂给回调。
 */
ota_net_err_t OTA_Net_HttpGetUrl(const char *url,
                                 ota_http_resp_t *final_resp,
                                 ota_net_sink_t sink, void *arg);

#endif /* __OTA_NET_H__ */
