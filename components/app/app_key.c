/**
 * @file    app_key.c
 * @brief   录音按键消抖状态机 + 业务状态机任务实现
 */

#include <string.h>

#include "app_key.h"
#include "app_bus.h"
#include "app_config.h"
#include "app_power.h"          /* 软省电期间屏蔽录音键（按键只当唤醒） */
#include "app_wifi.h"           /* 按下录音键前先看 WiFi 连没连（未连接就不录音、不上传） */

#include "bsp/bsp_gpio.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "APP_KEY";

/* ============================ 按键状态机 ============================ */

/** 消抖状态机的状态（与设计文档一致） */
typedef enum {
    KEY_FSM_IDLE = 0,           /*!< 空闲：等待按下 */
    KEY_FSM_DEBOUNCE_PRESS,     /*!< 按下消抖中 */
    KEY_FSM_PRESSED,            /*!< 已稳定按下 */
    KEY_FSM_DEBOUNCE_RELEASE,   /*!< 松开消抖中 */
    KEY_FSM_RELEASED,           /*!< 已稳定松开，下一个周期回到 IDLE */
} key_fsm_t;

/* ============================ 内部状态（全部 static） ============================ */

static esp_timer_handle_t s_timer      = NULL;
static key_fsm_t          s_state      = KEY_FSM_IDLE;
static uint8_t            s_stable_cnt = 0;         /*!< 连续相同电平的采样次数 */
static bool               s_pressed    = false;     /*!< 消抖后的按键状态 */
static uint32_t           s_press_ms   = 0;         /*!< 已按下时长（毫秒） */
static bool               s_timeout_fired = false;  /*!< 本次按下是否已经报过超时 */

/**
 * 接收 ASR / LLM 结果用的消息缓冲
 * @note  **必须放静态区**：app_text_msg_t 有 2KB，本任务栈只有几 KB，
 *        放局部变量会直接写穿自己的栈（现象：任务莫名跑飞）。
 *        只有本任务读这两个队列，所以静态复用是安全的。
 */
static app_text_msg_t     s_rx_msg;

/**
 * “界面打字”兜底超时（lv_tick 无关，用 FreeRTOS tick 计）
 * @note  正常流程是界面显示完最后一段后投递 APP_UI_EVENT_TYPING_DONE，
 *        状态机收到才回到 IDLE。但万一那个事件丢了（队列满 / 界面任务异常），
 *        状态机会永远卡在 AI_TYPING、按键再也按不动——
 *        所以这里加一个兜底：超过 APP_CHAT_TYPING_TIMEOUT_MS 强制回 IDLE。
 */
static uint32_t           s_typing_deadline = 0;

/* ============================ 按键扫描（20ms 周期回调） ============================ */

/**
 * @brief  esp_timer 20ms 周期回调：采样 GPIO -> 消抖 -> 投递事件
 *
 * @note   运行在 esp_timer 任务上下文，只做 GPIO 读取和队列投递，很快返回。
 */
static void key_scan_cb(void *arg)
{
    (void)arg;
    const bool raw = bsp_gpio_key_is_pressed();

    switch (s_state) {
    case KEY_FSM_IDLE:
        if (raw) {
            s_stable_cnt = 1;
            s_state      = KEY_FSM_DEBOUNCE_PRESS;
        }
        break;

    case KEY_FSM_DEBOUNCE_PRESS:
        if (raw) {
            if (++s_stable_cnt >= APP_KEY_DEBOUNCE_SAMPLES) {   /* 稳定按下 */
                s_state         = KEY_FSM_PRESSED;
                s_pressed       = true;
                s_press_ms      = 0;
                s_timeout_fired = false;
                ESP_LOGI(TAG, "按键按下");
                app_bus_post_key(APP_KEY_EVENT_PRESS);
            }
        } else {                                                 /* 抖动，回退 */
            s_stable_cnt = 0;
            s_state      = KEY_FSM_IDLE;
        }
        break;

    case KEY_FSM_PRESSED:
        s_press_ms += APP_KEY_SCAN_MS;

        if (!raw) {                                              /* 可能松开了 */
            s_stable_cnt = 1;
            s_state      = KEY_FSM_DEBOUNCE_RELEASE;
        } else if (s_press_ms >= APP_KEY_MAX_RECORD_MS && !s_timeout_fired) {
            s_timeout_fired = true;
            ESP_LOGW(TAG, "录音超过 %d 秒，自动停止", APP_KEY_MAX_RECORD_MS / 1000);
            app_bus_post_key(APP_KEY_EVENT_TIMEOUT);
            s_stable_cnt = 1;
            s_state      = KEY_FSM_DEBOUNCE_RELEASE;              /* 等真正松开 */
        }
        break;

    case KEY_FSM_DEBOUNCE_RELEASE:
        if (!raw) {
            if (++s_stable_cnt >= APP_KEY_DEBOUNCE_SAMPLES) {      /* 稳定松开 */
                s_pressed = false;
                s_state   = KEY_FSM_RELEASED;
                if (!s_timeout_fired) {                            /* 超时那次已经上报过 */
                    ESP_LOGI(TAG, "按键松开（按住 %u ms）", (unsigned)s_press_ms);
                    app_bus_post_key(APP_KEY_EVENT_RELEASE);
                }
                s_timeout_fired = false;
            }
        } else {                                                   /* 抖动，回到按下 */
            s_stable_cnt = 0;
            s_state      = KEY_FSM_PRESSED;
        }
        break;

    case KEY_FSM_RELEASED:
    default:
        s_state      = KEY_FSM_IDLE;
        s_stable_cnt = 0;
        break;
    }
}

/* ============================ 初始化 ============================ */

esp_err_t app_key_init(void)
{
    esp_err_t err = bsp_gpio_key_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "按键 GPIO 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    if (s_timer != NULL) {
        return ESP_OK;
    }

    const esp_timer_create_args_t args = {
        .callback        = key_scan_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "app_key_scan",
    };
    err = esp_timer_create(&args, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建扫描定时器失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_timer_start_periodic(s_timer, (uint64_t)APP_KEY_SCAN_MS * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动扫描定时器失败: %s", esp_err_to_name(err));
        esp_timer_delete(s_timer);
        s_timer = NULL;
        return err;
    }

    ESP_LOGI(TAG, "按键就绪：GPIO%d，扫描周期 %d ms，消抖 %d 次，最长录音 %d 秒",
             (int)BSP_KEY_GPIO, APP_KEY_SCAN_MS, APP_KEY_DEBOUNCE_SAMPLES,
             APP_KEY_MAX_RECORD_MS / 1000);
    return ESP_OK;
}

esp_err_t app_key_deinit(void)
{
    if (s_timer != NULL) {
        esp_timer_stop(s_timer);
        esp_timer_delete(s_timer);
        s_timer = NULL;
    }
    s_state   = KEY_FSM_IDLE;
    s_pressed = false;
    return ESP_OK;
}

bool app_key_is_pressed(void)
{
    return s_pressed;
}

/* ============================ 业务状态机任务 ============================ */

/**
 * @brief  开始一轮录音（按键按下时由状态机调用）
 */
static void flow_begin_recording(void)
{
    app_bus_post_audio_cmd(APP_AUDIO_CMD_START, false);
    app_bus_set_state(APP_STATE_RECORDING);
    app_bus_post_chat(CHAT_MSG_STATUS, "录音中... 松开发送");
}

/**
 * @brief  结束一轮录音并把数据交给网络任务
 *
 * @param  discard true = 录音太短，丢弃不放
 *
 * @note   松手时如果 WiFi 已经断开，这里直接丢弃、**不进入上传流程**
 *         （需求：WiFi 未连接时不再尝试上传，直到连上才恢复）。
 *         这样网络任务连 PCM 消息都收不到，连“白等 20 秒”都不会发生。
 */
static void flow_end_recording(bool discard)
{
    if (!discard && !app_wifi_is_connected()) {
        ESP_LOGW(TAG, "录音结束时 WiFi 已断开，丢弃本次录音（不尝试上传）");
        app_bus_post_audio_cmd(APP_AUDIO_CMD_STOP, true);   /* discard：录音器回到安全状态 */
        app_bus_post_chat(CHAT_MSG_ERROR, "WiFi 已断线，本次录音未上传");
        app_bus_set_state(APP_STATE_IDLE);
        return;
    }

    app_bus_post_audio_cmd(APP_AUDIO_CMD_STOP, discard);

    if (discard) {
        app_bus_post_chat(CHAT_MSG_ERROR, "说话时间太短，请按住多说一会儿");
        app_bus_set_state(APP_STATE_IDLE);
    } else {
        app_bus_set_state(APP_STATE_UPLOADING);
        app_bus_post_chat(CHAT_MSG_STATUS, "识别中...");
    }
}

void app_key_task(void *arg)
{
    (void)arg;

    QueueHandle_t key_q = app_bus_key_queue();
    QueueHandle_t asr_q = app_bus_asr_queue();
    QueueHandle_t llm_q = app_bus_llm_queue();
    QueueHandle_t ui_q  = app_bus_ui_queue();

    if (key_q == NULL || asr_q == NULL || llm_q == NULL || ui_q == NULL) {
        ESP_LOGE(TAG, "队列未初始化，任务退出");
        vTaskDelete(NULL);
        return;
    }

    TickType_t press_tick = 0;

    ESP_LOGI(TAG, "按键业务状态机任务启动（Core %d）", xPortGetCoreID());

    for (;;) {
        /* ---------------- 1. 按键事件 ---------------- */
        app_key_event_t key_ev;
        if (xQueueReceive(key_q, &key_ev, 0) == pdTRUE) {
            app_state_t state = app_bus_get_state();

            switch (key_ev) {
            case APP_KEY_EVENT_PRESS:
                /*
                 * 软省电中：录音键被屏蔽。
                 * 此时用户按下去的本意几乎一定是“把屏幕弄亮/叫醒设备”，
                 * 所以只刷新一次活动时间（让低功耗任务立刻退出软省电），
                 * 不录音、不弹窗、不改状态机。
                 */
                if (app_power_is_sleeping()) {
                    app_power_notify_activity();
                    ESP_LOGI(TAG, "软省电中：录音键已屏蔽，本次按键只用于唤醒");
                    break;
                }
                if (app_bus_is_ui_modal() || app_bus_is_ui_page()) {
                    /* 界面上正开着弹窗（背光设置 / WiFi 列表 / 输密码）或设置二级页，
                     * 这时按录音键一定是误触：屏幕上正输密码呢，别冒出“录音中...” */
                    ESP_LOGW(TAG, "界面弹窗打开中，忽略这次按下");
                    break;
                }
                if (state != APP_STATE_IDLE) {
                    /* 上一轮还没收尾（含“界面正在逐字显示回复”），这次按下不生效 */
                    ESP_LOGW(TAG, "当前状态不允许录音（%s），忽略这次按下",
                             app_bus_state_text(state));
                    app_bus_post_chat(CHAT_MSG_STATUS, app_bus_state_text(state));
                    break;
                }
                /*
                 * WiFi 未连接：不开始录音、不进入上传流程，只弹窗提示。
                 *
                 * 为什么在“按下”这一步就拦：录音一旦开始，松手就会把 PCM 投给网络任务，
                 * 而未连接时上传注定失败（还会白占 20 秒等链路）。所以直接在这里挡住，
                 * 并把「WiFi未连接」投给界面弹窗（CHAT_MSG_WIFI_ALERT）。
                 * 真正的兜底在 app_net_task 里（录音途中掉线那种情况也不会尝试上传）。
                 * 界面弹窗会置“界面模态”，弹窗开着时继续按也只会被忽略；
                 * 等 WiFi 连上，界面自动收起弹窗，按键录音随之恢复。
                 */
                if (!app_wifi_is_connected()) {
                    ESP_LOGW(TAG, "WiFi 未连接，忽略本次录音并弹出提示");
                    app_bus_post_chat(CHAT_MSG_WIFI_ALERT, "WiFi未连接");
                    break;
                }
                press_tick = xTaskGetTickCount();
                flow_begin_recording();
                break;

            case APP_KEY_EVENT_RELEASE:
            case APP_KEY_EVENT_TIMEOUT: {
                if (state != APP_STATE_RECORDING) {
                    break;      /* 没在录音，忽略（例如超时后又松开） */
                }
                uint32_t held_ms = (uint32_t)((xTaskGetTickCount() - press_tick) *
                                              portTICK_PERIOD_MS);
                bool too_short = (key_ev == APP_KEY_EVENT_RELEASE) &&
                                 (held_ms < APP_KEY_MIN_RECORD_MS);
                flow_end_recording(too_short);
                break;
            }
            default:
                break;
            }
        }

        /* ---------------- 2. ASR 结果 ----------------
         * 注意：成功时的“用户气泡”已经由网络任务在拿到识别结果那一刻直接投递了
         * （见 app_net.c），这里不再重复投递，否则界面上会出现两条一样的话；
         * 这里只负责切状态（界面底部状态行会自动跟着状态走）。 */
        app_text_msg_t *msg = &s_rx_msg;    /* 静态缓冲，别放栈上 */
        if (xQueueReceive(asr_q, msg, 0) == pdTRUE) {
            if (!msg->ok) {
                ESP_LOGW(TAG, "识别失败：%s", msg->text);
                app_bus_post_chat(CHAT_MSG_ERROR, msg->text);
                app_bus_set_state(APP_STATE_IDLE);
            } else {
                ESP_LOGI(TAG, "识别结果：%s", msg->text);
                app_bus_set_state(APP_STATE_LLM_REQUEST);
            }
        }

        /* ---------------- 3. LLM 结果 ----------------
         * 注意：AI 回复的正文**不是**在这里显示——网络任务每收到一段流式增量
         * 就直接投递 CHAT_MSG_AI_DELTA 给界面（打字机逐字显示），
         * 这里只负责收尾：如果界面还在打字，就先进 AI_TYPING 状态等它打完，
         * 期间按键不响应（避免用户对着半句话追问，把流式通道搅乱）。 */
        if (xQueueReceive(llm_q, msg, 0) == pdTRUE) {
            if (!msg->ok) {
                ESP_LOGW(TAG, "AI 回复失败：%s", msg->text);
                app_bus_post_chat(CHAT_MSG_ERROR, msg->text);
                s_typing_deadline = 0;
                app_bus_set_state(APP_STATE_IDLE);
            } else if (app_bus_is_ui_typing()) {
                s_typing_deadline = xTaskGetTickCount() +
                                    pdMS_TO_TICKS(APP_CHAT_TYPING_TIMEOUT_MS);
                app_bus_set_state(APP_STATE_AI_TYPING);
            } else {
                /* 界面已经追平（回复很短时很常见）：直接回待机 */
                s_typing_deadline = 0;
                app_bus_set_state(APP_STATE_IDLE);
            }
            app_bus_post_chat(CHAT_MSG_STATUS, app_bus_state_text(app_bus_get_state()));
        }

        /* ---------------- 4. 界面回报事件 ---------------- */
        app_ui_event_t ui_ev;
        if (xQueueReceive(ui_q, &ui_ev, 0) == pdTRUE) {
            if (ui_ev == APP_UI_EVENT_TYPING_DONE) {
                s_typing_deadline = 0;
                if (app_bus_get_state() == APP_STATE_AI_TYPING) {
                    ESP_LOGI(TAG, "回复已显示完，回到待机");
                    app_bus_set_state(APP_STATE_IDLE);
                    app_bus_post_chat(CHAT_MSG_STATUS, app_bus_state_text(APP_STATE_IDLE));
                }
            }
        }

        /* ---------------- 5. 打字兜底超时 ---------------- */
        if (s_typing_deadline != 0 &&
            (int32_t)(xTaskGetTickCount() - s_typing_deadline) >= 0) {
            s_typing_deadline = 0;
            if (app_bus_get_state() == APP_STATE_AI_TYPING) {
                ESP_LOGW(TAG, "等界面打字超过 %d 秒，强制回到待机",
                         APP_CHAT_TYPING_TIMEOUT_MS / 1000);
                app_bus_set_state(APP_STATE_IDLE);
                app_bus_post_chat(CHAT_MSG_STATUS, app_bus_state_text(APP_STATE_IDLE));
            }
        }

        /* 20ms 轮询一次，够快也不会白占 CPU */
        vTaskDelay(pdMS_TO_TICKS(APP_KEY_SCAN_MS));
    }
}
