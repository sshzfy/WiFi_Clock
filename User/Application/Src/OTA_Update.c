#include "OTA_Update.h"

#include "OTA_Net.h"
#include "OTA.h"
#include "OTA_Store.h"
#include "ota_pkg.h"
#include "Version.h"
#include "boot_crc32.h"
#include "boot_layout.h"
#include "lfs.h"
#include "BuildConfig.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>
#include <stdio.h>

/* ============================================================
 * 内部常量
 * ============================================================ */
#define OTA_GITEE_HOST "gitee.com"
#define OTA_GITEE_PORT 443U

/* 最新版本探测路径。服务器回 302，Location 里带 tag：
 *   Location: https://gitee.com/<owner>/<repo>/releases/tag/v1.0.7 */
#define OTA_LATEST_PATH "/" OTA_GITEE_OWNER "/" OTA_GITEE_REPO "/releases/latest"

#define OTA_DL_EXTRA 65536U /* 空间预检: 固件 x2 + 本值 */
#define OTA_TAG_MAX 32U     /* tag 缓冲，"v255.255.255" 足够 */
#define OTA_URL_MAX 256U    /* 附件地址缓冲，实测约 85 字节 */

/* ============================================================
 * 进度共享区
 * ============================================================ */
ota_prog_t g_ota_prog = {0};

/* ============================================================
 * 小工具
 * ============================================================ */

/**
 * @brief  解析版本字符串 "v1.0.6"/"1.0.6" -> 编码 0x010006
 * @param  s     输入字符串
 * @param  ver   输出编码值（可为 NULL）
 * @param  out   输出规范文本（可为 NULL，缓冲至少 16 字节）
 * @return true = 成功
 * @note   版本号只允许三段十进制，每段 <= 255。设备端不做前导零
 *         归一化，所以 tag 必须写成 v1.0.7 这种形式。
 */
static bool Ver_Parse(const char *s, uint32_t *ver, char *out)
{
    unsigned a, b, c;

    if (sscanf(s, "v%u.%u.%u", &a, &b, &c) != 3 &&
        sscanf(s, "V%u.%u.%u", &a, &b, &c) != 3 &&
        sscanf(s, "%u.%u.%u", &a, &b, &c) != 3)
        return false;
    if (a > 255U || b > 255U || c > 255U)
        return false;

    if (ver != NULL)
        *ver = (a << 16) | (b << 8) | c;
    if (out != NULL)
        sprintf(out, "%u.%u.%u", a, b, c);

    return true;
}

/**
 * @brief  从 302 的 Location 里抠出最新 tag
 * @param  loc     Location 头内容，形如
 *                 https://gitee.com/o/r/releases/tag/v1.0.7
 * @param  tag     输出 tag 文本（保留 'v' 前缀）
 * @param  tag_len 输出缓冲大小
 * @return true = 提取成功
 * @note   只认 "/releases/tag/" 之后的第一段，遇到 '?' '#' '/'
 *         或字符串结尾就停，避免把查询参数带进文件名。
 */
static bool OTA_ParseTag(const char *loc, char *tag, uint32_t tag_len)
{
    static const char key[] = "/releases/tag/";
    const char *p;
    uint32_t n = 0U;

    if (loc == NULL || tag == NULL || tag_len < 3U)
        return false;

    p = strstr(loc, key);
    if (p == NULL)
        return false;
    p += sizeof(key) - 1U;

    while (*p != '\0' && *p != '?' && *p != '#' && *p != '/' &&
           n + 1U < tag_len)
    {
        tag[n++] = *p++;
    }
    tag[n] = '\0';

    return n > 1U; /* 至少 "v1" 才值得继续解析 */
}

/**
 * @brief  按 Gitee 发布约定拼出附件下载地址
 * @param  tag     原始 tag，如 "v1.0.7"
 * @param  tag_ver 规范化版本，如 "1.0.7"
 * @param  url     输出缓冲
 * @param  url_len 缓冲大小
 * @return true = 成功
 * @note   约定格式：
 *           https://gitee.com/<owner>/<repo>/releases/download/<tag>/fw_<ver>.ota
 *         例：tag=v1.0.7 -> fw_1.0.7.ota
 */
static bool OTA_BuildAssetUrl(const char *tag, const char *tag_ver,
                              char *url, uint32_t url_len)
{
    int n = snprintf(url, url_len,
                     "https://" OTA_GITEE_HOST "/%s/%s/releases/download/%s/fw_%s.ota",
                     OTA_GITEE_OWNER, OTA_GITEE_REPO, tag, tag_ver);

    return (n > 0) && ((uint32_t)n < url_len);
}

/* ============================================================
 * 下载：sink 上下文
 * ============================================================ */
typedef struct
{
    uint8_t hdr[64];  /* .ota 包头缓存（前 64 字节） */
    uint32_t hdr_got; /* 包头已收字节 */
    uint32_t got;     /* 已收总字节 */
    uint32_t expect;  /* 期望总字节（0 = 长度未知） */
    uint32_t crc;     /* payload CRC 累加 */
    uint32_t skipped; /* 头字节跳过计数 */
    bool store_ok;    /* littlefs 写入是否正常 */
} dl_ctx_t;

static bool Dl_Sink(const uint8_t *data, uint32_t len, void *arg)
{
    dl_ctx_t *ctx = (dl_ctx_t *)arg;

    /* 超长防护（expect=0 表示长度未知，不限制） */
    if (ctx->expect != 0U && ctx->got + len > ctx->expect)
        return false;

    /* 缓存包头 */
    {
        uint32_t need = (ctx->hdr_got < 64U) ? (64U - ctx->hdr_got) : 0U;
        uint32_t take = (need > len) ? len : need;
        if (take > 0U)
        {
            memcpy(ctx->hdr + ctx->hdr_got, data, take);
            ctx->hdr_got += take;
        }
    }

    /* 写入 littlefs */
    if (OTA_Store_Write(data, len) != (int)len)
    {
        ctx->store_ok = false;
        return false;
    }

    /* payload CRC：跳过 64 B 包头 */
    if (ctx->skipped < 64U)
    {
        uint32_t skip = 64U - ctx->skipped;
        if (len <= skip)
        {
            ctx->skipped += len;
        }
        else
        {
            ctx->crc = Boot_CRC32_Update(ctx->crc, data + skip, len - skip);
            ctx->skipped = 64U;
        }
    }
    else
    {
        ctx->crc = Boot_CRC32_Update(ctx->crc, data, len);
    }

    ctx->got += len;
    g_ota_prog.got = ctx->got;

    return true;
}

/* ============================================================
 * 主流程
 * ============================================================ */

/**
 * @brief  执行一次"检查并更新"
 * @note   阻塞运行，成功路径不复返（OTA_CommitFirmware 内部复位）。
 *
 *         与旧实现的关键差别：版本比较提前到下载之前。旧版是
 *         "先下 160 KB 再比版本"，已是最新也要空跑 45 秒；现在
 *         只读一个 302 头（56 字节），几秒就能判定。
 */
void OTA_Update_Run(void)
{
    /* 大缓冲用 static：本流程只在 netTask 单线程执行，放 .bss 可把
     * 约 700 B 从任务栈挪走（栈曾在 1KB 时溢出导致死机） */
    static ota_http_resp_t resp;
    static dl_ctx_t ctx;
    static char tag[OTA_TAG_MAX];
    static char tag_ver[16];
    static char url[OTA_URL_MAX];

    ota_net_err_t err;
    ota_hdr_t *hdr;
    uint32_t latest_ver = 0U;

    memset(&resp, 0, sizeof(resp));
    memset(&ctx, 0, sizeof(ctx));

    /* ---- 1. 探测最新 tag ---- */
    g_ota_prog.state = OTA_UPD_CHECKING;
    g_ota_prog.total = 0U;
    g_ota_prog.got = 0U;

    DBG_PRINTF("[OTA] probe %s\r\n", OTA_LATEST_PATH);

    err = OTA_Net_HttpGet(OTA_GITEE_HOST, OTA_GITEE_PORT, true,
                          OTA_LATEST_PATH, &resp, NULL, NULL);

    /* 302 是预期结果，底层会返回 OTA_NET_ERR_HTTP，
     * 但 resp 已完整填充，所以以 status_code 为准。 */
    if (err != OTA_NET_OK && err != OTA_NET_ERR_HTTP)
    {
        DBG_PRINTF("[OTA] probe err=%d\r\n", (int)err);
        g_ota_prog.state = OTA_UPD_ERR_NET;
        return;
    }

    DBG_PRINTF("[OTA] probe http=%d clen=%u\r\n",
               resp.status_code, (unsigned)resp.content_length);

    if (resp.status_code < 300 || resp.status_code >= 400 ||
        resp.location[0] == '\0')
    {
        /* 没拿到跳转：要么 Gitee 改了行为，要么仓库/网络异常 */
        DBG_PRINTF("[OTA] no redirect, loc=%s\r\n", resp.location);
        g_ota_prog.state = OTA_UPD_ERR_JSON;
        return;
    }

    if (!OTA_ParseTag(resp.location, tag, sizeof(tag)) ||
        !Ver_Parse(tag, &latest_ver, tag_ver))
    {
        DBG_PRINTF("[OTA] bad tag in loc\r\n");
        g_ota_prog.state = OTA_UPD_ERR_JSON;
        return;
    }

    DBG_PRINTF("[OTA] latest tag=%s ver=%s\r\n", tag, tag_ver);

    /* ---- 2. 版本比较：不新就直接结束，不下载 ---- */
    if (latest_ver <= APP_VERSION)
    {
        DBG_PRINTF("[OTA] up to date (cur=0x%06X)\r\n", (unsigned)APP_VERSION);
        g_ota_prog.state = OTA_UPD_UP_TO_DATE;
        return;
    }

    /* ---- 3. 按约定拼附件地址 ---- */
    if (!OTA_BuildAssetUrl(tag, tag_ver, url, sizeof(url)))
    {
        g_ota_prog.state = OTA_UPD_ERR_NO_ASSET;
        return;
    }
    DBG_PRINTF("[OTA] asset %s\r\n", url);

    /* ---- 4. 空间预检：固件 + 备份 + 余量 ---- */
    if (OTA_Store_FreeBytes() < (APP_FW_SIZE_FALLBACK * 2U + OTA_DL_EXTRA))
    {
        g_ota_prog.state = OTA_UPD_ERR_SPACE;
        return;
    }

    /* ---- 5. 下载 ---- */
    (void)OTA_Store_Remove(OTA_FW_TMP_PATH);
    if (OTA_Store_OpenWrite(OTA_FW_TMP_PATH) != LFS_ERR_OK)
    {
        g_ota_prog.state = OTA_UPD_ERR_STORE;
        return;
    }

    ctx.expect = 0U; /* 长度未知，边收边算 */
    ctx.store_ok = true;
    ctx.crc = 0xFFFFFFFFUL; /* CRC32 初值，缺了会校验失败 */

    g_ota_prog.state = OTA_UPD_DOWNLOADING;
    g_ota_prog.total = 0U;
    g_ota_prog.got = 0U;

    err = OTA_Net_HttpGetUrl(url, &resp, Dl_Sink, &ctx);
    (void)OTA_Store_Close();

    DBG_PRINTF("[OTA] dl err=%d got=%u clen=%u http=%d\r\n",
               (int)err, (unsigned)ctx.got,
               (unsigned)resp.content_length, resp.status_code);

    if (err != OTA_NET_OK || !ctx.store_ok || ctx.got < OTA_PKG_HEADER_SIZE)
    {
        (void)OTA_Store_Remove(OTA_FW_TMP_PATH);
        g_ota_prog.state = (err == OTA_NET_OK) ? OTA_UPD_ERR_HTTP : OTA_UPD_ERR_NET;
        return;
    }

    /* ---- 6. 包头静态校验（含"附件版本必须与 tag 一致"） ---- */
    hdr = (ota_hdr_t *)ctx.hdr;
    if (hdr->magic != OTA_PKG_MAGIC ||
        hdr->header_size != OTA_PKG_HEADER_SIZE ||
        hdr->hw_id != OTA_PKG_HW_ID ||
        hdr->fw_size != ctx.got - OTA_PKG_HEADER_SIZE ||
        hdr->fw_version != latest_ver)
    {
        DBG_PRINTF("[OTA] bad hdr (pkg ver=0x%06X tag ver=0x%06X)\r\n",
                   (unsigned)hdr->fw_version, (unsigned)latest_ver);
        (void)OTA_Store_Remove(OTA_FW_TMP_PATH);
        g_ota_prog.state = OTA_UPD_ERR_CRC;
        return;
    }

    DBG_PRINTF("[OTA] hdr ver=0x%06X size=%u\r\n",
               (unsigned)hdr->fw_version, (unsigned)hdr->fw_size);

    /* ---- 7. payload CRC ---- */
    if ((~ctx.crc) != hdr->fw_crc32)
    {
        DBG_PRINTF("[OTA] crc mismatch\r\n");
        (void)OTA_Store_Remove(OTA_FW_TMP_PATH);
        g_ota_prog.state = OTA_UPD_ERR_CRC;
        return;
    }

    /* ---- 8. 提交：改名/备份/写参数/复位（成功不复返） ---- */
    g_ota_prog.state = OTA_UPD_APPLYING;
    if (OTA_CommitFirmware(hdr->fw_size, hdr->fw_crc32, hdr->fw_version) != OTA_OK)
    {
        g_ota_prog.state = OTA_UPD_ERR_BACKUP;
    }
}
