/**
 * @file    app_wifi_ui.c
 * @brief   WiFi 弹窗实现：附近 WiFi 列表 + 小键盘输入密码
 *
 * 布局（屏 320×240，弹窗面板 312×232，内边距 4，内容区 304×224）：
 *
 *   列表弹窗                                密码弹窗
 *   ┌──────────────────────────┐           ┌──────────────────────────┐
 *   │ 附近 WiFi        共 8 个 │           │ 输入 WiFi 密码  正在连接…│
 *   │ ┌──────────────────────┐ │           │ MyComputer               │
 *   │ │ MyComputer   已存  强│ │           │ ┌──────────────────────┐ │
 *   │ │ TP-LINK_5G   开放  中│ │           │ │ ******               │ │
 *   │ │ Xiaomi_A1          弱│ │           │ └──────────────────────┘ │
 *   │ └──────────────────────┘ │           │   【连接】   【取消】    │
 *   │  【重新扫描】   【关闭】 │           │ ┌─── 小键盘（lv_keyboard）│
 *   └──────────────────────────┘           └──────────────────────────┘
 *
 * 几个实现上的取舍：
 *   - **扫描/连接全在 app_wifi_task 里做**：net_scan 是全信道主动扫描（1~3 秒），
 *     net_connect 最长 15 秒，放 LVGL 任务里界面直接冻住。这里只发请求 + 轮询结果。
 *   - **用“版本号”判断有没有新结果**，而不是“状态 == DONE”：
 *     状态是边沿型的，界面可能错过（比如弹窗没开的时候刚扫完一次），
 *     版本号只增不减，比一次就永远不会漏。
 *   - 密码框用密文模式（掩码字符用 ASCII 的 '*'，不用默认的 '•'）：
 *     这块屏的中文字库是 GB2312 点阵，'•' 这种符号不一定有字模，
 *     而 '*' 走西文字体一定有，另外每敲一个字会短暂显示原文，
 *     免得用户根本不知道自己敲进去没有。
 *   - 输入框留空点【连接】时：先查 NVS 里记过的密码（省得每次重输），
 *     没有记录且是开放网络就按空密码连。
 *   - 弹窗打开期间会置“界面模态”标记，让录音按键失效（见 app_bus / app_key）。
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_wifi_ui.h"
#include "app_config.h"
#include "app_wifi.h"
#include "app_wifi_store.h"
#include "app_bus.h"

#include "my_drivers/lcd.h"
#include "my_drivers/font_hzk16/hzk16.h"

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "APP_WIFI_UI";

/* ============================ 内部常量 ============================ */

/** 面板比屏幕小一圈，四周留出遮罩，看起来像“弹窗”而不是“换了一屏” */
#define UI_PANEL_W      (BSP_LCD_H_RES - 8)
#define UI_PANEL_H      (BSP_LCD_V_RES - 8)
/** 内容区（面板去掉 4 像素内边距） */
#define UI_CONTENT_W    (UI_PANEL_W - 8)
#define UI_CONTENT_H    (UI_PANEL_H - 8)

/** 扫描一直没结果时的兜底提示时间（毫秒） */
#define UI_SCAN_TIMEOUT_MS  10000

/* ============================ 内部状态（全部 static） ============================ */

static const lv_font_t *s_font = NULL;

/* ---- 列表弹窗 ---- */
static lv_obj_t   *s_list_mask = NULL;      /*!< NULL = 弹窗没开 */
static lv_obj_t   *s_list_box  = NULL;      /*!< 列表容器（每行一个按钮） */
static lv_obj_t   *s_list_hint = NULL;      /*!< 右上角提示（扫描中/共 N 个） */

/* ---- 密码弹窗 ---- */
static lv_obj_t   *s_pwd_mask  = NULL;
static lv_obj_t   *s_pwd_hint  = NULL;
static lv_obj_t   *s_pwd_ok    = NULL;      /*!< 【连接】按钮（提交后禁用，防连点） */
static lv_obj_t   *s_pwd_ta    = NULL;
static char        s_pwd_ssid[APP_WIFI_SSID_MAX_LEN + 1];
static bool        s_pwd_secure = true;

/* ---- 轮询与版本号 ---- */
static lv_timer_t *s_timer         = NULL;
static uint32_t    s_seen_scan_ver = 0;
static uint32_t    s_seen_conn_ver = 0;
static uint32_t    s_scan_deadline = 0;      /*!< 扫描超时兜底（0 = 没在等） */
static bool        s_scan_timeout  = false;  /*!< 已经提示过“扫描超时”（等用户重扫） */
static uint32_t    s_close_at      = 0;      /*!< 连接成功后延迟关闭（0 = 不关） */

/* ---- 缓冲区：都在静态区（LVGL 任务栈只有 7KB，别放大数组到栈上） ---- */
static app_wifi_ap_t s_aps[APP_WIFI_SCAN_LIST_MAX];
static int           s_aps_n;
static char          s_saved_pw[APP_WIFI_PASS_MAX_LEN + 1];
static char          s_text[96];

/* ============================ 通用小控件 ============================ */

/** 全屏半透明遮罩：吃掉下面所有点击 */
static lv_obj_t *ui_mask_create(lv_obj_t *parent)
{
    lv_obj_t *mask = lv_obj_create(parent);
    lv_obj_set_size(mask, LV_PCT(100), LV_PCT(100));
    lv_obj_center(mask);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(mask, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(mask, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mask, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(mask, false);
    lv_obj_set_clickable(mask, true);
    return mask;
}

/** 弹窗面板（遮罩里居中的那块） */
static lv_obj_t *ui_panel_create(lv_obj_t *mask)
{
    lv_obj_t *panel = lv_obj_create(mask);
    lv_obj_set_size(panel, UI_PANEL_W, UI_PANEL_H);
    lv_obj_center(panel);
    lv_obj_set_style_radius(panel, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(panel, lv_color_hex(APP_COLOR_PANEL), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(APP_COLOR_WARN), LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 4, LV_PART_MAIN);
    lv_obj_set_scrollable(panel, false);
    return panel;
}

/** 建一个中文 label（统一字体/颜色） */
static lv_obj_t *ui_label_create(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    if (s_font != NULL) {
        lv_obj_set_style_text_font(lbl, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
    return lbl;
}

/** 建一个按钮（返回按钮对象，文字 label 需要调用者自己建或直接用 lv_button 的文字） */
static lv_obj_t *ui_button_create(lv_obj_t *parent, const char *text,
                                  int w, int h, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(APP_COLOR_USER_BUBBLE), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *lbl = ui_label_create(btn, text, APP_COLOR_TEXT);
    lv_obj_center(lbl);
    return btn;
}

/**
 * @brief  设置 label 文字（内容一样就不动）
 *
 * @note   这个函数会被 150ms 的定时器反复调用，不比较就会一直重新分配文字、
 *         一直让屏幕重绘（LVGL 池本来就不宽裕）。
 */
static void ui_set_text(lv_obj_t *lbl, const char *text)
{
    if (lbl == NULL || text == NULL) {
        return;
    }
    const char *old = lv_label_get_text(lbl);
    if (old != NULL && strcmp(old, text) == 0) {
        return;
    }
    lv_label_set_text(lbl, text);
}

/** 信号档位文字 */
static const char *ui_level_text(int8_t rssi)
{
    if (rssi >= APP_WIFI_RSSI_STRONG) {
        return "强";
    }
    if (rssi >= APP_WIFI_RSSI_MEDIUM) {
        return "中";
    }
    return "弱";
}

/* ============================ 密码弹窗 ============================ */

static void wifi_ui_close_pwd(void)
{
    if (s_pwd_mask != NULL) {
        lv_obj_delete(s_pwd_mask);
        s_pwd_mask = NULL;
    }
    s_pwd_hint  = NULL;
    s_pwd_ok    = NULL;
    s_pwd_ta    = NULL;
    s_pwd_ssid[0] = '\0';
    s_close_at  = 0;
}

/** 弹窗里的提示文字（+颜色） */
static void wifi_ui_pwd_hint(const char *text, uint32_t color)
{
    if (s_pwd_hint == NULL) {
        return;
    }
    ui_set_text(s_pwd_hint, text);
    lv_obj_set_style_text_color(s_pwd_hint, lv_color_hex(color), LV_PART_MAIN);
}

/**
 * @brief  【连接】：读懂输入框里的密码，发起连接请求
 */
static void wifi_ui_do_connect(void)
{
    if (s_pwd_ssid[0] == '\0') {
        return;
    }

    const char *typed = (s_pwd_ta != NULL) ? lv_textarea_get_text(s_pwd_ta) : "";
    const char *pw    = typed;

    if (pw == NULL || pw[0] == '\0') {
        /* 没输密码：优先用 NVS 里记过的（以前连过就不用再输一遍） */
        if (app_wifi_store_find(s_pwd_ssid, s_saved_pw, sizeof(s_saved_pw))) {
            pw = s_saved_pw;
            ESP_LOGI(TAG, "密码框留空，用 NVS 里记着的密码连接「%s」", s_pwd_ssid);
        } else if (!s_pwd_secure) {
            pw = "";                                            /* 开放网络 */
        } else {
            wifi_ui_pwd_hint("请输入密码", APP_COLOR_ERROR);
            return;
        }
    }

    if (app_wifi_connect_request(s_pwd_ssid, pw) != ESP_OK) {
        wifi_ui_pwd_hint("请求失败，请重试", APP_COLOR_ERROR);
        return;
    }

    wifi_ui_pwd_hint("正在连接...", APP_COLOR_WARN);
    if (s_pwd_ok != NULL) {
        lv_obj_add_state(s_pwd_ok, LV_STATE_DISABLED);          /* 防连点 */
    }
}

/** 【连接】/【取消】按钮，以及小键盘的确认/关闭键 */
static void wifi_ui_pwd_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED || code == LV_EVENT_READY) {
        wifi_ui_do_connect();
    } else if (code == LV_EVENT_CANCEL) {
        ESP_LOGI(TAG, "取消输入密码");
        wifi_ui_close_pwd();
    }
}

static void wifi_ui_pwd_cancel_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "取消输入密码");
    wifi_ui_close_pwd();
}

static void wifi_ui_pwd_ok_cb(lv_event_t *e)
{
    (void)e;
    wifi_ui_do_connect();
}

/**
 * @brief  打开密码弹窗（点列表里的某一行）
 *
 * @param  ssid   要连的 AP
 * @param  secure true = 需要密码（开放网络也让用户点一下【连接】，保持一致）
 */
static void wifi_ui_open_pwd(const char *ssid, bool secure)
{
    if (s_list_mask == NULL || s_pwd_mask != NULL) {
        return;
    }
    if (ssid == NULL || ssid[0] == '\0') {
        return;
    }

    strncpy(s_pwd_ssid, ssid, sizeof(s_pwd_ssid) - 1);
    s_pwd_ssid[sizeof(s_pwd_ssid) - 1] = '\0';
    s_pwd_secure = secure;
    s_seen_conn_ver = app_wifi_connect_version();
    s_close_at = 0;

    lv_obj_t *mask  = ui_mask_create(s_list_mask);
    lv_obj_t *panel = ui_panel_create(mask);

    /* 标题 + 状态提示 */
    lv_obj_t *title = ui_label_create(panel, "输入 WiFi 密码", APP_COLOR_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 2, 0);

    s_pwd_hint = ui_label_create(panel, "", APP_COLOR_WARN);
    lv_obj_set_style_max_width(s_pwd_hint, 170, LV_PART_MAIN);
    lv_label_set_long_mode(s_pwd_hint, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_pwd_hint, LV_ALIGN_TOP_RIGHT, -2, 0);

    /* 要连的 SSID */
    lv_obj_t *ssid_lbl = ui_label_create(panel, ssid, APP_COLOR_WARN);
    lv_obj_set_style_max_width(ssid_lbl, UI_CONTENT_W - 4, LV_PART_MAIN);
    lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(ssid_lbl, LV_ALIGN_TOP_LEFT, 2, 20);

    /* 密码输入框：一行、掩码（'*'）、最多 63 字符 */
    s_pwd_ta = lv_textarea_create(panel);
    lv_obj_set_size(s_pwd_ta, UI_CONTENT_W, 30);
    lv_obj_align(s_pwd_ta, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_textarea_set_one_line(s_pwd_ta, true);
    lv_textarea_set_max_length(s_pwd_ta, APP_WIFI_PASS_MAX_LEN - 1);
    lv_textarea_set_password_mode(s_pwd_ta, true);
    /* 掩码用 ASCII 的 '*'：GB2312 点阵字库不一定有 '•' 的字模 */
    lv_textarea_set_password_bullet(s_pwd_ta, "*");
    lv_textarea_set_password_show_time(s_pwd_ta, 600);  /* 每敲一个字短暂显示原文 */
    lv_textarea_set_placeholder_text(s_pwd_ta, "密码（留空=用已存密码）");
    if (s_font != NULL) {
        lv_obj_set_style_text_font(s_pwd_ta, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_color(s_pwd_ta, lv_color_hex(APP_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_pwd_ta, lv_color_hex(APP_COLOR_TEXT), LV_PART_MAIN);
    /* 光标颜色也指定一下：默认主题的光标在这块深色底上几乎看不见 */
    lv_obj_set_style_bg_color(s_pwd_ta, lv_color_hex(APP_COLOR_WARN), LV_PART_CURSOR);
    lv_obj_set_style_border_color(s_pwd_ta, lv_color_hex(APP_COLOR_WARN), LV_PART_CURSOR);
    /* 让光标一开始就闪起来：LVGL 的光标闪烁是靠 FOCUSED 事件启动的，
     * 而这块屏只有触摸输入（没有给输入设备建 group），不会自动给它焦点。 */
    lv_obj_add_state(s_pwd_ta, LV_STATE_FOCUSED);
    lv_obj_send_event(s_pwd_ta, LV_EVENT_FOCUSED, NULL);

    /* 【连接】【取消】 */
    s_pwd_ok = ui_button_create(panel, "连接", 110, 26, wifi_ui_pwd_ok_cb);
    lv_obj_align(s_pwd_ok, LV_ALIGN_TOP_LEFT, 24, 76);
    lv_obj_t *btn_cancel = ui_button_create(panel, "取消", 110, 26, wifi_ui_pwd_cancel_cb);
    lv_obj_align(btn_cancel, LV_ALIGN_TOP_RIGHT, -24, 76);

    /* 小键盘（lv_keyboard：自带字母/数字/符号切换和退格） */
    lv_obj_t *kb = lv_keyboard_create(panel);
    lv_obj_set_size(kb, UI_CONTENT_W, 114);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(APP_COLOR_TOPBAR), LV_PART_MAIN);
    lv_obj_set_style_radius(kb, 6, LV_PART_MAIN);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, s_pwd_ta);
    lv_obj_add_event_cb(kb, wifi_ui_pwd_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, wifi_ui_pwd_event_cb, LV_EVENT_CANCEL, NULL);

    s_pwd_mask = mask;

    /* 提示：以前连过就告诉用户“可以留空” */
    if (app_wifi_store_has(ssid)) {
        wifi_ui_pwd_hint(secure ? "已存密码，可留空" : "开放网络，直接连接", APP_COLOR_OK);
    } else {
        wifi_ui_pwd_hint(secure ? "请输入密码" : "开放网络，直接连接", APP_COLOR_WARN);
    }

    ESP_LOGI(TAG, "打开密码弹窗：「%s」（%s%s）", ssid, secure ? "加密" : "开放",
             app_wifi_store_has(ssid) ? "，NVS 里有记录" : "");
}

/* ============================ 列表弹窗 ============================ */

/** 点某一行：打开密码弹窗 */
static void wifi_ui_row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    if (idx < 0 || idx >= s_aps_n) {
        return;
    }
    wifi_ui_open_pwd(s_aps[idx].ssid, s_aps[idx].secure);
}

/**
 * @brief  按最新的扫描结果重建列表
 */
static void wifi_ui_rebuild_list(void)
{
    if (s_list_box == NULL) {
        return;
    }

    lv_obj_clean(s_list_box);       /* 旧行全部删掉（行里的 label 也跟着走） */
    s_aps_n = app_wifi_scan_snapshot(s_aps, APP_WIFI_SCAN_LIST_MAX);

    if (s_aps_n <= 0) {
        ui_set_text(s_list_hint, "没找到 WiFi");
        lv_obj_set_style_text_color(s_list_hint, lv_color_hex(APP_COLOR_ERROR), LV_PART_MAIN);
        return;
    }

    for (int i = 0; i < s_aps_n; i++) {
        bool saved = app_wifi_store_has(s_aps[i].ssid);

        lv_obj_t *row = lv_button_create(s_list_box);
        lv_obj_set_size(row, LV_PCT(100), APP_WIFI_LIST_ROW_H);
        lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(saved ? APP_COLOR_TOPBAR : APP_COLOR_BG),
                                  LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_column(row, 6, LV_PART_MAIN);
        lv_obj_set_scrollable(row, false);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(row, wifi_ui_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* SSID（占满剩余宽度，太长就省略号） */
        lv_obj_t *name = ui_label_create(row, s_aps[i].ssid, APP_COLOR_TEXT);
        lv_obj_set_flex_grow(name, 1);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_max_width(name, 180, LV_PART_MAIN);

        /* 状态标记：已存 / 开放（都没有就不显示） */
        if (saved) {
            ui_label_create(row, "已存", APP_COLOR_OK);
        } else if (!s_aps[i].secure) {
            ui_label_create(row, "开放", APP_COLOR_WARN);
        }

        /* 信号档位 */
        ui_label_create(row, ui_level_text(s_aps[i].rssi), APP_COLOR_TEXT);
    }

    snprintf(s_text, sizeof(s_text), "共 %d 个", s_aps_n);
    ui_set_text(s_list_hint, s_text);
    lv_obj_set_style_text_color(s_list_hint, lv_color_hex(APP_COLOR_TEXT), LV_PART_MAIN);
    ESP_LOGI(TAG, "列表已刷新：%d 个 AP", s_aps_n);
}

/** 请求一次扫描（弹窗开着的时候才有意义） */
static void wifi_ui_request_scan(void)
{
    app_wifi_scan_request();
    s_seen_scan_ver = app_wifi_scan_version();
    s_scan_deadline = lv_tick_get() + UI_SCAN_TIMEOUT_MS;
    s_scan_timeout  = false;
    ui_set_text(s_list_hint, "扫描中...");
    lv_obj_set_style_text_color(s_list_hint, lv_color_hex(APP_COLOR_WARN), LV_PART_MAIN);
}

static void wifi_ui_rescan_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "用户点了【重新扫描】");
    wifi_ui_request_scan();
}

static void wifi_ui_close_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "用户关闭了 WiFi 弹窗");
    app_wifi_ui_close();
}

/**
 * @brief  弹窗轮询：看扫描/连接有没有新结果
 *
 * @note   在 LVGL 任务里跑（lv_timer），只读状态、只碰 LVGL 对象，不做任何阻塞操作。
 */
static void wifi_ui_tick(lv_timer_t *timer)
{
    (void)timer;

    if (s_list_mask == NULL) {
        return;                     /* 弹窗没开（正常不会发生，关了就把定时器删了） */
    }

    /* ---------- 1. 扫描结果 ---------- */
    uint32_t ver = app_wifi_scan_version();
    if (ver != s_seen_scan_ver) {
        s_seen_scan_ver = ver;
        s_scan_deadline = 0;
        s_scan_timeout  = false;
        wifi_ui_rebuild_list();
    } else if (s_scan_timeout) {
        /* 已经提示过“扫描超时”了：保持这个提示，等用户点【重新扫描】 */
    } else if (app_wifi_scan_state() == APP_WIFI_SCAN_FAILED) {
        ui_set_text(s_list_hint, "扫描失败，请重试");
        lv_obj_set_style_text_color(s_list_hint, lv_color_hex(APP_COLOR_ERROR), LV_PART_MAIN);
    } else if (app_wifi_scan_state() == APP_WIFI_SCAN_RUNNING) {
        if (s_scan_deadline != 0 && (int32_t)(lv_tick_get() - s_scan_deadline) >= 0) {
            /* 扫描迟迟没结果（比如 WiFi 任务没起来）：别让用户一直盯着“扫描中” */
            s_scan_deadline = 0;
            s_scan_timeout  = true;
            ui_set_text(s_list_hint, "扫描超时，请重试");
            lv_obj_set_style_text_color(s_list_hint, lv_color_hex(APP_COLOR_ERROR), LV_PART_MAIN);
        } else {
            ui_set_text(s_list_hint, "扫描中...");
            lv_obj_set_style_text_color(s_list_hint, lv_color_hex(APP_COLOR_WARN), LV_PART_MAIN);
        }
    }

    /* ---------- 2. 密码弹窗里的连接结果 ---------- */
    if (s_pwd_mask != NULL) {
        uint32_t cver = app_wifi_connect_version();
        if (cver != s_seen_conn_ver) {
            s_seen_conn_ver = cver;
            app_wifi_connect_state_t cs  = app_wifi_connect_state();
            const char              *msg = app_wifi_connect_message();

            if (cs == APP_WIFI_CONNECT_OK) {
                wifi_ui_pwd_hint(msg, APP_COLOR_OK);
                s_close_at = lv_tick_get() + APP_WIFI_UI_CLOSE_DELAY_MS;
            } else if (cs == APP_WIFI_CONNECT_FAIL) {
                wifi_ui_pwd_hint(msg, APP_COLOR_ERROR);
                if (s_pwd_ok != NULL) {
                    lv_obj_remove_state(s_pwd_ok, LV_STATE_DISABLED);   /* 让用户改密码重试 */
                }
            } else if (cs == APP_WIFI_CONNECT_RUNNING) {
                wifi_ui_pwd_hint("正在连接...", APP_COLOR_WARN);
            }
        }

        /* 连上了：让用户看 1.5 秒“连接成功”，然后自动关掉弹窗 */
        if (s_close_at != 0 && (int32_t)(lv_tick_get() - s_close_at) >= 0) {
            s_close_at = 0;
            ESP_LOGI(TAG, "连接成功，自动关闭 WiFi 弹窗");
            app_wifi_ui_close();
            return;                 /* 弹窗（含定时器）已经删了，别再往下走 */
        }
    }
}

/* ============================ 对外接口 ============================ */

void app_wifi_ui_open(void)
{
    if (s_list_mask != NULL) {
        ESP_LOGI(TAG, "WiFi 列表已经打开了，忽略这次点击");
        return;
    }

    lv_obj_t *scr = lv_screen_active();
    if (scr == NULL) {
        ESP_LOGE(TAG, "还没有活动的 screen，打不开 WiFi 弹窗");
        return;
    }

    s_font = hzk16_get_lv_font();

    lv_obj_t *mask  = ui_mask_create(scr);
    lv_obj_t *panel = ui_panel_create(mask);

    /* 标题 + 右上角提示 */
    lv_obj_t *title = ui_label_create(panel, "附近 WiFi", APP_COLOR_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 2, 0);

    s_list_hint = ui_label_create(panel, "扫描中...", APP_COLOR_WARN);
    lv_obj_set_style_max_width(s_list_hint, 170, LV_PART_MAIN);
    lv_label_set_long_mode(s_list_hint, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_list_hint, LV_ALIGN_TOP_RIGHT, -2, 0);

    /* 列表容器（可上下滚动） */
    s_list_box = lv_obj_create(panel);
    lv_obj_set_size(s_list_box, LV_PCT(100), 168);
    lv_obj_align(s_list_box, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_obj_set_style_radius(s_list_box, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_list_box, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_list_box, lv_color_hex(APP_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_list_box, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_list_box, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list_box, LV_SCROLLBAR_MODE_AUTO);

    /* 底部两个按钮 */
    lv_obj_t *btn_rescan = ui_button_create(panel, "重新扫描", 110, 28, wifi_ui_rescan_cb);
    lv_obj_align(btn_rescan, LV_ALIGN_BOTTOM_LEFT, 24, 0);
    lv_obj_t *btn_close  = ui_button_create(panel, "关闭", 110, 28, wifi_ui_close_cb);
    lv_obj_align(btn_close, LV_ALIGN_BOTTOM_RIGHT, -24, 0);

    s_list_mask = mask;
    s_aps_n     = 0;
    s_close_at  = 0;

    /* 弹窗期间不让录音按键生效（否则用户正输密码，按住按键就跑去录音了） */
    app_bus_set_ui_modal(true);

    /* 先把上一次的缓存结果摆上（有的话），再请求一次新的扫描 */
    s_seen_scan_ver = app_wifi_scan_version();
    wifi_ui_rebuild_list();
    wifi_ui_request_scan();

    if (s_timer == NULL) {
        s_timer = lv_timer_create(wifi_ui_tick, APP_WIFI_UI_TICK_MS, NULL);
        if (s_timer == NULL) {
            ESP_LOGE(TAG, "创建轮询定时器失败（扫描结果不会自动刷新）");
        }
    } else {
        lv_timer_resume(s_timer);
    }

    ESP_LOGI(TAG, "打开 WiFi 列表弹窗（NVS 里已存 %d 个网络）", app_wifi_store_count());
}

void app_wifi_ui_close(void)
{
    if (s_list_mask != NULL) {
        lv_obj_delete(s_list_mask);     /* 密码弹窗是它的子对象，一起删掉 */
        s_list_mask = NULL;
    }
    s_list_box  = NULL;
    s_list_hint = NULL;
    s_pwd_mask  = NULL;
    s_pwd_hint  = NULL;
    s_pwd_ok    = NULL;
    s_pwd_ta    = NULL;
    s_close_at  = 0;
    s_scan_deadline = 0;
    s_scan_timeout  = false;

    if (s_timer != NULL) {
        lv_timer_delete(s_timer);       /* 在自身回调里删也是安全的（LVGL 内部会处理） */
        s_timer = NULL;
    }

    /* 恢复录音按键 */
    app_bus_set_ui_modal(false);
    ESP_LOGI(TAG, "WiFi 弹窗已关闭");
}

bool app_wifi_ui_is_open(void)
{
    return (s_list_mask != NULL);
}
