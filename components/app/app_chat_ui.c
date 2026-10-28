/**
 * @file    app_chat_ui.c
 * @brief   聊天界面：顶部状态栏 + 气泡列表 + 底部状态行 + AI 回复打字机效果
 *
 * 布局：
 *   ┌───────────────────────────────┐ 0
 *   │ WiFi:已连接         录音中     │ 顶部状态栏（26px）
 *   ├───────────────────────────────┤ 26
 *   │                  ┌──────────┐ │
 *   │                  │ 用户消息  │ │  消息区（192px，可上下滚动）
 *   │                  └──────────┘ │
 *   │ ┌──────────┐                  │
 *   │ │ AI 回复…  │ ← 逐字追加       │
 *   │ └──────────┘                  │
 *   ├───────────────────────────────┤ 218
 *   │ 按住按键说话                   │ 底部状态行（22px）
 *   └───────────────────────────────┘ 240
 *
 * 打字机（需求：逐字显示、不要一次性 set_text 整段）：
 *   - 网络任务每收到一段流式增量就投递 CHAT_MSG_AI_DELTA，这里累积进“目标文本”；
 *   - 一个 30ms 的 LVGL 定时器按 APP_CHAT_TYPE_INTERVAL_MS（默认 100ms）的节奏，
 *     每次从目标文本里**按完整 UTF-8 字符**取 1 个汉字（或 3 个 ASCII）追加到 label；
 *   - 每次追加后自动滚到底部；
 *   - 积压过多时按 APP_CHAT_TYPE_CATCHUP_BYTES 自动加速追赶（可关）；
 *   - 全部显示完后 app_bus_post_typing_done()，状态机据此恢复待机、重新允许录音。
 *
 * 线程模型：
 *   - 界面**只在 LVGL 任务里**被创建和修改；
 *   - 其它任务通过 app_bus_post_chat() 投递消息，由 LVGL 定时器取走处理。
 */

#include <stdio.h>
#include <string.h>

#include "app_chat_ui.h"
#include "app_config.h"
#include "app_gb2312.h"
#include "app_power.h"              /* 设置页：休眠超时的读写 + 活动通知 */
#include "app_settings.h"           /* 设置页：亮度读写/持久化 */
#include "app_time.h"               /* 待机时在状态行后面挂一个 HH:MM */
#include "app_wifi.h"               /* WiFi 连没连：决定“WiFi未连接”提示弹窗要不要自动收起 */
#include "app_wifi_ui.h"            /* WiFi 文字按钮点开“附近 WiFi”弹窗 */

#include "my_drivers/lcd.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "APP_UI";

/* ============================ 内部常量 ============================ */

/** 消息区高度 = 屏幕高 - 顶部栏 - 底部状态行 */
#define UI_MSG_AREA_H       (BSP_LCD_V_RES - APP_CHAT_TOPBAR_H - APP_CHAT_STATUSBAR_H)
/** 错误提示显示时长（毫秒） */
#define UI_ERROR_SHOW_MS    6000

/* ============================ 内部状态（全部 static） ============================ */

static const lv_font_t *s_font        = NULL;
static lv_obj_t        *s_scr         = NULL;   /*!< 本模块新建的 screen */
static lv_obj_t        *s_msg_area    = NULL;   /*!< 消息区（滚动容器） */
static lv_obj_t        *s_status_lbl  = NULL;   /*!< 底部状态行 */
static lv_obj_t        *s_wifi_btn    = NULL;   /*!< 顶部 WiFi 文字按钮（点开附近 WiFi 列表） */
static lv_obj_t        *s_wifi_lbl    = NULL;   /*!< 顶部 WiFi 状态文字（WiFi未连接 / WiFi 强|中|弱） */
static lv_obj_t        *s_rec_flag    = NULL;   /*!< 顶部录音/处理指示 */
static lv_timer_t      *s_timer       = NULL;

/** 「WiFi未连接」提示弹窗（NULL = 没打开）；控件在关闭时一起删掉 */
static lv_obj_t        *s_wifi_alert_modal = NULL;

/** 设置二级页（NULL = 还没建；懒创建，建好一直复用） */
static lv_obj_t        *s_set_scr = NULL;

/** 当前显示的气泡行（用于超量时删除最老的） */
static lv_obj_t        *s_rows[APP_CHAT_MAX_BUBBLES];
static int              s_row_count   = 0;

/** 错误提示到期时间（lv_tick 基准，0 = 没有错误提示） */
static uint32_t         s_error_until = 0;
/** 上一次显示的状态文字，用于判断是否需要刷新 */
static char             s_status_text[128] = "";
/** 顶部指示器上一次反映的状态（避免每 tick 重复设置文本） */
static app_state_t      s_ind_state   = (app_state_t)-1;

/**
 * 气泡文本裁剪缓冲
 * @note  放静态区（不放栈上）：LVGL 任务栈只有 7KB，1KB 的局部数组容易被挤爆；
 *        本模块只在 LVGL 线程里被调用，用静态缓冲是安全的。
 */
static char             s_bubble_text[APP_CHAT_BUBBLE_TEXT_MAX];

/**
 * 从 chat_msg_queue 取消息用的缓冲
 * @note  同样必须放静态区：chat_msg_t 有 1KB+，而本回调是在 LVGL 任务里跑的，
 *        上面还压着 lv_timer_handler 的一串调用，放局部变量容易爆栈。
 */
static chat_msg_t       s_rx_msg;

/* ---------------- 打字机状态 ---------------- */

static char      s_type_target[APP_CHAT_BUBBLE_TEXT_MAX];  /*!< 目标全文（UTF-8） */
static size_t    s_type_len      = 0;   /*!< 目标文本字节数 */
static size_t    s_type_shown    = 0;   /*!< 已显示字节数（永远落在字符边界上） */
static lv_obj_t *s_type_label    = NULL;/*!< 正在打字的气泡里的 label */
static lv_obj_t *s_type_row      = NULL;/*!< 对应行（滚动用） */
static bool      s_type_active   = false;   /*!< 正在打字 */
static bool      s_type_more     = false;   /*!< 流式还没结束（还会有增量） */
static bool      s_type_wrapped  = false;   /*!< 已切到定宽换行排版 */
static bool      s_type_truncated = false;  /*!< 超长截断过（只告警一次） */
static uint32_t  s_type_last_tick = 0;      /*!< 上次追加时间（lv_tick） */

/* ============================ 内部函数：样式 ============================ */

/** 给对象套用统一的“无边框无内边距”设置 */
static void ui_obj_base(lv_obj_t *obj, lv_color_t bg)
{
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, bg, LV_PART_MAIN);
    lv_obj_set_scrollable(obj, false);
}

/* ============================ 内部函数：气泡 ============================ */

/**
 * @brief  删除最老的一个气泡行
 */
static void ui_drop_oldest_row(void)
{
    if (s_row_count <= 0) {
        return;
    }
    if (s_rows[0] != NULL) {
        /* 万一删到正在打字的那一行，把打字机句柄一起清掉（正常不会发生） */
        if (s_rows[0] == s_type_row) {
            s_type_row   = NULL;
            s_type_label = NULL;
        }
        lv_obj_delete(s_rows[0]);
    }
    for (int i = 0; i < s_row_count - 1; i++) {
        s_rows[i] = s_rows[i + 1];
    }
    s_row_count--;
    s_rows[s_row_count] = NULL;
}

/**
 * @brief  创建一个气泡（行容器 + 气泡本体 + 文字）
 *
 * @param  text      初始文本（UTF-8，可为空串）
 * @param  is_user   true = 用户（右对齐 + 浅蓝），false = AI（左对齐 + 浅灰）
 * @param  label_out 输出：文字 label 句柄（可为 NULL）
 * @param  row_out   输出：整行句柄（可为 NULL，滚动用）
 * @return 行对象；失败返回 NULL
 */
static lv_obj_t *ui_create_bubble(const char *text, bool is_user,
                                 lv_obj_t **label_out, lv_obj_t **row_out)
{
    if (s_msg_area == NULL) {
        return NULL;
    }
    if (label_out != NULL) {
        *label_out = NULL;
    }
    if (row_out != NULL) {
        *row_out = NULL;
    }

    /* 行容器：占满宽度，用 flex 把气泡推到左边或右边 */
    lv_obj_t *row = lv_obj_create(s_msg_area);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          is_user ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    ui_obj_base(row, lv_color_hex(APP_COLOR_BG));

    /* 气泡本体 */
    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(bubble, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(bubble, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bubble,
                              lv_color_hex(is_user ? APP_COLOR_USER_BUBBLE : APP_COLOR_AI_BUBBLE),
                              LV_PART_MAIN);
    lv_obj_set_scrollable(bubble, false);

    /* 文字 */
    lv_obj_t *label = lv_label_create(bubble);
    lv_label_set_text(label, (text != NULL) ? text : "");
    if (s_font != NULL) {
        lv_obj_set_style_text_font(label, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    /* 排不下就换行：按初始文本估算宽度 */
    uint32_t text_w = app_text_display_width((text != NULL) ? text : "");
    if (text_w > APP_CHAT_BUBBLE_MAX_W) {
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(label, APP_CHAT_BUBBLE_MAX_W);
    } else {
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_width(label, LV_SIZE_CONTENT);
    }

    /* 记录并限制气泡数量 */
    if (s_row_count >= APP_CHAT_MAX_BUBBLES) {
        ui_drop_oldest_row();
    }
    s_rows[s_row_count++] = row;

    if (label_out != NULL) {
        *label_out = label;
    }
    if (row_out != NULL) {
        *row_out = row;
    }
    return row;
}

/**
 * @brief  追加一个“立即完整显示”的气泡（用户消息走这里，不做打字机）
 */
static void ui_add_bubble(const char *text, bool is_user)
{
    if (text == NULL) {
        return;
    }

    /* 按字节数截断（不会切断多字节字符），避免超长文本吃满 LVGL 内存 */
    app_text_truncate_utf8(text, s_bubble_text, sizeof(s_bubble_text), 0xFFFFFFFFU);
    if (s_bubble_text[0] == '\0') {
        return;
    }

    lv_obj_t *row = ui_create_bubble(s_bubble_text, is_user, NULL, NULL);
    if (row != NULL) {
        lv_obj_scroll_to_view(row, LV_ANIM_ON);     /* 新消息自动滚到底部 */
    }
}

/* ============================ 内部函数：打字机 ============================ */

/**
 * @brief  开始一条新的 AI 消息（准备逐字显示）
 */
static void ui_type_begin(void)
{
    if (s_type_active) {
        return;
    }
    s_type_len        = 0;
    s_type_shown      = 0;
    s_type_target[0]  = '\0';
    s_type_label      = NULL;
    s_type_row        = NULL;
    s_type_active     = true;
    s_type_more       = true;       /* 默认认为还会来增量，收到 AI_END 才置 false */
    s_type_wrapped    = false;
    s_type_truncated  = false;
    s_type_last_tick  = lv_tick_get();
    app_bus_set_ui_typing(true);    /* 告诉状态机：界面开始打字了（先别让用户录音） */
}

/**
 * @brief  往目标文本追加一段增量（UTF-8 安全、限长）
 */
static void ui_type_push(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    if (!s_type_active) {
        ui_type_begin();
    }

    size_t len  = strlen(text);
    size_t room = sizeof(s_type_target) - 1 - s_type_len;

    if (len > room) {
        /* 按**完整字符**截断，绝不留半个多字节字符 */
        size_t cut = 0;
        while (cut < len) {
            int cb = app_utf8_char_bytes((unsigned char)text[cut]);
            if (cut + (size_t)cb > room) {
                break;
            }
            cut += (size_t)cb;
        }
        len = cut;
        if (!s_type_truncated) {
            s_type_truncated = true;
            ESP_LOGW(TAG, "气泡文字达到上限 %u 字节，后续不再显示（完整回复仍在对话历史里）",
                     (unsigned)(sizeof(s_type_target) - 1));
        }
    }
    if (len == 0) {
        return;
    }

    memcpy(s_type_target + s_type_len, text, len);
    s_type_len += len;
    s_type_target[s_type_len] = '\0';
}

/**
 * @brief  把“当前已显示的部分”刷到 label 上，并滚到底部
 *
 * @note   做法：临时在 s_type_shown 处截断字符串（lv_label_set_text 会复制一份），
 *         刷完再恢复——于是每次只显示前缀，天然就是“逐字追加”。
 */
static void ui_type_render(void)
{
    if (s_type_label == NULL) {
        return;
    }

    char save = s_type_target[s_type_shown];
    s_type_target[s_type_shown] = '\0';
    lv_label_set_text(s_type_label, s_type_target);
    s_type_target[s_type_shown] = save;

    /* 文字长到需要换行时切换一次排版（只切一次，避免每帧抖动） */
    if (!s_type_wrapped && app_text_display_width(s_type_target) > APP_CHAT_BUBBLE_MAX_W) {
        lv_label_set_long_mode(s_type_label, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(s_type_label, APP_CHAT_BUBBLE_MAX_W);
        s_type_wrapped = true;
    }

    /* 需求：每次更新后自动滚到底部 */
    if (s_type_row != NULL) {
        lv_obj_scroll_to_view(s_type_row, LV_ANIM_OFF);
    }
}

/**
 * @brief  第一次要显示文字时才把气泡建出来（避免先闪一个空气泡）
 */
static void ui_type_ensure_bubble(void)
{
    if (s_type_label != NULL || s_msg_area == NULL) {
        return;
    }
    ui_create_bubble("", false, &s_type_label, &s_type_row);
}

/**
 * @brief  结束本条 AI 消息：剩下的字直接补全，并通知状态机“显示完毕”
 */
static void ui_type_finish(void)
{
    if (!s_type_active) {
        return;
    }

    if (s_type_shown < s_type_len) {
        s_type_shown = s_type_len;
        ui_type_ensure_bubble();
        ui_type_render();
    }

    s_type_active = false;
    s_type_more   = false;
    s_type_label  = NULL;
    s_type_row    = NULL;

    /* 先清标记再回报，避免状态机看到“事件已到但还在打字”的中间态 */
    app_bus_set_ui_typing(false);
    app_bus_post_typing_done();     /* 恢复待机、允许下次录音 */
}

/**
 * @brief  打字机节拍：按配置的间隔与速度往 label 上追字
 *
 * @param  now_tick 当前 lv_tick
 */
static void ui_type_tick(uint32_t now_tick)
{
    if (!s_type_active) {
        return;
    }

    if (s_type_len == 0) {
        if (!s_type_more) {
            ui_type_finish();       /* 空回复也要回报完成，别让状态机一直卡着 */
        }
        return;
    }
    if (s_type_shown >= s_type_len) {
        if (!s_type_more) {
            ui_type_finish();       /* 都显示完了，收尾 */
        }
        return;
    }

    if ((uint32_t)(now_tick - s_type_last_tick) < APP_CHAT_TYPE_INTERVAL_MS) {
        return;                     /* 还没到下一次追加的时间 */
    }
    s_type_last_tick = now_tick;

    /* 每次追加几个“字”：1 个汉字 = 3 分，1 个 ASCII = 1 分 */
    uint32_t units = APP_CHAT_TYPE_UNITS;
#if (APP_CHAT_TYPE_CATCHUP_BYTES > 0)
    size_t backlog = s_type_len - s_type_shown;
    if (backlog > APP_CHAT_TYPE_CATCHUP_BYTES) {
        units += (uint32_t)(backlog / APP_CHAT_TYPE_CATCHUP_BYTES);
        if (units > APP_CHAT_TYPE_MAX_UNITS) {
            units = APP_CHAT_TYPE_MAX_UNITS;
        }
    }
#endif

    ui_type_ensure_bubble();

    uint32_t budget = units * 3U;
    while (s_type_shown < s_type_len && budget > 0) {
        int      cb   = app_utf8_char_bytes((unsigned char)s_type_target[s_type_shown]);
        uint32_t cost = (cb == 1) ? 1U : 3U;

        if (s_type_shown + (size_t)cb > s_type_len) {
            break;      /* 尾巴不完整（多字节字符被拆开），等下一段增量 */
        }
        if (cost > budget) {
            break;
        }
        budget       -= cost;
        s_type_shown += (size_t)cb;
    }

    ui_type_render();

    if (s_type_shown >= s_type_len && !s_type_more) {
        ui_type_finish();
    }
}

/* ============================ 内部函数：状态 ============================ */

/**
 * @brief  设置底部状态行文字
 * @param  is_error true 时用红色显示，并在若干秒后自动恢复状态提示
 */
static void ui_set_status(const char *text, bool is_error)
{
    if (s_status_lbl == NULL || text == NULL) {
        return;
    }

    lv_label_set_text(s_status_lbl, text);
    lv_obj_set_style_text_color(s_status_lbl,
                               lv_color_hex(is_error ? APP_COLOR_ERROR : APP_COLOR_WARN),
                               LV_PART_MAIN);

    strncpy(s_status_text, text, sizeof(s_status_text) - 1);
    s_status_text[sizeof(s_status_text) - 1] = '\0';

    s_error_until = is_error ? (lv_tick_get() + UI_ERROR_SHOW_MS) : 0;
}

/* ============================ 内部函数：「WiFi未连接」提示弹窗 ============================ */

/**
 * @brief  当前是否还有模态弹窗开着
 *
 * @note   关闭某个弹窗时用它决定要不要清“界面模态”标记：
 *         只有**所有**弹窗都关了，录音键才恢复正常响应。
 */
static bool ui_modal_present(void)
{
    return (s_wifi_alert_modal != NULL) || app_wifi_ui_is_open();
}

/**
 * @brief  关闭「WiFi未连接」提示弹窗（子控件随遮罩一起删掉）
 */
static void ui_wifi_alert_close(void)
{
    if (s_wifi_alert_modal == NULL) {
        return;
    }
    lv_obj_delete(s_wifi_alert_modal);
    s_wifi_alert_modal = NULL;

    if (!ui_modal_present()) {          /* 别的弹窗都关着才恢复录音键 */
        app_bus_set_ui_modal(false);
    }
    ESP_LOGI(TAG, "「WiFi未连接」提示弹窗已关闭");
}

/** 【知道了】：只关提示，不连 WiFi */
static void ui_wifi_alert_dismiss_cb(lv_event_t *e)
{
    (void)e;
    ui_wifi_alert_close();
}

/** 【去连接】：收起提示，直接打开“附近 WiFi”列表，让用户马上选网 */
static void ui_wifi_alert_go_connect_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "提示弹窗里点【去连接】，打开附近 WiFi 列表");
    ui_wifi_alert_close();
    app_wifi_ui_open();
}

/**
 * @brief  打开「WiFi未连接」提示弹窗
 *
 * @note   触发场景见 app_key.c：WiFi 未连接时按下录音键，状态机**不录音**，
 *         只投递 CHAT_MSG_WIFI_ALERT，UI 定时器收到后调用本函数。
 *         弹窗期间会置“界面模态”标记，录音键继续无效；
 *         WiFi 一连上，ui_timer_cb 里的检查会自动把它收起来并恢复录音。
 */
static void ui_wifi_alert_open(void)
{
    if (s_wifi_alert_modal != NULL || s_scr == NULL) {
        return;                         /* 已经打开 / 界面还没建好 */
    }

    /* ---------- 1. 遮罩（吃掉下层点击，避免误触） ---------- */
    lv_obj_t *mask = lv_obj_create(s_scr);
    lv_obj_set_size(mask, LV_PCT(100), LV_PCT(100));
    lv_obj_center(mask);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(mask, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(mask, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mask, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(mask, false);
    lv_obj_set_clickable(mask, true);

    /* ---------- 2. 面板 ---------- */
    lv_obj_t *panel = lv_obj_create(mask);
    lv_obj_set_size(panel, 270, 150);
    lv_obj_center(panel);
    lv_obj_set_style_radius(panel, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(panel, lv_color_hex(APP_COLOR_PANEL), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(APP_COLOR_ERROR), LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 10, LV_PART_MAIN);
    lv_obj_set_scrollable(panel, false);

    /* ---------- 3. 标题 + 两行说明 ---------- */
    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "WiFi未连接");
    if (s_font != NULL) {
        lv_obj_set_style_text_font(title, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(title, lv_color_hex(APP_COLOR_ERROR), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *desc = lv_label_create(panel);
    lv_label_set_text(desc, "请先连接 WiFi 再按住说话");
    if (s_font != NULL) {
        lv_obj_set_style_text_font(desc, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(desc, lv_color_hex(APP_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_align(desc, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *desc2 = lv_label_create(panel);
    lv_label_set_text(desc2, "录音与上传已暂停，连上后自动恢复");
    if (s_font != NULL) {
        lv_obj_set_style_text_font(desc2, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(desc2, lv_color_hex(APP_COLOR_WARN), LV_PART_MAIN);
    lv_obj_align(desc2, LV_ALIGN_TOP_MID, 0, 58);

    /* ---------- 4. 按钮：去连接 / 知道了 ---------- */
    lv_obj_t *btn_go = lv_button_create(panel);
    lv_obj_set_size(btn_go, 110, 30);
    lv_obj_align(btn_go, LV_ALIGN_BOTTOM_LEFT, 14, 0);
    lv_obj_set_style_radius(btn_go, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_go, lv_color_hex(APP_COLOR_USER_BUBBLE), LV_PART_MAIN);
    lv_obj_add_event_cb(btn_go, ui_wifi_alert_go_connect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_go_lbl = lv_label_create(btn_go);
    lv_label_set_text(btn_go_lbl, "去连接");
    if (s_font != NULL) {
        lv_obj_set_style_text_font(btn_go_lbl, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(btn_go_lbl, lv_color_hex(APP_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_center(btn_go_lbl);

    lv_obj_t *btn_ok = lv_button_create(panel);
    lv_obj_set_size(btn_ok, 100, 30);
    lv_obj_align(btn_ok, LV_ALIGN_BOTTOM_RIGHT, -14, 0);
    lv_obj_set_style_radius(btn_ok, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_ok, lv_color_hex(APP_COLOR_USER_BUBBLE), LV_PART_MAIN);
    lv_obj_add_event_cb(btn_ok, ui_wifi_alert_dismiss_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_ok_lbl = lv_label_create(btn_ok);
    lv_label_set_text(btn_ok_lbl, "知道了");
    if (s_font != NULL) {
        lv_obj_set_style_text_font(btn_ok_lbl, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(btn_ok_lbl, lv_color_hex(APP_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_center(btn_ok_lbl);

    s_wifi_alert_modal = mask;

    /* 弹窗期间录音键无效；WiFi 连上或用户点按钮关掉后恢复 */
    app_bus_set_ui_modal(true);

    ESP_LOGI(TAG, "WiFi 未连接，弹出提示弹窗（录音与上传已暂停，WiFi 连上后自动收起）");
}

/* ============================ 内部函数：定时器 ============================ */

/**
 * @brief  界面刷新定时器（在 LVGL 任务上下文里执行）
 */
static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    /* 1. 处理消息队列（s_rx_msg 是静态缓冲，不占 LVGL 任务的栈） */
    QueueHandle_t q = app_bus_chat_queue();
    if (q != NULL) {
        chat_msg_t *msg = &s_rx_msg;
        while (xQueueReceive(q, msg, 0) == pdTRUE) {
            switch (msg->type) {
            case CHAT_MSG_USER:
                /* 用户消息立即完整显示；上一条 AI 若还在打字，先把它打完 */
                ui_type_finish();
                ui_add_bubble(msg->text, true);
                break;

            case CHAT_MSG_AI_DELTA:
                ui_type_push(msg->text);
                break;

            case CHAT_MSG_AI_END:
                s_type_more = false;
                if (!s_type_active) {
                    app_bus_post_typing_done();     /* 一个字都没有：直接回报完成 */
                }
                break;

            case CHAT_MSG_STATUS:
                ui_set_status(msg->text, false);
                break;

            case CHAT_MSG_ERROR:
                /* 出错：把已经收到的字打完（别丢），再显示错误 */
                ui_type_finish();
                ui_set_status(msg->text, true);
                ESP_LOGW(TAG, "界面错误提示：%s", msg->text);
                break;

            case CHAT_MSG_WIFI:
                if (s_wifi_lbl != NULL) {
                    lv_label_set_text(s_wifi_lbl, msg->text);
                }
                break;

            case CHAT_MSG_WIFI_ALERT:
                /* WiFi 未连接时按下录音键：弹出提示（状态机没开始录音，
                 * 也不会尝试上传）。重复消息不会叠出第二个弹窗。 */
                ui_wifi_alert_open();
                break;

            default:
                break;
            }
        }
    }

    /* WiFi 连上了：自动收起「WiFi未连接」提示弹窗，恢复录音与上传 */
    if (s_wifi_alert_modal != NULL && app_wifi_is_connected()) {
        ESP_LOGI(TAG, "WiFi 已连接，自动关闭「WiFi未连接」提示弹窗");
        ui_wifi_alert_close();
    }

    /* 2. 打字机节拍 */
    ui_type_tick(lv_tick_get());

    /* 3. 状态行：错误提示优先显示，过期后自动回到状态机的文字。
     *    待机时顺带把当前时间（HH:MM）挂在后面：
     *    时间没对上（刚开机、没网）就不显示，避免出现 1970 那种假时间。 */
    app_state_t state        = app_bus_get_state();
    bool        error_active = (s_error_until != 0) &&
                               ((int32_t)(lv_tick_get() - s_error_until) < 0);
    if (!error_active) {
        char        want[96];
        const char *base = app_bus_state_text(state);
        char        hhmm[16];

        if (state == APP_STATE_IDLE && app_time_is_synced() &&
            app_time_now_clock(hhmm, sizeof(hhmm)) > 0) {
            snprintf(want, sizeof(want), "%s（%s）", base, hhmm);
        } else {
            snprintf(want, sizeof(want), "%s", base);
        }
        if (strcmp(s_status_text, want) != 0) {
            ui_set_status(want, false);
        }
    }

    /* 4. 顶部指示（纯中文，保证 hzk16 字库一定能渲染；只在变化时刷新） */
    if (s_rec_flag != NULL && state != s_ind_state) {
        s_ind_state = state;
        if (state == APP_STATE_RECORDING) {
            lv_label_set_text(s_rec_flag, "录音中");
            lv_obj_set_style_text_color(s_rec_flag, lv_color_hex(APP_COLOR_ERROR), LV_PART_MAIN);
        } else if (state == APP_STATE_UPLOADING) {
            lv_label_set_text(s_rec_flag, "识别中");
            lv_obj_set_style_text_color(s_rec_flag, lv_color_hex(APP_COLOR_WARN), LV_PART_MAIN);
        } else if (state == APP_STATE_LLM_REQUEST) {
            lv_label_set_text(s_rec_flag, "思考中");
            lv_obj_set_style_text_color(s_rec_flag, lv_color_hex(APP_COLOR_WARN), LV_PART_MAIN);
        } else if (state == APP_STATE_AI_TYPING) {
            lv_label_set_text(s_rec_flag, "输出中");
            lv_obj_set_style_text_color(s_rec_flag, lv_color_hex(APP_COLOR_WARN), LV_PART_MAIN);
        } else {
            lv_label_set_text(s_rec_flag, "待机");
            lv_obj_set_style_text_color(s_rec_flag, lv_color_hex(APP_COLOR_OK), LV_PART_MAIN);
        }
    }
}

/* ============================ 内部函数：设置二级页 ============================ */

static void ui_settings_open(void);        /* 前向声明：按钮回调在上面 */

/**
 * @brief  “进入休眠的时间”三个可选项（对应 app_config.h 里的超时常量）
 */
static const struct {
    const char *text;
    uint32_t    timeout_ms;
} s_sleep_options[3] = {
    { "30 秒",  APP_POWER_TIMEOUT_30S },
    { "1 分钟", APP_POWER_TIMEOUT_60S },
    { "永不",   APP_POWER_TIMEOUT_NEVER },
};

/**
 * @brief  “IMU 唤醒阈值”三个可选项（对应 app_config.h 里的灵敏度档位）
 */
static const struct {
    const char *text;
    uint8_t     level;
} s_imu_options[3] = {
    { "弱", APP_POWER_IMU_SENS_WEAK },
    { "中", APP_POWER_IMU_SENS_MED },
    { "强", APP_POWER_IMU_SENS_STRONG },
};

/** 设置页里的三个休眠选项按钮（建页时填，用于高亮当前选中项） */
static lv_obj_t *s_set_sleep_btn[3] = { NULL, NULL, NULL };
/** 设置页里的三个 IMU 灵敏度按钮（同上） */
static lv_obj_t *s_set_imu_btn[3]   = { NULL, NULL, NULL };
/** 设置页里的亮度滑条 / 百分比文字 */
static lv_obj_t *s_set_bk_slider    = NULL;
static lv_obj_t *s_set_bk_value_lbl = NULL;

/**
 * @brief  刷新“亮度 xx%”文字
 */
static void ui_set_update_brightness_label(int value)
{
    if (s_set_bk_value_lbl == NULL) {
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%d%%", value);
    lv_label_set_text(s_set_bk_value_lbl, buf);
}

/**
 * @brief  按当前生效的超时高亮对应的选项按钮
 */
static void ui_set_refresh_sleep_buttons(void)
{
    uint32_t current = app_power_get_timeout_ms();

    for (int i = 0; i < 3; i++) {
        if (s_set_sleep_btn[i] == NULL) {
            continue;
        }
        bool selected = (current == s_sleep_options[i].timeout_ms);
        lv_obj_set_style_bg_color(s_set_sleep_btn[i],
                                  lv_color_hex(selected ? APP_COLOR_USER_BUBBLE : APP_COLOR_PANEL),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_set_sleep_btn[i], LV_OPA_COVER, LV_PART_MAIN);
    }
}

/**
 * @brief  按当前生效的 IMU 灵敏度档位高亮对应的选项按钮
 */
static void ui_set_refresh_imu_buttons(void)
{
    uint8_t current = app_power_get_imu_sensitivity();

    for (int i = 0; i < 3; i++) {
        if (s_set_imu_btn[i] == NULL) {
            continue;
        }
        bool selected = (current == s_imu_options[i].level);
        lv_obj_set_style_bg_color(s_set_imu_btn[i],
                                  lv_color_hex(selected ? APP_COLOR_USER_BUBBLE : APP_COLOR_PANEL),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_set_imu_btn[i], LV_OPA_COVER, LV_PART_MAIN);
    }
}

/**
 * @brief  亮度滑条：拖动时实时生效（不写卡），松手才写卡
 *
 * @note   和原来的弹窗一样：拖动过程中反复写 TF 卡既慢又费卡，没必要。
 */
static void ui_set_brightness_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int       value  = (int)lv_slider_get_value(slider);

    app_settings_set_backlight(value, false);       /* persist = false：只生效 */
    ui_set_update_brightness_label(value);

    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        esp_err_t err = app_settings_set_backlight(value, true);
        if (err != ESP_OK) {
            app_bus_post_chatf(CHAT_MSG_ERROR, "亮度已生效，但保存失败(%s)",
                               esp_err_to_name(err));
        }
    }
}

/**
 * @brief  休眠时间选项被点击：保存并刷新高亮
 */
static void ui_set_sleep_cb(lv_event_t *e)
{
    uint32_t timeout_ms = (uint32_t)(uintptr_t)lv_event_get_user_data(e);

    esp_err_t err = app_power_set_timeout_ms(timeout_ms);
    ui_set_refresh_sleep_buttons();

    if (err != ESP_OK) {
        app_bus_post_chatf(CHAT_MSG_ERROR, "休眠时间已生效，但保存失败(%s)",
                           esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "休眠时间设置为 %u ms（0=永不）", (unsigned)timeout_ms);
}

/**
 * @brief  IMU 唤醒阈值档位被点击：保存并刷新高亮
 */
static void ui_set_imu_cb(lv_event_t *e)
{
    uint8_t level = (uint8_t)(uintptr_t)lv_event_get_user_data(e);

    esp_err_t err = app_power_set_imu_sensitivity(level);
    ui_set_refresh_imu_buttons();

    if (err != ESP_OK) {
        app_bus_post_chatf(CHAT_MSG_ERROR, "IMU 阈值已生效，但保存失败(%s)",
                           esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "IMU 唤醒阈值设置为 %s（%u）",
             (level == APP_POWER_IMU_SENS_WEAK)   ? "弱"
             : (level == APP_POWER_IMU_SENS_STRONG) ? "强"
                                                   : "中",
             (unsigned)level);
}

/**
 * @brief  左上角【返回】：回到聊天主界面
 */
static void ui_set_back_cb(lv_event_t *e)
{
    (void)e;

    app_bus_set_ui_page(false);
    if (s_scr != NULL) {
        lv_screen_load(s_scr);
    }
    app_power_notify_activity();
    ESP_LOGI(TAG, "退出设置页");
}

/**
 * @brief  创建设置二级页（只建一次，之后反复复用）
 *
 * 布局：顶部一条（左【返回】、中“设置”），下面是
 *       “进入休眠的时间” 30 秒 / 1 分钟 / 永不 三个并排按钮，
 *       再下面是“屏幕亮度” 滑条 + 百分比。
 */
static void ui_set_build(void)
{
    s_set_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_set_scr, lv_color_hex(APP_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_set_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_set_scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_set_scr, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_set_scr, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(s_set_scr, false);

    /* ---------- 顶部条 + 返回按钮 ---------- */
    lv_obj_t *bar = lv_obj_create(s_set_scr);
    lv_obj_set_size(bar, LV_PCT(100), APP_CHAT_TOPBAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    ui_obj_base(bar, lv_color_hex(APP_COLOR_TOPBAR));
    lv_obj_set_scrollable(bar, false);

    lv_obj_t *back = lv_button_create(bar);
    lv_obj_set_size(back, 52, APP_CHAT_TOPBAR_H - 4);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(APP_COLOR_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(back, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(back, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, ui_set_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, "返回");
    if (s_font != NULL) {
        lv_obj_set_style_text_font(back_lbl, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(APP_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_center(back_lbl);

    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "设置");
    if (s_font != NULL) {
        lv_obj_set_style_text_font(title, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(title, lv_color_hex(APP_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* ---------- 进入休眠的时间 ---------- */
    /* 纵向排布（屏高 240、顶栏 26）：三组控件各占一个“标题 + 一行”，
     * 行高压到 30~38，保证 3 组都在屏内且不重叠：
     *   34..50   “进入休眠的时间”标题
     *   52..90   30 秒 / 1 分钟 / 永不 三个按钮
     *   96..112  “IMU 唤醒阈值”标题
     *   114..152 弱 / 中 / 强 三个按钮
     *   160..176 “屏幕亮度”标题
     *   182..200 亮度滑条
     *   206..222 底部小字说明 */
    int y = APP_CHAT_TOPBAR_H + 8;

    lv_obj_t *lbl_sleep = lv_label_create(s_set_scr);
    lv_label_set_text(lbl_sleep, "进入休眠的时间");
    if (s_font != NULL) {
        lv_obj_set_style_text_font(lbl_sleep, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(lbl_sleep, lv_color_hex(APP_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_align(lbl_sleep, LV_ALIGN_TOP_LEFT, 12, y);

    lv_obj_t *row = lv_obj_create(s_set_scr);
    lv_obj_set_size(row, LV_PCT(100), 38);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y + 18);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(row, false);

    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_button_create(row);
        lv_obj_set_size(btn, 88, 30);
        lv_obj_set_style_radius(btn, 6, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, lv_color_hex(APP_COLOR_WARN), LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, ui_set_sleep_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)s_sleep_options[i].timeout_ms);

        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, s_sleep_options[i].text);
        if (s_font != NULL) {
            lv_obj_set_style_text_font(bl, s_font, LV_PART_MAIN);
        }
        lv_obj_set_style_text_color(bl, lv_color_hex(APP_COLOR_TEXT), LV_PART_MAIN);
        lv_obj_center(bl);

        s_set_sleep_btn[i] = btn;
    }

    /* ---------- IMU 唤醒阈值（弱 / 中 / 强，只影响 IMU，触摸不受影响） ---------- */
    y += 62;

    lv_obj_t *lbl_imu = lv_label_create(s_set_scr);
    lv_label_set_text(lbl_imu, "IMU 唤醒阈值");
    if (s_font != NULL) {
        lv_obj_set_style_text_font(lbl_imu, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(lbl_imu, lv_color_hex(APP_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_align(lbl_imu, LV_ALIGN_TOP_LEFT, 12, y);

    lv_obj_t *imu_row = lv_obj_create(s_set_scr);
    lv_obj_set_size(imu_row, LV_PCT(100), 38);
    lv_obj_align(imu_row, LV_ALIGN_TOP_MID, 0, y + 18);
    lv_obj_set_style_bg_opa(imu_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(imu_row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(imu_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(imu_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(imu_row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_column(imu_row, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(imu_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(imu_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(imu_row, false);

    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_button_create(imu_row);
        lv_obj_set_size(btn, 88, 30);
        lv_obj_set_style_radius(btn, 6, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, lv_color_hex(APP_COLOR_WARN), LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, ui_set_imu_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)s_imu_options[i].level);

        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, s_imu_options[i].text);
        if (s_font != NULL) {
            lv_obj_set_style_text_font(bl, s_font, LV_PART_MAIN);
        }
        lv_obj_set_style_text_color(bl, lv_color_hex(APP_COLOR_TEXT), LV_PART_MAIN);
        lv_obj_center(bl);

        s_set_imu_btn[i] = btn;
    }

    /* ---------- 屏幕亮度 ---------- */
    y += 64;

    lv_obj_t *lbl_bk = lv_label_create(s_set_scr);
    lv_label_set_text(lbl_bk, "屏幕亮度");
    if (s_font != NULL) {
        lv_obj_set_style_text_font(lbl_bk, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(lbl_bk, lv_color_hex(APP_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_align(lbl_bk, LV_ALIGN_TOP_LEFT, 12, y);

    s_set_bk_value_lbl = lv_label_create(s_set_scr);
    if (s_font != NULL) {
        lv_obj_set_style_text_font(s_set_bk_value_lbl, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(s_set_bk_value_lbl, lv_color_hex(APP_COLOR_WARN), LV_PART_MAIN);
    lv_obj_align(s_set_bk_value_lbl, LV_ALIGN_TOP_RIGHT, -12, y);

    s_set_bk_slider = lv_slider_create(s_set_scr);
    lv_obj_set_width(s_set_bk_slider, LV_PCT(92));
    lv_obj_set_height(s_set_bk_slider, 18);
    lv_slider_set_range(s_set_bk_slider, 0, 100);
    lv_obj_align(s_set_bk_slider, LV_ALIGN_TOP_MID, 0, y + 22);
    lv_obj_add_event_cb(s_set_bk_slider, ui_set_brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_set_bk_slider, ui_set_brightness_cb, LV_EVENT_RELEASED, NULL);

    /* ---------- 底部一行小字说明（单行，240 高屏也放得下：206..225） ---------- */
    lv_obj_t *hint = lv_label_create(s_set_scr);
    lv_label_set_text(hint, "弱=轻碰就醒，强=需要明显晃动");
    if (s_font != NULL) {
        lv_obj_set_style_text_font(hint, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8A9BA8), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 12, y + 46);

    ui_set_refresh_sleep_buttons();
    ui_set_refresh_imu_buttons();
}

/**
 * @brief  进入设置二级页（懒创建 + 同步当前值）
 */
static void ui_settings_open(void)
{
    if (s_set_scr == NULL) {
        ui_set_build();
    }
    if (s_set_scr == NULL) {
        return;
    }

    /* 每次进来都按“当前生效值”同步一遍，避免显示旧值 */
    ui_set_refresh_sleep_buttons();
    ui_set_refresh_imu_buttons();

    int bk = app_settings_get_backlight();
    if (s_set_bk_slider != NULL) {
        lv_slider_set_value(s_set_bk_slider, bk, LV_ANIM_OFF);
    }
    ui_set_update_brightness_label(bk);

    lv_screen_load(s_set_scr);

    /* 设置页打开期间：录音键不生效；但它不是“弹窗”，不阻止自动省电
     * （用户停在设置页走开，屏幕一样该暗下来）。 */
    app_bus_set_ui_page(true);
    app_power_notify_activity();

    ESP_LOGI(TAG, "打开设置页（休眠超时 %u ms，IMU 灵敏度 %u，亮度 %d%%）",
             (unsigned)app_power_get_timeout_ms(),
             (unsigned)app_power_get_imu_sensitivity(), bk);
}

/**
 * @brief  顶栏“设置”被点击：进入设置二级页
 */
static void ui_settings_clicked_cb(lv_event_t *e)
{
    (void)e;
    ui_settings_open();
}

/**
 * @brief  创建顶栏“设置”文字按钮
 *
 * @param  parent 顶栏里的横向容器
 * @note   原来是太阳图标（直接改亮度），现在换成文字按钮进二级页，
 *         亮度设置搬到二级页里了（需求要求）。
 */
static lv_obj_t *ui_create_settings_button(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(btn, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(btn, false);
    /* 按下时给一点浅色底，和 WiFi 按钮保持一致的手感 */
    lv_obj_set_style_bg_color(btn, lv_color_hex(APP_COLOR_WARN), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, ui_settings_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "设置");
    if (s_font != NULL) {
        lv_obj_set_style_text_font(lbl, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(lbl, lv_color_hex(APP_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_center(lbl);

    return btn;
}

/* ============================ 内部函数：WiFi 文字按钮 ============================ */

/**
 * @brief  WiFi 文字被点击：弹出“附近 WiFi”列表
 *
 * @note   这个回调在 LVGL 任务里执行；扫描/连接本身由 app_wifi_task 做，
 *         所以这里不会卡界面（详见 app_wifi_ui.c 文件头）。
 */
static void ui_wifi_clicked_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "点开了 WiFi 状态文字，打开附近 WiFi 列表");
    app_wifi_ui_open();
}

/**
 * @brief  创建左上角的 WiFi 文字按钮
 *
 * @param  parent 顶栏里的横向容器
 * @return 按钮对象（文字 label 由外部继续维护，句柄存在 s_wifi_lbl）
 *
 * @note   需求：原来这里是个**纯文本 label**，现在要能点 —— 做法是把 label 套进一个
 *         背景全透明的按钮里（不画边框/底色，视觉上还是那行字），
 *         按下时给一点半透明底色做“按得动”的反馈。
 */
static lv_obj_t *ui_create_wifi_button(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(btn, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(btn, false);
    /* 按下时给个浅色底：这块屏没有触感反馈，全靠视觉提示 */
    lv_obj_set_style_bg_color(btn, lv_color_hex(APP_COLOR_WARN), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, ui_wifi_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_wifi_lbl = lv_label_create(btn);
    lv_label_set_text(s_wifi_lbl, "WiFi未连接");
    if (s_font) {
        lv_obj_set_style_text_font(s_wifi_lbl, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(s_wifi_lbl, lv_color_hex(APP_COLOR_TEXT), LV_PART_MAIN);
    /* 只留“WiFi + 一档信号”这么长；真要变长了就省略号，绝不挤到右边的状态字 */
    lv_obj_set_style_max_width(s_wifi_lbl, APP_CHAT_WIFI_TEXT_MAX_W, LV_PART_MAIN);
    lv_label_set_long_mode(s_wifi_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_center(s_wifi_lbl);

    return btn;
}

/* ============================ 对外接口 ============================ */

/**
 * @brief  在 LVGL 任务上下文里做一次“整屏强制重绘”（lv_async_call 的回调）
 *
 * 为什么要绕到异步回调：省电唤醒是在 app_power 任务里发生的，那里栈很紧
 * （以前直接 lv_refr_now() 会 stack overflow）。lv_async_call 把“标脏 + 渲染”
 * 排到 LVGL 自己的任务里执行，用的是 LVGL 任务的栈，安全。
 *
 * 日志意义：看到“LVGL 已在任务上下文强制重绘 / 强制刷新完成”，说明问题不在
 * LVGL 没刷新；若此时屏幕仍无画面，就说明是面板/SPI 那条通路（对照 lcd.c 的
 * “清屏探测”日志一起判断）。
 */
static void ui_force_redraw_async(void *p)
{
    (void)p;

    ESP_LOGI(TAG, "LVGL 已在任务上下文强制重绘（screen=%p）", (void *)lv_screen_active());

    /* UI 树自检：子对象数 > 0 说明界面对象还在（只是“没显示”），
     * 子对象数 = 0 说明界面树本身被清空了（那才是“没内容”）。
     * 和唤醒自检彩条对照：看得到彩条 + 子对象数 > 0 → 内容在、但 LVGL 输出没到屏上。 */
    ESP_LOGI(TAG, "唤醒重绘检查：screen=%p 子对象数=%u 显示对象=%p",
             (void *)lv_screen_active(),
             (unsigned)lv_obj_get_child_count(lv_screen_active()),
             (void *)lv_obj_get_display(lv_screen_active()));

    lv_obj_invalidate(lv_screen_active());

    /* 立刻跑一拍刷新，把“到底刷没刷”变成确定事实：
     * 本函数运行在 LVGL 任务里，渲染开销和平时刷新一样，不会撑爆 app_power 的栈 */
    lv_display_t *d = lcd_get_display();
    if (d == NULL) {
        d = lv_display_get_default();
    }
    if (d != NULL) {
        lv_refr_now(d);
        ESP_LOGI(TAG, "LVGL 强制刷新完成（lv_refr_now）");
    } else {
        ESP_LOGW(TAG, "找不到 LVGL display，跳过强制刷新");
    }
}

/**
 * @brief  请求 LVGL 在它自己的任务里强制重绘一次（线程安全）
 *
 * @note   必须在持有 LVGL 锁时调用（lv_async_call 会改 LVGL 内部链表）。
 *         真正干活的是 ui_force_redraw_async()，在 lv_timer_handler 里被调用。
 */
void app_chat_ui_force_redraw_async(void)
{
    lv_async_call(ui_force_redraw_async, NULL);
}

esp_err_t app_chat_ui_init(const lv_font_t *cn_font)
{
    if (s_scr != NULL) {
        ESP_LOGW(TAG, "界面已经创建过了");
        return ESP_OK;
    }

    s_font = cn_font;

    /* 建界面必须持有 LVGL 锁（LVGL 任务在另一个任务里跑） */
    if (!lvgl_port_lock(2000)) {
        ESP_LOGE(TAG, "获取 LVGL 锁失败");
        return ESP_ERR_INVALID_STATE;
    }

    /* ---------- 1. 新建 screen 并加载（覆盖游戏主页，主页对象留在旧 screen 里） ---------- */
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(APP_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_scr, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(s_scr, false);
    lv_screen_load(s_scr);

    /* ---------- 2. 顶部状态栏 ---------- */
    lv_obj_t *topbar = lv_obj_create(s_scr);
    lv_obj_set_size(topbar, LV_PCT(100), APP_CHAT_TOPBAR_H);
    lv_obj_align(topbar, LV_ALIGN_TOP_MID, 0, 0);
    ui_obj_base(topbar, lv_color_hex(APP_COLOR_TOPBAR));

    /* 左上角：WiFi 文字按钮 + “设置”文字按钮。
     * 用一个横向 flex 容器把两者排在一起，这样 WiFi 文字变长（WiFi未连接 ↔ WiFi 强）
     * 时“设置”会自动跟着移动，不需要手算坐标。
     * WiFi 那一段是个**按钮**：点一下弹出附近 WiFi 列表（见 app_wifi_ui.c）。
     * “设置”按钮点开二级页（休眠时间 + 屏幕亮度，见本文件设置页部分）。 */
    lv_obj_t *left_box = lv_obj_create(topbar);
    lv_obj_set_size(left_box, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_flex_flow(left_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_box, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_obj_base(left_box, lv_color_hex(APP_COLOR_TOPBAR));      /* 与顶栏同色 = 视觉透明 */
    lv_obj_set_style_pad_left(left_box, 6, LV_PART_MAIN);       /* 注意：要在 base 之后再设 */
    lv_obj_set_style_pad_column(left_box, 6, LV_PART_MAIN);     /* 文字与按钮间距 */
    lv_obj_align(left_box, LV_ALIGN_LEFT_MID, 0, 0);

    s_wifi_btn = ui_create_wifi_button(left_box);
    (void)s_wifi_btn;

    /* 亮度设置已经搬到设置二级页里，这里只留一个“设置”入口 */
    ui_create_settings_button(left_box);

    s_rec_flag = lv_label_create(topbar);
    lv_label_set_text(s_rec_flag, "待机");
    if (s_font) {
        lv_obj_set_style_text_font(s_rec_flag, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(s_rec_flag, lv_color_hex(APP_COLOR_OK), LV_PART_MAIN);
    lv_obj_align(s_rec_flag, LV_ALIGN_RIGHT_MID, -6, 0);

    /* ---------- 3. 消息区（可滚动） ---------- */
    s_msg_area = lv_obj_create(s_scr);
    lv_obj_set_size(s_msg_area, LV_PCT(100), UI_MSG_AREA_H);
    lv_obj_align(s_msg_area, LV_ALIGN_TOP_MID, 0, APP_CHAT_TOPBAR_H);
    lv_obj_set_style_pad_all(s_msg_area, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_msg_area, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_msg_area, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_msg_area, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_msg_area, lv_color_hex(APP_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_flex_flow(s_msg_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_msg_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_msg_area, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_msg_area, LV_SCROLLBAR_MODE_AUTO);

    /* ---------- 4. 底部状态行（整屏宽 + 超长省略号，避免文字跑出屏幕） ---------- */
    s_status_lbl = lv_label_create(s_scr);
    if (s_font) {
        lv_obj_set_style_text_font(s_status_lbl, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(APP_COLOR_WARN), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(s_status_lbl, BSP_LCD_H_RES - 8);
    lv_label_set_long_mode(s_status_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(s_status_lbl, "正在启动 ...");
    lv_obj_align(s_status_lbl, LV_ALIGN_BOTTOM_MID, 0, -4);
    strncpy(s_status_text, "正在启动 ...", sizeof(s_status_text) - 1);

    /* ---------- 5. 欢迎气泡 ---------- */
    ui_add_bubble("你好！按住 GPIO0 按键说话，松开发送。", false);

    /* ---------- 6. 刷新定时器（LVGL 任务里跑） ---------- */
    s_timer = lv_timer_create(ui_timer_cb, APP_CHAT_UI_TICK_MS, NULL);
    if (s_timer == NULL) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "创建界面定时器失败");
        return ESP_ERR_NO_MEM;
    }

    lvgl_port_unlock();

    ESP_LOGI(TAG, "聊天界面创建完成（%dx%d，消息区 %d 高；打字机 %dms/%d字，追赶阈值 %d 字节）",
             BSP_LCD_H_RES, BSP_LCD_V_RES, UI_MSG_AREA_H,
             APP_CHAT_TYPE_INTERVAL_MS, APP_CHAT_TYPE_UNITS, APP_CHAT_TYPE_CATCHUP_BYTES);
    return ESP_OK;
}

void app_chat_ui_clear(void)
{
    if (s_msg_area == NULL) {
        return;
    }
    if (lvgl_port_lock(1000)) {
        for (int i = 0; i < s_row_count; i++) {
            if (s_rows[i] != NULL) {
                lv_obj_delete(s_rows[i]);
                s_rows[i] = NULL;
            }
        }
        s_row_count   = 0;
        s_type_row    = NULL;
        s_type_label  = NULL;
        s_type_active = false;
        app_bus_set_ui_typing(false);
        lvgl_port_unlock();
    }
}

void app_chat_ui_show_status(const char *text)
{
    app_bus_post_chat(CHAT_MSG_STATUS, text);
}

void app_chat_ui_show_wifi(const char *text)
{
    app_bus_post_chat(CHAT_MSG_WIFI, text);
}

void app_chat_ui_add_message(bool is_user, const char *text)
{
    if (is_user) {
        app_bus_post_chat(CHAT_MSG_USER, text);
    } else {
        /* 外部只给一整段 AI 文本：当成“一段增量 + 结束”，
         * 这样打字机照样逐字播放，也不会一直等后续增量 */
        app_bus_post_chat(CHAT_MSG_AI_DELTA, text);
        app_bus_post_chat(CHAT_MSG_AI_END, NULL);
    }
}

bool app_chat_ui_is_ready(void)
{
    return s_scr != NULL;
}
