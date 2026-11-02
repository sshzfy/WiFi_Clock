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

/* 内部常量 */
#define OTA_NET_SSL_PORT 443U
#define OTA_NET_CONN_TIMEOUT 15000U  // CIPSTART 等待
#define OTA_NET_SEND_RETRY 3U        // CIPSEND 重试(连接/握手未就绪)
#define OTA_NET_PROMPT_TIMEOUT 5000U // 等 CIPSEND 的 '>'
#define OTA_NET_BODY_TIMEOUT 10000U  // 两次数据间的最大间隔
#define OTA_NET_TOTAL_TIMEOUT 60000U // 整次会话总超时(ms)
#define OTA_NET_HDR_MAX 4096U        // 响应头最大字节数（Gitee 的 302 头可超 1 KB）
#define OTA_NET_REDIRECT_MAX 3U      // 302 最大跟随次数

/* 会话状态 */
static char s_hdr[OTA_NET_HDR_MAX]; // 响应头缓冲
static uint32_t s_hdr_len;          // 响应头已收字节数

static ota_http_resp_t s_resp; // 解析结果
static ota_net_sink_t s_sink;
static void *s_sink_arg;
static uint32_t s_body_got; // 已交给回调的字节数
static bool s_hdr_done;     // \r\n\r\n 已到
static bool s_finished;     // 响应体收完
static bool s_aborted;      // 回调请求中止

/* chunked: 0=大小行 1=数据 2=数据CR 3=数据LF 4=trailer行 */
static int s_ch_state;
static uint32_t s_ch_remain;
static uint32_t s_ch_size_len;
static char s_ch_size[16];

/* ================ 底层收发 ================ */

/**
 * @brief 向 AT 模组发送原始字节（轮询 TXE，带 1 秒保护）
 * @param data 待发送数据
 * @param len  数据长度
 * @return true = 全部发出；false = TXE 长时间不置位（USART 异常）
 */
static bool Net_SendRaw(const char *data, uint32_t len)
{
    for (uint32_t i = 0U; i < len; i++)
    {
        uint64_t t0 = TIM5_Get_ms();
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET)
        {
            if (TIM5_Get_ms() - t0 > 1000U)
                return false; /* 1 秒保护: USART1 异常时不能死等 */
        }
        USART_SendData(USART1, (uint16_t)(uint8_t)data[i]);
    }

    return true;
}

/** @brief 发送一行 AT 命令
 *  @param cmd 不含 "\r\n"，函数会自动加上
 *  @return true = 发送成功；false = USART 异常
 */
static bool Net_SendCmd(const char *cmd)
{
    if (!Net_SendRaw(cmd, (uint32_t)strlen(cmd)))
        return false;

    return Net_SendRaw("\r\n", 2U);
}

/** @brief 等待子串在接收流中出现（吃掉的字节不返还）
 *  @param str 待匹配子串
 *  @param timeout_ms 超时时间（ms）
 *  @return true = 找到子串；false = 超时
 */
static bool Net_WaitStr(const char *str, uint32_t timeout_ms)
{
    uint32_t matched = 0U; // 已匹配的前缀长度
    uint64_t start = TIM5_Get_ms();

    while (true)
    {
        /* 超时检查放最前：模组持续吐数据时也必须能退出，
         * 否则循环每拍都读到字节，超时永远不触发（实测死机） */
        if (TIM5_Get_ms() - start >= timeout_ms)
            return false;

        int ch = Usart1_RX_Read();
        if (ch < 0)
        {
            vTaskDelay(1);
            continue;
        }

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

/* AT 原始回复记录（失败时打印，便于定位模组差异） */
static char s_at_log[256];
static uint32_t s_at_log_len;

/** @brief 清空 AT 回复记录 */
static void Net_LogReset(void)
{
    s_at_log_len = 0U;
    s_at_log[0] = '\0';
}

/** @brief 追加一个收到的字节（超出丢弃） */
static void Net_LogChar(char c)
{
    if (s_at_log_len < sizeof(s_at_log) - 1U)
    {
        s_at_log[s_at_log_len++] = c;
        s_at_log[s_at_log_len] = '\0';
    }

#if (DEBUG_SUPPORT == 0)
    /* 正式版 DBG_PRINTF 展开为空, s_at_log 变成只写不读, AC5 报 #550-D。
     * 读一次首字节, 让编译器认为它被使用过。 */
    (void)s_at_log[0];
#endif
}

/**
 * @brief 等待子串出现，同时记录模组原始回复
 * @param str 待匹配子串
 * @param timeout_ms 超时
 * @return true = 出现过
 */
static bool Net_WaitStrLogged(const char *str, uint32_t timeout_ms)
{
    uint32_t matched = 0U;
    uint64_t start = TIM5_Get_ms();

    while (true)
    {
        /* 同上：超时检查必须在读数据之前 */
        if (TIM5_Get_ms() - start >= timeout_ms)
            return false;

        {
            int ch = Usart1_RX_Read();
            if (ch < 0)
            {
                vTaskDelay(1);
                continue;
            }

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
}

/* ================ 连接管理 ================ */

/**
 * @brief  连接失败后的网络诊断：固件版本 / PING / TCP 80
 * @param  host 目标主机
 * @note   结果全部打进日志，用于区分"DNS 解析失败 / 端口不通 /
 *         TLS 握手失败 / 模组能力不足"。诊断会占用数秒。
 */
static void Net_Diagnose(const char *host)
{
    static char cmd[96];

    /* 1. 固件版本：确认模组型号与 AT 版本 */
    Net_LogReset();
    Usart1_RX_Flush();
    Net_SendCmd("AT+GMR");
    (void)Net_WaitStrLogged("OK", 3000U);
    DBG_PRINTF("[OTA] GMR: %s\r\n", s_at_log);

    /* 2. PING: 同时验证 DNS 与网络连通性 */
    Net_LogReset();
    Usart1_RX_Flush();
    snprintf(cmd, sizeof(cmd), "AT+PING=\"%s\"", host);
    Net_SendCmd(cmd);
    (void)Net_WaitStrLogged("OK", 8000U);
    DBG_PRINTF("[OTA] PING: %s\r\n", s_at_log);

    /* 3. TCP 80: 排除 TLS 因素，只看裸 TCP 能否建立 */
    Net_LogReset();
    Usart1_RX_Flush();
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",80", host);
    Net_SendCmd(cmd);
    (void)Net_WaitStrLogged("OK", 10000U);
    DBG_PRINTF("[OTA] TCP80: %s\r\n", s_at_log);
    Usart1_RX_Flush();
    Net_SendCmd("AT+CIPCLOSE");
    (void)Net_WaitStr("OK", 1000U);
    Usart1_RX_Flush();
}

/**
 * @brief  建立 SSL 连接并发送 GET 请求
 * @param  host 主机名或 IP
 * @param  port 端口（443/80/自定义）
 * @param  use_ssl true = SSL 连接，false = 明文 TCP
 * @param  path 路径
 * @return OTA_NET_OK = 请求已发出；其余为错误
 * @note   连接建立后不等待任何"发送成功"确认，直接进入接收；
 *         模组对 CIPSEND 的 OK/SEND OK 回执在接收阶段被忽略。
 */
static ota_net_err_t Net_ConnectAndSend(const char *host, uint16_t port,
                                        bool use_ssl, const char *path)
{
    /* static：只在 netTask 单线程调用，608 B 缓冲放 .bss 不占任务栈 */
    static char cmd[96];
    static char req[512];
    int req_len;

    DBG_PRINTF("[OTA] net: enter host=%s port=%u ssl=%d\r\n", host, port, (int)use_ssl);

    /* ---- 0. 单连接模式 + 清理可能残留的链路 ---- */
    Usart1_RX_Flush();
    if (!Net_SendCmd("AT+CIPMUX=0"))
        return OTA_NET_ERR_SEND;
    DBG_PRINTF("[OTA] net: mux sent\r\n");
    (void)Net_WaitStr("OK", 2000U);
    DBG_PRINTF("[OTA] net: mux wait done\r\n");
    Usart1_RX_Flush();
    if (!Net_SendCmd("AT+CIPCLOSE")) /* 无连接时回 ERROR, 忽略 */
        return OTA_NET_ERR_SEND;
    (void)Net_WaitStr("OK", 1000U);
    DBG_PRINTF("[OTA] net: close wait done\r\n");
    Usart1_RX_Flush();

    /* ---- 1. 建立连接：按协议选择 AT 语法 ----
     *   SSL : "CIPSTART=\"SSL\""（ESP32-C3 AT 固件语法）
     *   TCP : 直接 "CIPSTART=\"TCP\""（局域网/明文服务器）
     * 失败时打印模组原始回复，便于判断 DNS/网络/TLS 问题。 */
    {
        bool connected;

        Net_LogReset();
        if (use_ssl)
        {
            snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"SSL\",\"%s\",%u", host, port);
        }
        else
        {
            snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%u", host, port);
        }
        DBG_PRINTF("[OTA] net: dial %s\r\n", cmd);
        if (!Net_SendCmd(cmd))
            return OTA_NET_ERR_SEND;
        DBG_PRINTF("[OTA] net: dial waiting OK...\r\n");

        connected = Net_WaitStrLogged("OK", OTA_NET_CONN_TIMEOUT);
        if (!connected)
        {
            DBG_PRINTF("[OTA] CIPSTART no OK, AT: %s\r\n", s_at_log);
            Net_Diagnose(host);
            return OTA_NET_ERR_CONNECT;
        }
    }
    /* 给模组一点时间完成 TCP/TLS 握手 */
    vTaskDelay(pdMS_TO_TICKS(500));
    Usart1_RX_Flush();

    /* ---- 2. 构造请求（非默认端口时 Host 头要带端口） ---- */
    if ((use_ssl && port == 443U) || (!use_ssl && port == 80U))
    {
        req_len = snprintf(req, sizeof(req),
                           "GET %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "User-Agent: WiFi_Clock-OTA/1.0\r\n"
                           "Accept: */*\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           path, host);
    }
    else
    {
        req_len = snprintf(req, sizeof(req),
                           "GET %s HTTP/1.1\r\n"
                           "Host: %s:%u\r\n"
                           "User-Agent: WiFi_Clock-OTA/1.0\r\n"
                           "Accept: */*\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           path, host, port);
    }

    /* ---- 3. CIPSEND（未就绪时重试） ---- */
    {
        uint32_t try_cnt;
        bool sent = false;

        for (try_cnt = 0U; try_cnt < OTA_NET_SEND_RETRY; try_cnt++)
        {
            char cipsend[64];

            Usart1_RX_Flush();
            snprintf(cipsend, sizeof(cipsend), "AT+CIPSEND=%d", req_len);
            DBG_PRINTF("[OTA] net: cipsend try=%u\r\n", (unsigned)try_cnt);
            if (!Net_SendCmd(cipsend))
                return OTA_NET_ERR_SEND;

            /* 等 '>' 提示符（新固件先回 OK 再 '>'，老固件直接 '>'） */
            if (Net_WaitStr(">", OTA_NET_PROMPT_TIMEOUT))
            {
                sent = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (!sent)
            return OTA_NET_ERR_SEND;
    }

    /* ---- 4. 发原始请求字节（其后模组会回 OK/SEND OK，接收阶段忽略） ---- */
    if (!Net_SendRaw(req, (uint32_t)req_len))
        return OTA_NET_ERR_SEND;
    DBG_PRINTF("[OTA] net: request sent, enter recv loop\r\n");

    return OTA_NET_OK;
}

/* ================ HTTP 响应解析 ================*/

/** @brief 不区分大小写的字符串比较（AC5 库兼容） */
static bool Net_StrIStartsWith(const char *s, const char *prefix)
{
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

/** @brief 重置会话 */
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
    s_ch_state = 0;
    s_ch_remain = 0U;
    s_ch_size_len = 0U;
    memset(s_ch_size, 0, sizeof(s_ch_size));
}

/** @brief 解析响应头 */
static void Http_ParseHeader(void)
{
    uint32_t i = 0U;
    char *line = s_hdr;

    /* 状态行: HTTP/1.1 200 OK */
    {
        char *eol = strstr(line, "\r\n");
        if (eol == NULL)
            return;
        *eol = '\0';
        if (strncmp(line, "HTTP/", 5U) == 0)
        {
            char *sp = strchr(line, ' ');
            if (sp != NULL)
                s_resp.status_code = atoi(sp);
        }
        *eol = '\r';
        i = (uint32_t)(eol - s_hdr) + 2U; /* 绝对偏移, 否则 i 来回跳 */
    }

    /* 字段行 */
    while (i < s_hdr_len)
    {
        char *eol = strstr(s_hdr + i, "\r\n");
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
        else if (Net_StrIStartsWith(line, "Transfer-Encoding:"))
        {
            s_resp.chunked = (strstr(line, "chunked") != NULL);
        }

        *eol = '\r';
        i = (uint32_t)(eol - s_hdr) + 2U; /* 绝对偏移, 否则 i 来回跳 */
    }
}

/** @brief 向数据回调送一段 body 数据 */
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

/** @brief 处理 chunked 大小行 */
static void Http_ChunkSizeLine(void)
{
    s_ch_remain = (uint32_t)strtoul(s_ch_size, NULL, 16);

    if (s_ch_remain == 0U)
    {
        s_ch_state = 4; /* 0 块: 等 trailer 空行即结束 */
    }
    else
    {
        s_ch_state = 1; /* 进数据态 */
    }
    s_ch_size_len = 0U;
    memset(s_ch_size, 0, sizeof(s_ch_size));
}

/** @brief 响应体一字节（header 之后） */
static bool Http_BodyByte(uint8_t c)
{
    if (s_resp.chunked)
    {
        switch (s_ch_state)
        {
        case 0: /* 块大小行 */
            if (c == '\r')
                return true;
            if (c == '\n')
            {
                Http_ChunkSizeLine();
                return true;
            }
            if (s_ch_size_len < sizeof(s_ch_size) - 1U)
            {
                s_ch_size[s_ch_size_len++] = (char)c;
            }
            return true;

        case 1: /* 块数据 */
            s_ch_remain--;
            if (!Http_SinkPush(&c, 1U))
                return false;
            if (s_ch_remain == 0U)
                s_ch_state = 2;
            return true;

        case 2: /* 数据后 CR */
            s_ch_state = (c == '\r') ? 3 : 2;
            return true;

        case 3: /* 数据后 LF */
            s_ch_state = (c == '\n') ? 0 : 2;
            return true;

        case 4: /* trailer 行: 空行即结束 */
            if (c == '\n')
            {
                s_finished = true;
            }
            return true;

        default:
            return true;
        }
    }

    /* 非 chunked: 按 Content-Length 计数 */
    if (s_resp.content_length != 0U && s_body_got >= s_resp.content_length)
        return true; /* 已收满, 尾部多余字节忽略 */

    s_body_got++;
    /* 收满即置 finished：服务器随后会 CLOSED，不置位的话
     * 接收循环会把正常结束误判成 ERR_CLOSED */
    if (s_resp.content_length != 0U && s_body_got >= s_resp.content_length)
        s_finished = true;
    return Http_SinkPush(&c, 1U);
}

/* ================ 接收循环：+IPD 拆包 → HTTP 状态机 ================ */

/**
 * @brief  会话执行接收循环，直到响应体收完/连接关闭/出错
 * @return OTA_NET_OK 等错误码
 */
static ota_net_err_t Net_RecvLoop(void)
{
    /* 行缓冲：识别 +IPD,/CLOSED/ERROR 关键字 */
    char line[64];
    uint32_t line_len = 0U;

    /* +IPD 数据模式: s_ipd_remain 为当前块剩余字节数 */
    uint32_t ipd_remain = 0U;
    bool ipd_mode = false;

    /* 连接已被对端关闭 */
    bool closed = false;

    uint64_t last_data = TIM5_Get_ms();     /* 最后一次收到数据 */
    uint64_t session_start = TIM5_Get_ms(); /* 会话总超时起点 */

    /* 已收到完整头且无 body（如 302, Content-Length:0）→ 立即结束 */
    if (s_hdr_done && !s_resp.chunked && s_resp.content_length == 0U)
        s_finished = true;

    while (true)
    {

        /* 总超时：模组持续刷数据时也必须能退出 */
        if (TIM5_Get_ms() - session_start >= OTA_NET_TOTAL_TIMEOUT)
            return OTA_NET_ERR_TIMEOUT;
        int ch = Usart1_RX_Read();

        if (ch < 0)
        {
            if (s_finished || s_aborted)
                break;
            if (closed)
            {
                /* 连接关了但体没收满 */
                if (!s_finished)
                    return OTA_NET_ERR_CLOSED;
                break;
            }
            if (TIM5_Get_ms() - last_data >= OTA_NET_BODY_TIMEOUT)
                return OTA_NET_ERR_TIMEOUT;
            vTaskDelay(1);
            continue;
        }

        last_data = TIM5_Get_ms();

        if (ipd_mode)
        {
            /* ---- +IPD 数据字节 → HTTP 状态机 ---- */
            if (!s_hdr_done)
            {
                /* 响应头阶段 */
                if (s_hdr_len < OTA_NET_HDR_MAX - 1U)
                {
                    s_hdr[s_hdr_len++] = (char)ch;
                    s_hdr[s_hdr_len] = '\0'; /* strstr 需要字符串结尾 */
                }

                /* 检测 \r\n\r\n */
                if (s_hdr_len >= 4U &&
                    s_hdr[s_hdr_len - 4U] == '\r' &&
                    s_hdr[s_hdr_len - 3U] == '\n' &&
                    s_hdr[s_hdr_len - 2U] == '\r' &&
                    s_hdr[s_hdr_len - 1U] == '\n')
                {
                    s_hdr_done = true;
                    Http_ParseHeader();
                    DBG_PRINTF("[OTA] net: http hdr status=%d len=%u chunked=%d\r\n",
                               s_resp.status_code, (unsigned)s_resp.content_length,
                               (int)s_resp.chunked);
                    if (!s_resp.chunked && s_resp.content_length == 0U)
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

        /* ---- 普通字节流：积累行识别关键字 ---- */
        if (line_len < sizeof(line) - 1U)
        {
            line[line_len++] = (char)ch;
            line[line_len] = '\0';
        }
        else
        {
            /* 行超长: 丢掉一半防溢出 */
            line_len = 0U;
            line[0] = '\0';
        }

        /* +IPD, 前缀一到就切数据模式(不等换行) */
        if (strncmp(line, "+IPD,", 5U) == 0 && line_len >= 5U)
        {
            /* 当前行已含 ",len:" 全部数字与冒号? 边收边解析: */
            char *colon = strchr(line + 5U, ':');
            if (colon != NULL)
            {
                char num[16];
                uint32_t nl = (uint32_t)(colon - (line + 5U));
                if (nl > sizeof(num) - 1U)
                    nl = sizeof(num) - 1U;
                memcpy(num, line + 5U, nl);
                num[nl] = '\0';
                ipd_remain = (uint32_t)atol(num);
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
                if (s_hdr_len > 0U)
                {
                    DBG_PRINTF("[OTA] hdr: %.120s\r\n", s_hdr);
                }
            }
            else if (strstr(line, "ERROR") != NULL)
                return OTA_NET_ERR_PARSE;
            line_len = 0U;
            line[0] = '\0';
        }
    }

    if (s_aborted)
        return OTA_NET_ERR_ABORT;

    return OTA_NET_OK;
}

/** @brief 关闭连接（尽力而为，忽略结果） */
static void Net_Close(void)
{
    Usart1_RX_Flush();
    Net_SendCmd("AT+CIPCLOSE");
    (void)Net_WaitStr("OK", 1500U);
    Usart1_RX_Flush();
}

/* ================ public API ================ */

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
    if (err != OTA_NET_OK)
    {
        Net_Close();
        return err;
    }

    err = Net_RecvLoop();
    Net_Close();

    if (resp != NULL)
        *resp = s_resp;

    /* 状态码非 2xx 时把错误留给调用方判断（3xx 由 HttpGetUrl 处理） */
    if (err == OTA_NET_OK && (s_resp.status_code < 200 || s_resp.status_code >= 300))
        err = OTA_NET_ERR_HTTP;

    return err;
}

ota_net_err_t OTA_Net_HttpGetUrl(const char *url, ota_http_resp_t *final_resp,
                                 ota_net_sink_t sink, void *arg)
{
    static char cur_url[320]; /* static: 放 .bss, 不占任务栈 */
    uint32_t hop;

    if (url == NULL)
        return OTA_NET_ERR_PARAM;
    strncpy(cur_url, url, sizeof(cur_url) - 1U);
    cur_url[sizeof(cur_url) - 1U] = '\0';

    for (hop = 0U; hop < OTA_NET_REDIRECT_MAX; hop++)
    {
        char host[128];
        char path[256];
        uint16_t port;
        bool ssl;
        const char *rest;
        const char *slash;
        const char *colon;
        uint32_t hlen;
        ota_http_resp_t r;
        ota_net_err_t err;

        /* ---- 解析 URL: scheme://host[:port]/path ---- */
        if (strncmp(cur_url, "https://", 8U) == 0)
        {
            rest = cur_url + 8U;
            ssl = true;
            port = 443U;
        }
        else if (strncmp(cur_url, "http://", 7U) == 0)
        {
            rest = cur_url + 7U;
            ssl = false;
            port = 80U;
        }
        else
        {
            return OTA_NET_ERR_PARAM;
        }

        slash = strchr(rest, '/');
        hlen = (slash != NULL) ? (uint32_t)(slash - rest) : (uint32_t)strlen(rest);
        if (hlen == 0U || hlen >= sizeof(host))
            return OTA_NET_ERR_PARAM;

        colon = (const char *)memchr(rest, ':', hlen);
        if (colon != NULL)
        {
            port = (uint16_t)atoi(colon + 1);
            hlen = (uint32_t)(colon - rest);
        }

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

        /* ---- 探测: 不带回调, 避免 3xx 响应体污染下载文件 ---- */
        err = OTA_Net_HttpGet(host, port, ssl, path, &r, NULL, NULL);

        if (err == OTA_NET_OK)
        {
            /* 2xx: 对最终 URL 正式请求, 响应体交给回调 */
            return OTA_Net_HttpGet(host, port, ssl, path, final_resp, sink, arg);
        }

        if (err == OTA_NET_ERR_HTTP &&
            (r.status_code == 301 || r.status_code == 302 ||
             r.status_code == 307 || r.status_code == 308) &&
            r.location[0] != '\0')
        {
            /* 组装下一跳 URL: 相对路径沿用当前 host/port/scheme */
            if (r.location[0] == '/')
            {
                snprintf(cur_url, sizeof(cur_url), "%s://%s:%u%s",
                         ssl ? "https" : "http", host, port, r.location);
            }
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
