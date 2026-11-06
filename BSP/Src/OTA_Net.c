#include "OTA_Net.h"
#include "BuildConfig.h"
#include "Secrets.h"

#include "Usart.h"
#include "Timer.h"
#include "stm32f4xx.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define OTA_NET_SSL_PORT 443U        // SSL 端口
#define OTA_NET_HTTP_PORT 80U        // HTTP 端口
#define OTA_NET_CONN_TIMEOUT 15000U  // CIPSTART 等待
#define OTA_NET_SEND_RETRY 3U        // CIPSEND 重试(连接/握手未就绪)
#define OTA_NET_PROMPT_TIMEOUT 5000U // 等 CIPSEND 的 '>'
#define OTA_NET_BODY_TIMEOUT 10000U  // 两次数据间的最大间隔
#define OTA_NET_TOTAL_TIMEOUT 60000U // 整次会话总超时
#define OTA_NET_HDR_MAX 2048U        // 响应头最大字节数
#define OTA_NET_REDIRECT_MAX 3U      // 302 最大跟随次数
#define OTA_NET_CLOSE_WAIT_MS 50U    // CIPCLOSE 后等待
#define OTA_NET_LINK_SETTLE_MS 100U  // CIPSTART 回 OK 后给链路的余量

/* ================ 会话状态 ================ */
static char s_hdr[OTA_NET_HDR_MAX]; // 响应头缓冲
static uint32_t s_hdr_len;          // 响应头已收字节数

static ota_http_resp_t s_resp; // 解析结果
static ota_net_sink_t s_sink;  // 数据回调
static void *s_sink_arg;       // 数据回调参数
static uint32_t s_body_got;    // 已交给回调的字节数
static bool s_hdr_done;        // \r\n\r\n 已到
static bool s_finished;        // 响应体收完
static bool s_aborted;         // 回调请求中止

/* AT 原始回复记录（连接失败时打印，便于定位模组差异） */
static char s_at_log[256];    // AT 原始回复记录
static uint32_t s_at_log_len; // AT 原始回复记录长度

/* ================ 底层收发 ================ */

/** @brief 向 AT 模组发送原始字节（轮询 TXE） */
static bool Net_SendRaw(const char *data, uint32_t len)
{
    uint32_t i;

    for (i = 0U; i < len; i++)
    {
        uint64_t t0 = TIM5_Get_ms();
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET)
        {
            if (TIM5_Get_ms() - t0 > 1000U)
                return false; /* 1 秒保护: USART1 异常时不死等 */
        }
        USART_SendData(USART1, (uint16_t)(uint8_t)data[i]);
    }

    return true;
}

/** @brief 发送一行 AT 命令 */
static bool Net_SendCmd(const char *cmd)
{
    if (!Net_SendRaw(cmd, (uint32_t)strlen(cmd)))
        return false;

    return Net_SendRaw("\r\n", 2U);
}

/** @brief 清空 AT 回复记录 */
static void Net_LogReset(void)
{
    s_at_log_len = 0U;
    s_at_log[0] = '\0';
}

/** @brief 追加一个收到的字节，超出丢弃 */
static void Net_LogChar(char c)
{
    if (s_at_log_len < sizeof(s_at_log) - 1U)
    {
        s_at_log[s_at_log_len++] = c;
        s_at_log[s_at_log_len] = '\0';
    }

#if (DEBUG_SUPPORT == 0)
    (void)s_at_log[0];
#endif
}

/**
 * @brief  等待子串在接收流中出现
 * @param  str        待匹配子串
 * @param  timeout_ms 超时时间
 * @param  log        true = 同时记录模组原始回复（诊断/失败定位用）
 * @return true = 找到子串；false = 超时
 * @note   超时判断必须在读数据之前，否则数据持续到达时超时分支会被饿死。
 */
static bool Net_WaitStr(const char *str, uint32_t timeout_ms, bool log)
{
    uint32_t matched = 0U; // 已匹配的前缀长度
    uint64_t start = TIM5_Get_ms();

    while (true)
    {
        int ch;

        if (TIM5_Get_ms() - start >= timeout_ms)
            return false;

        ch = Usart1_RX_Read();
        if (ch < 0)
        {
            vTaskDelay(1);
            continue;
        }

        if (log)
            Net_LogChar((char)ch);

        if ((char)ch == str[matched])
        {
            matched++;
            if (str[matched] == '\0')
                return true;
        }
        else if ((char)ch == str[0])
        {
            matched = 1U;
        }
        else
        {
            matched = 0U;
        }
    }
}

/**
 * @brief  连接失败后的网络诊断：固件版本 / PING / TCP 80
 * @param  host 目标主机
 * @note   结果全部打进日志，用于区分"DNS 解析失败 / 端口不通 /
 *         TLS 握手失败 / 模组能力不足"。诊断会占用数秒。
 */
static void Net_Diagnose(const char *host)
{
    static char cmd[96];

    /* PING: 同时验证 DNS 与网络连通性 */
    Net_LogReset();
    Usart1_RX_Flush();
    snprintf(cmd, sizeof(cmd), "AT+PING=\"%s\"", host);
    (void)Net_SendCmd(cmd);
    (void)Net_WaitStr("OK", 8000U, true);
    DBG_PRINTF("[OTA] PING: %s\r\n", s_at_log);

    /* TCP 80: 排除 TLS 因素，只看裸 TCP 能否建立 */
    Net_LogReset();
    Usart1_RX_Flush();
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",80", host);
    (void)Net_SendCmd(cmd);
    (void)Net_WaitStr("OK", 10000U, true);
    DBG_PRINTF("[OTA] TCP80: %s\r\n", s_at_log);

    Usart1_RX_Flush();
    (void)Net_SendCmd("AT+CIPCLOSE");
    vTaskDelay(pdMS_TO_TICKS(OTA_NET_CLOSE_WAIT_MS));
    Usart1_RX_Flush();
}

/**
 * @brief  建立 SSL/TCP 连接并发送 GET 请求
 * @param  host    主机名或 IP
 * @param  port    端口
 * @param  use_ssl true = SSL 连接，false = 明文 TCP
 * @param  path    路径
 * @return OTA_NET_OK = 请求已发出；其余为错误
 * @note   连接建立后不等待"发送成功"确认，直接进入接收；
 *         模组对 CIPSEND 的 OK/SEND OK 回执在接收阶段被忽略。
 */
static ota_net_err_t Net_ConnectAndSend(const char *host, uint16_t port,
                                        bool use_ssl, const char *path)
{
    static char cmd[96];
    static char req[384];
    static char hdr_host[160];
    int req_len;

    /* 单连接模式 + 清理可能残留的链路 */
    Usart1_RX_Flush();
    if (!Net_SendCmd("AT+CIPMUX=0"))
        return OTA_NET_ERR_SEND;
    (void)Net_WaitStr("OK", 2000U, false);
    Usart1_RX_Flush();

    /* 无连接时把链路清掉，结果与残留应答都由 Flush 收尾。 */
    (void)Net_SendCmd("AT+CIPCLOSE");
    vTaskDelay(pdMS_TO_TICKS(OTA_NET_CLOSE_WAIT_MS));
    Usart1_RX_Flush();

    /* 建立连接（SSL / 明文 TCP 两种语法） */
    bool connected;

    Net_LogReset();
    if (use_ssl)
        snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"SSL\",\"%s\",%u", host, port);
    else
        snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%u", host, port);

    if (!Net_SendCmd(cmd))
        return OTA_NET_ERR_SEND;

    connected = Net_WaitStr("OK", OTA_NET_CONN_TIMEOUT, true);

    if (!connected)
    {
        DBG_PRINTF("[OTA] CIPSTART no OK, AT: %s\r\n", s_at_log);
        Net_Diagnose(host);
        return OTA_NET_ERR_CONNECT;
    }
    vTaskDelay(pdMS_TO_TICKS(OTA_NET_LINK_SETTLE_MS));
    Usart1_RX_Flush();

    /* 构造请求 */
    if ((use_ssl && port == OTA_NET_SSL_PORT) || (!use_ssl && port == OTA_NET_HTTP_PORT))
        snprintf(hdr_host, sizeof(hdr_host), "Host: %s\r\n", host);
    else
        snprintf(hdr_host, sizeof(hdr_host), "Host: %s:%u\r\n", host, port);

    req_len = snprintf(req, sizeof(req),
                       "GET %s HTTP/1.1\r\n"
                       "%s"
                       "User-Agent: WiFi_Clock-OTA/1.0\r\n"
                       "Connection: close\r\n"
                       "\r\n",
                       path, hdr_host);

    if (req_len <= 0 || (uint32_t)req_len >= sizeof(req))
        return OTA_NET_ERR_PARAM;

    /* CIPSEND（未就绪时重试） */
    uint32_t try_cnt;
    bool sent = false;

    for (try_cnt = 0U; try_cnt < OTA_NET_SEND_RETRY; try_cnt++)
    {
        char cipsend[64];

        Usart1_RX_Flush();
        snprintf(cipsend, sizeof(cipsend), "AT+CIPSEND=%d", req_len);
        if (!Net_SendCmd(cipsend))
            return OTA_NET_ERR_SEND;

        /* 等 '>' 提示符 */
        if (Net_WaitStr(">", OTA_NET_PROMPT_TIMEOUT, false))
        {
            sent = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!sent)
        return OTA_NET_ERR_SEND;

    return Net_SendRaw(req, (uint32_t)req_len) ? OTA_NET_OK : OTA_NET_ERR_SEND;
}

/* ================ HTTP 响应解析 ================ */

/** @brief 不区分大小写的"以 prefix 开头"判断
 *  @param  s      待匹配字符串
 *  @param  prefix 前缀字符串
 *  @return true = 匹配；false = 不匹配
 */
static bool Net_StrIStartsWith(const char *s, const char *prefix)
{
    if (s == NULL || prefix == NULL)
        return false;

    while (*prefix != '\0')
    {
        char a = *s;
        char b = *prefix;

        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');
        if (a != b)
            return false;

        s++;
        prefix++;
    }

    return true;
}

/** @brief 重置会话
 *  @param  sink 数据回调
 *  @param  arg  数据回调参数
 */
static void Http_Reset(ota_net_sink_t sink, void *arg)
{
    memset(&s_resp, 0, sizeof(s_resp));
    s_hdr_len = 0U;
    s_sink = sink;
    s_sink_arg = arg;
    s_body_got = 0U;
    s_hdr_done = false;
    s_finished = false;
    s_aborted = false;
}

/** @brief 解析响应头（状态码 / Content-Length / Location） */
static void Http_ParseHeader(void)
{
    uint32_t i = 0U; // 当前行偏移
    char *line;      // 当前行行首
    char *eol;       // 行尾

    /* 状态行: HTTP/1.1 200 OK */
    eol = strstr(s_hdr, "\r\n");
    if (eol == NULL)
        return;

    *eol = '\0';
    if (strncmp(s_hdr, "HTTP/", 5U) == 0)
    {
        char *sp = strchr(s_hdr, ' ');
        if (sp != NULL)
            s_resp.status_code = atoi(sp); // 取状态码
    }
    *eol = '\r';
    i = (uint32_t)(eol - s_hdr) + 2U; // 绝对偏移, 否则 i 来回跳

    /* 字段行 */
    while (i < s_hdr_len)
    {
        eol = strstr(s_hdr + i, "\r\n"); // 下一行的行尾
        if (eol == NULL)
            break;

        *eol = '\0';
        line = s_hdr + i;

        if (Net_StrIStartsWith(line, "Content-Length:"))
        {
            s_resp.content_length = (uint32_t)atol(line + 15U);
        }
        else if (Net_StrIStartsWith(line, "Location:"))
        {
            const char *v = line + 9U;
            while (*v == ' ' || *v == '\t')
                v++;
            strncpy(s_resp.location, v, sizeof(s_resp.location) - 1U);
            s_resp.location[sizeof(s_resp.location) - 1U] = '\0';
        }

        *eol = '\r';
        i = (uint32_t)(eol - s_hdr) + 2U;
    }
}

/** @brief 向数据回调送一段 body 数据
 *  @param  data 数据指针
 *  @param  len  数据长度
 */
static bool Http_SinkPush(const uint8_t *data, uint32_t len)
{
    if (s_sink == NULL)
        return true;

    if (!s_sink(data, len, s_sink_arg))
    {
        s_aborted = true;
        return false;
    }

    return true;
}

/** @brief  响应体一字节（header 之后） */
static bool Http_BodyByte(uint8_t c)
{
    if (s_resp.content_length != 0U && s_body_got >= s_resp.content_length)
        return true; /* 已收满, 尾部多余字节忽略 */

    s_body_got++;

    if (s_resp.content_length != 0U && s_body_got >= s_resp.content_length)
        s_finished = true;

    return Http_SinkPush(&c, 1U);
}

/* ================ 接收循环: +IPD 拆包 -> HTTP 状态机 ================ */

/**
 * @brief  会话执行接收循环，直到响应体收完/连接关闭/出错
 * @return OTA_NET_OK 或错误码
 */
static ota_net_err_t Net_RecvLoop(void)
{
    char line[64];          // 当前行缓冲区
    uint32_t line_len = 0U; // 当前行已收字节数

    uint32_t ipd_remain = 0U; // 当前 +IPD 块剩余字节数
    bool ipd_mode = false;    // true = 正在搬 +IPD 数据
    bool closed = false;      // 对端已关闭连接

    uint64_t last_data = TIM5_Get_ms();     // 最后一次收到数据时间
    uint64_t session_start = TIM5_Get_ms(); // 会话总超时起点

    /* 已收到完整头且无 body，立即结束 */
    if (s_hdr_done && s_resp.content_length == 0U)
        s_finished = true;

    while (true)
    {
        int ch;

        /* 超时退出 */
        if (TIM5_Get_ms() - session_start >= OTA_NET_TOTAL_TIMEOUT)
            return OTA_NET_ERR_TIMEOUT;

        ch = Usart1_RX_Read();

        if (ch < 0)
        {
            if (s_finished || s_aborted)
                break;
            if (closed)
                return OTA_NET_ERR_CLOSED;
            if (TIM5_Get_ms() - last_data >= OTA_NET_BODY_TIMEOUT)
                return OTA_NET_ERR_TIMEOUT;
            vTaskDelay(1);
            continue;
        }

        last_data = TIM5_Get_ms();

        if (ipd_mode)
        {
            /* +IPD 数据字节 -> HTTP 状态机 */
            if (!s_hdr_done)
            {
                if (s_hdr_len < OTA_NET_HDR_MAX - 1U)
                {
                    s_hdr[s_hdr_len++] = (char)ch;
                    s_hdr[s_hdr_len] = '\0'; // strstr 需要字符串结尾
                }

                /* 检测 \r\n\r\n */
                if (s_hdr_len >= 4U &&
                    s_hdr[s_hdr_len - 4U] == '\r' && s_hdr[s_hdr_len - 3U] == '\n' &&
                    s_hdr[s_hdr_len - 2U] == '\r' && s_hdr[s_hdr_len - 1U] == '\n')
                {
                    s_hdr_done = true;
                    Http_ParseHeader();
                    DBG_PRINTF("[OTA] net: http hdr status=%d len=%u\r\n",
                               s_resp.status_code, (unsigned)s_resp.content_length);
                    if (s_resp.content_length == 0U)
                        s_finished = true;
                }
            }
            else if (!Http_BodyByte((uint8_t)ch))
            {
                s_aborted = true;
                break;
            }

            ipd_remain--;
            if (ipd_remain == 0U)
                ipd_mode = false;
            continue;
        }

        /* 普通字节流：积累行识别关键字 */
        if (line_len < sizeof(line) - 1U)
        {
            line[line_len++] = (char)ch;
            line[line_len] = '\0';
        }
        else
        {
            line_len = 0U; // 行超长: 丢弃重来
            line[0] = '\0';
        }

        /* +IPD, 前缀一到就切数据模式
         * 格式：+IPD,size:data
         */
        if (line_len >= 5U && strncmp(line, "+IPD,", 5U) == 0)
        {
            char *colon = strchr(line + 5U, ':');
            if (colon != NULL)
            {
                char num[16];
                uint32_t nl = (uint32_t)(colon - (line + 5U)); // size 数字长度

                if (nl > sizeof(num) - 1U)
                    nl = sizeof(num) - 1U;
                memcpy(num, line + 5U, nl);
                num[nl] = '\0';

                ipd_remain = (uint32_t)atol(num); // 有 +IPD 数据，进入 +IPD 数据模式
                if (ipd_remain > 0U)
                    ipd_mode = true;

                line_len = 0U;
                line[0] = '\0';
            }
            continue;
        }

        /* 行结束: 检查关键字 */
        if (ch == '\n')
        {
            if (strstr(line, "CLOSED") != NULL)
            {
                closed = true;
                DBG_PRINTF("[OTA] net: CLOSED ipd_remain=%u hdr_len=%u\r\n",
                           (unsigned)ipd_remain, (unsigned)s_hdr_len);
            }
            else if (strstr(line, "ERROR") != NULL)
            {
                return OTA_NET_ERR_PARSE;
            }

            line_len = 0U;
            line[0] = '\0';
        }
    }

    return s_aborted ? OTA_NET_ERR_ABORT : OTA_NET_OK;
}

/** @brief 关闭连接 */
static void Net_Close(void)
{
    Usart1_RX_Flush();
    (void)Net_SendCmd("AT+CIPCLOSE");
    vTaskDelay(pdMS_TO_TICKS(OTA_NET_CLOSE_WAIT_MS));
    Usart1_RX_Flush();
}

/* ================ public API ================ */

/** @brief 发送 HTTP GET 请求
 *  @param host 主机名
 *  @param port 端口
 *  @param use_ssl 是否使用 SSL
 *  @param path 路径
 *  @param resp 响应结构体指针
 *  @param sink 数据接收函数指针
 *  @param arg 数据接收函数参数
 */
ota_net_err_t OTA_Net_HttpGet(const char *host, uint16_t port, bool use_ssl,
                              const char *path,
                              ota_http_resp_t *resp,
                              ota_net_sink_t sink, void *arg)
{
    ota_net_err_t err;

    if (host == NULL || path == NULL || host[0] == '\0')
        return OTA_NET_ERR_PARAM;

    Http_Reset(sink, arg);

    err = Net_ConnectAndSend(host, port, use_ssl, path);
    if (err == OTA_NET_OK)
        err = Net_RecvLoop();
    Net_Close();

    /* 不论成败都回填，避免调用方拿到未初始化或上一次的残留结果 */
    if (resp != NULL)
        *resp = s_resp;

    /* 状态码非 2xx 时把错误留给调用方判断 */
    if (err == OTA_NET_OK && (s_resp.status_code < 200 || s_resp.status_code >= 300))
        err = OTA_NET_ERR_HTTP;

    return err;
}

/** @brief 发送 HTTP GET 请求，支持重定向
 *  @param url 目标 URL
 *  @param final_resp 最终响应结构体指针
 *  @param sink 数据接收函数指针
 *  @param arg 数据接收函数参数
 */
ota_net_err_t OTA_Net_HttpGetUrl(const char *url, ota_http_resp_t *final_resp,
                                 ota_net_sink_t sink, void *arg)
{
    static char cur_url[320];
    uint32_t hop;

    /* 只支持 https://：Gitee 全站 443，省掉 scheme/port 分支 */
    if (url == NULL || strncmp(url, "https://", 8U) != 0)
        return OTA_NET_ERR_PARAM;

    strncpy(cur_url, url, sizeof(cur_url) - 1U);
    cur_url[sizeof(cur_url) - 1U] = '\0';

    for (hop = 0U; hop < OTA_NET_REDIRECT_MAX; hop++)
    {
        char host[128];
        char path[256];
        const char *rest = cur_url + 8U; // 跳过 "https://"
        const char *slash = strchr(rest, '/');
        uint32_t hlen = (slash != NULL) ? (uint32_t)(slash - rest)
                                        : (uint32_t)strlen(rest);
        ota_http_resp_t r;
        ota_net_err_t err;

        if (hlen == 0U || hlen >= sizeof(host))
            return OTA_NET_ERR_PARAM;

        memcpy(host, rest, hlen);
        host[hlen] = '\0';

        if (slash != NULL)
        {
            strncpy(path, slash, sizeof(path) - 1U);
            path[sizeof(path) - 1U] = '\0';
        }
        else
        {
            strcpy(path, "/");
        }

        memset(&r, 0, sizeof(r));

        /* 探测: 不带回调, 避免 3xx 响应体污染下载文件 */
        err = OTA_Net_HttpGet(host, OTA_NET_SSL_PORT, true, path, &r, NULL, NULL);

        if (err == OTA_NET_OK)
        {
            /* 2xx: 对最终 URL 正式请求, 响应体交给回调 */
            return OTA_Net_HttpGet(host, OTA_NET_SSL_PORT, true, path,
                                   final_resp, sink, arg);
        }

        if (err == OTA_NET_ERR_HTTP &&
            (r.status_code == 301 || r.status_code == 302 ||
             r.status_code == 307 || r.status_code == 308) &&
            r.location[0] != '\0')
        {
            /* 组装下一跳 URL: 相对路径沿用当前 host */
            if (r.location[0] == '/')
                snprintf(cur_url, sizeof(cur_url), "https://%s%s", host, r.location);
            else
            {
                strncpy(cur_url, r.location, sizeof(cur_url) - 1U);
                cur_url[sizeof(cur_url) - 1U] = '\0';
            }
            continue;
        }

        return err;
    }

    return OTA_NET_ERR_REDIRECT;
}
