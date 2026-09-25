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
#include "app_time.h"               /* 待机时在状态行后面挂一个 HH:MM */

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
static lv_obj_t        *s_wifi_lbl    = NULL;   /*!< 顶部 WiFi 状态 */
static lv_obj_t        *s_rec_flag    = NULL;   /*!< 顶部录音/处理指示 */
static lv_timer_t      *s_timer       = NULL;

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

            default:
                break;
            }
        }
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

/* ============================ 对外接口 ============================ */

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

    s_wifi_lbl = lv_label_create(topbar);
    lv_label_set_text(s_wifi_lbl, "WiFi: 连接中...");
    if (s_font) {
        lv_obj_set_style_text_font(s_wifi_lbl, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(s_wifi_lbl, lv_color_hex(APP_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_max_width(s_wifi_lbl, BSP_LCD_H_RES - 80, LV_PART_MAIN);
    lv_label_set_long_mode(s_wifi_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_wifi_lbl, LV_ALIGN_LEFT_MID, 6, 0);

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
