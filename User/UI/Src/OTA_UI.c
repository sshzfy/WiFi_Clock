#include "OTA_UI.h"

#include "OTA_Update.h"
#include "ui.h"
#include "screens/ui_WiFi.h"

#include "FreeRTOS.h"
#include "task.h"

/* ============================================================
 * 状态
 * ============================================================ */
static lv_obj_t *s_scr   = NULL;  /* 浮层屏 */
static lv_obj_t *s_text  = NULL;  /* 状态文本 */
static lv_obj_t *s_bar   = NULL;  /* 进度条 */
static bool      s_active = false; /* 浮层当前是否在屏上 */
static uint32_t  s_term_ms = 0U;   /* 结果状态开始显示的时刻 */

/* ============================================================
 * 实现
 * ============================================================ */

/** @brief 是否为"结果"状态（显示 3 秒后自动关闭） */
static bool State_IsTerminal(uint32_t st)
{
    if (st == OTA_UPD_UP_TO_DATE)
        return true;
    return st >= OTA_UPD_ERR_NET && st <= OTA_UPD_ERR_PARAM;
}

/** @brief 状态 -> 显示文本 */
static const char *State_Text(uint32_t st)
{
    switch (st)
    {
    case OTA_UPD_CHECKING:    return "Checking update...";
    case OTA_UPD_UP_TO_DATE:  return "Already up to date";
    case OTA_UPD_DOWNLOADING: return NULL; /* 动态显示百分比 */
    case OTA_UPD_APPLYING:    return "Verifying & backup...";
    case OTA_UPD_REBOOTING:   return "Rebooting...";
    case OTA_UPD_ERR_NET:     return "Network error";
    case OTA_UPD_ERR_HTTP:    return "HTTP error";
    case OTA_UPD_ERR_JSON:    return "Bad release info";
    case OTA_UPD_ERR_NO_ASSET:return "No .ota in release";
    case OTA_UPD_ERR_CRC:     return "CRC mismatch";
    case OTA_UPD_ERR_STORE:   return "Storage error";
    case OTA_UPD_ERR_SPACE:   return "Not enough space";
    case OTA_UPD_ERR_BACKUP:  return "Backup failed";
    case OTA_UPD_ERR_PARAM:   return "Param write failed";
    default:                  return "Updating...";
    }
}

/**
 * @brief  创建浮层（LVGL 初始化后调用一次）
 */
void OTA_UI_Init(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_scr, 240, 320);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_90, LV_PART_MAIN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    s_text = lv_label_create(s_scr);
    lv_obj_align(s_text, LV_ALIGN_CENTER, 0, -50);
    lv_label_set_text(s_text, "Checking update...");

    s_bar = lv_bar_create(s_scr);
    lv_obj_set_size(s_bar, 200, 14);
    lv_obj_align(s_bar, LV_ALIGN_CENTER, 0, 10);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
}

/**
 * @brief  每拍刷新（uiTask 主循环调用；LVGL 未就绪时直接返回）
 */
void OTA_UI_Tick(void)
{
    uint32_t st;
    uint32_t now;

    if (s_scr == NULL)
        return;

    st = g_ota_prog.state;

    /* ---- 空闲: 收起浮层回 WiFi 页 ---- */
    if (st == OTA_UPD_IDLE)
    {
        if (s_active)
        {
            s_active = false;
            s_term_ms = 0U;
            lv_scr_load(ui_WiFi);
        }
        return;
    }

    now = (uint32_t)xTaskGetTickCount();

    /* ---- 首次进入: 载入浮层 ---- */
    if (!s_active)
    {
        s_active = true;
        s_term_ms = 0U;
        lv_scr_load(s_scr);
    }

    /* ---- 按状态刷新 ---- */
    if (st == OTA_UPD_DOWNLOADING)
    {
        if (g_ota_prog.total > 0U)
        {
            uint32_t pct = g_ota_prog.got * 100U / g_ota_prog.total;
            lv_bar_set_value(s_bar, (int)pct, LV_ANIM_OFF);
            lv_label_set_text_fmt(s_text, "Downloading %u%%", (unsigned)pct);
        }
        else
        {
            /* 直连模式长度未知：只显示已下载量 */
            lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
            lv_label_set_text_fmt(s_text, "Downloading %u KB",
                                  (unsigned)(g_ota_prog.got / 1024U));
        }
    }
    else
    {
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(s_text, State_Text(st));
    }

    /* ---- 结果状态: 3 秒后自动回空闲 ---- */
    if (State_IsTerminal(st))
    {
        if (s_term_ms == 0U)
            s_term_ms = now;
        else if (now - s_term_ms > pdMS_TO_TICKS(3000))
            g_ota_prog.state = OTA_UPD_IDLE;
    }
    else
    {
        s_term_ms = 0U;
    }
}
