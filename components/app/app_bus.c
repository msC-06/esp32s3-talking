/**
 * @file    app_bus.c
 * @brief   应用总线实现：队列创建、状态机、消息投递
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_bus.h"
#include "app_gb2312.h"

#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "app_bus";

/* ============================ 内部状态（全部 static） ============================ */

static QueueHandle_t key_event_queue   = NULL;  /*!< 按键事件 */
static QueueHandle_t audio_ctrl_queue  = NULL;  /*!< 录音启停命令 */
static QueueHandle_t pcm_queue         = NULL;  /*!< 录音数据 */
static QueueHandle_t asr_result_queue  = NULL;  /*!< ASR 结果 */
static QueueHandle_t llm_result_queue  = NULL;  /*!< LLM 结果 */
static QueueHandle_t chat_msg_queue    = NULL;  /*!< 聊天消息 */
static QueueHandle_t ui_event_queue    = NULL;  /*!< 界面回报事件 */

/** 当前业务状态：32 位对齐读写，单写者 + 多读者，无需加锁 */
static volatile app_state_t s_state = APP_STATE_IDLE;

/** 界面是否正在逐字显示 AI 回复（写者：LVGL 定时器；读者：状态机） */
static volatile bool s_ui_typing = false;

/* ============================ 发送暂存区 ============================ */
/*
 * 为什么不用局部变量发消息：
 *   app_text_msg_t 现在有 2KB（APP_TEXT_MAX_LEN），如果把它当局部变量放在发送函数里，
 *   这几 KB 就落在**调用者的栈**上。而 app_audio_task / app_key_task 的栈只有 4KB，
 *   一进函数就爆栈——现象就是 “InstrFetchProhibited”、返回地址变成 0xffffffff
 *   （栈被写穿，把相邻任务的栈/TCB 冲掉了）。
 *
 *   所以改成“每个队列一个静态暂存区 + 一把互斥锁”：
 *   锁只包住一次 xQueueSend（它会把内容整体拷进队列），持锁时间极短。
 *   暂存区用 EXT_RAM_BSS_ATTR 放 PSRAM，也不占内部 RAM。
 *
 *   注意：暂存区在 PSRAM 里，所以 xQueueSend 期间**不能**被抢占改写——
 *   这就是那把锁存在的意义（PSRAM 上不能做原子性假设）。
 */
EXT_RAM_BSS_ATTR static app_text_msg_t s_asr_stage;    /*!< ASR 结果暂存 */
EXT_RAM_BSS_ATTR static app_text_msg_t s_llm_stage;    /*!< LLM 结果暂存 */
EXT_RAM_BSS_ATTR static chat_msg_t     s_chat_stage;   /*!< 聊天消息暂存 */
EXT_RAM_BSS_ATTR static chat_msg_t     s_chat_trash;   /*!< 只用来接住被丢掉的最老消息 */
static SemaphoreHandle_t               s_stage_lock = NULL;

/* ============================ 内部工具 ============================ */

/**
 * @brief  安全拷贝文本：限长 + **保证不切断多字节 UTF-8 字符**
 *
 * @param  dst      目标缓冲
 * @param  dst_size 目标缓冲总大小（含结尾 '\0'）
 * @param  src      源字符串（NULL 视为空串）
 *
 * @note   strncpy 是“按字节”截断的，中文会被砍成半个字符——
 *         半个字符送到 HTTP 请求体（JSON）里会让服务端直接回 400，
 *         送到 hzk16 字库查表也会显示成乱码，所以这里必须按字符边界收尾。
 */
static void bus_copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';

    size_t len = strlen(dst);
    if (len == strlen(src)) {
        return;                     /* 没被截断，原样可用 */
    }

    /* 被截断了：从尾部回退到最后一个字符的首字节 */
    size_t start = len;
    while (start > 0 && (((unsigned char)dst[start - 1] & 0xC0U) == 0x80U)) {
        start--;                    /* 0b10xxxxxx 是后续字节，继续往前走 */
    }
    if (start == 0) {
        dst[0] = '\0';              /* 全是后续字节，说明被截得很惨，直接留空 */
        return;
    }

    int need = app_utf8_char_bytes((unsigned char)dst[start - 1]);
    if (need <= 0) {
        need = 1;
    }
    if ((start - 1) + (size_t)need > len) {
        dst[start - 1] = '\0';      /* 尾巴缺字节：砍掉这个不完整的字符 */
    }
}

/* ============================ 初始化 ============================ */

esp_err_t app_bus_init(void)
{
    if (key_event_queue != NULL) {
        ESP_LOGW(TAG, "总线已经初始化过了");
        return ESP_ERR_INVALID_STATE;
    }

    key_event_queue  = xQueueCreate(APP_KEY_QUEUE_LEN,   sizeof(app_key_event_t));
    audio_ctrl_queue = xQueueCreate(APP_AUDIO_QUEUE_LEN, sizeof(app_audio_cmd_t));
    pcm_queue        = xQueueCreate(APP_PCM_QUEUE_LEN,   sizeof(app_pcm_msg_t));
    asr_result_queue = xQueueCreate(APP_ASR_QUEUE_LEN,   sizeof(app_text_msg_t));
    llm_result_queue = xQueueCreate(APP_LLM_QUEUE_LEN,   sizeof(app_text_msg_t));
    chat_msg_queue   = xQueueCreate(APP_CHAT_QUEUE_LEN,  sizeof(chat_msg_t));
    ui_event_queue   = xQueueCreate(APP_UI_QUEUE_LEN,    sizeof(app_ui_event_t));

    if (key_event_queue == NULL || audio_ctrl_queue == NULL || pcm_queue == NULL ||
        asr_result_queue == NULL || llm_result_queue == NULL || chat_msg_queue == NULL ||
        ui_event_queue == NULL) {
        ESP_LOGE(TAG, "队列创建失败（内部 RAM 不足？）");
        return ESP_ERR_NO_MEM;
    }

    s_stage_lock = xSemaphoreCreateMutex();
    if (s_stage_lock == NULL) {
        ESP_LOGE(TAG, "发送暂存区互斥锁创建失败");
        return ESP_ERR_NO_MEM;
    }

    s_state     = APP_STATE_IDLE;
    s_ui_typing = false;
    ESP_LOGI(TAG, "队列就绪：key=%d audio=%d pcm=%d asr=%d llm=%d chat=%d ui=%d",
             APP_KEY_QUEUE_LEN, APP_AUDIO_QUEUE_LEN, APP_PCM_QUEUE_LEN,
             APP_ASR_QUEUE_LEN, APP_LLM_QUEUE_LEN, APP_CHAT_QUEUE_LEN, APP_UI_QUEUE_LEN);
    return ESP_OK;
}

/* ============================ 队列句柄 ============================ */

QueueHandle_t app_bus_key_queue(void)   { return key_event_queue; }
QueueHandle_t app_bus_audio_queue(void) { return audio_ctrl_queue; }
QueueHandle_t app_bus_pcm_queue(void)   { return pcm_queue; }
QueueHandle_t app_bus_asr_queue(void)   { return asr_result_queue; }
QueueHandle_t app_bus_llm_queue(void)   { return llm_result_queue; }
QueueHandle_t app_bus_chat_queue(void)  { return chat_msg_queue; }
QueueHandle_t app_bus_ui_queue(void)    { return ui_event_queue; }

/* ============================ 状态机 ============================ */

void app_bus_set_state(app_state_t state)
{
    if (s_state != state) {
        ESP_LOGI(TAG, "状态: %s -> %s", app_bus_state_text(s_state), app_bus_state_text(state));
    }
    s_state = state;
}

app_state_t app_bus_get_state(void)
{
    return s_state;
}

const char *app_bus_state_text(app_state_t state)
{
    switch (state) {
    case APP_STATE_IDLE:        return "按住按键说话";
    case APP_STATE_RECORDING:   return "录音中... 松开发送";
    case APP_STATE_UPLOADING:   return "识别中...";
    case APP_STATE_LLM_REQUEST: return "AI 正在回复...";
    case APP_STATE_AI_TYPING:   return "正在显示回复，请稍候...";
    default:                    return "未知状态";
    }
}

/* ============================ 打字机握手 ============================ */

void app_bus_set_ui_typing(bool typing)
{
    s_ui_typing = typing;
}

bool app_bus_is_ui_typing(void)
{
    return s_ui_typing;
}

/* ============================ 投递接口 ============================ */

esp_err_t app_bus_post_key(app_key_event_t event)
{
    if (key_event_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(key_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "按键事件队列满，丢弃事件 %d", (int)event);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t app_bus_post_audio_cmd(app_audio_cmd_id_t id, bool discard)
{
    if (audio_ctrl_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    app_audio_cmd_t cmd = { .id = id, .discard = discard };
    if (xQueueSend(audio_ctrl_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "录音命令队列满，丢弃命令 %d", (int)id);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t app_bus_post_pcm(const app_pcm_msg_t *msg)
{
    if (pcm_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (msg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xQueueSend(pcm_queue, msg, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGE(TAG, "录音数据队列满，本次录音被丢弃");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t app_bus_post_asr(bool ok, const char *text)
{
    if (asr_result_queue == NULL || s_stage_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 用静态暂存区，避免 2KB 消息落在调用者栈上（见文件上方说明） */
    xSemaphoreTake(s_stage_lock, portMAX_DELAY);

    s_asr_stage.ok = ok;
    bus_copy_text(s_asr_stage.text, sizeof(s_asr_stage.text), text);

    BaseType_t sent = xQueueSend(asr_result_queue, &s_asr_stage, pdMS_TO_TICKS(200));
    xSemaphoreGive(s_stage_lock);

    if (sent != pdTRUE) {
        ESP_LOGW(TAG, "ASR 结果队列满");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t app_bus_post_llm(bool ok, const char *text)
{
    if (llm_result_queue == NULL || s_stage_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_stage_lock, portMAX_DELAY);

    s_llm_stage.ok = ok;
    bus_copy_text(s_llm_stage.text, sizeof(s_llm_stage.text), text);

    BaseType_t sent = xQueueSend(llm_result_queue, &s_llm_stage, pdMS_TO_TICKS(200));
    xSemaphoreGive(s_stage_lock);

    if (sent != pdTRUE) {
        ESP_LOGW(TAG, "LLM 结果队列满");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t app_bus_post_chat(chat_msg_type_t type, const char *text)
{
    if (chat_msg_queue == NULL || s_stage_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * AI 流式增量（CHAT_MSG_AI_DELTA）不能丢：
     *   丢一段就是回复缺字，用户看到的就是残句。
     *   LVGL 定时器每 30ms 会清空整个队列（一次 while 循环取干净），
     *   所以这里等一小会儿必然能等到空位；等不到才记日志放弃。
     *   注意此时持锁等待是安全的：取消息的一方（LVGL 任务）不碰这把锁。
     */
    const TickType_t wait = (type == CHAT_MSG_AI_DELTA) ? pdMS_TO_TICKS(300) : 0;

    xSemaphoreTake(s_stage_lock, portMAX_DELAY);

    s_chat_stage.type = type;
    bus_copy_text(s_chat_stage.text, sizeof(s_chat_stage.text), text);

    BaseType_t sent = xQueueSend(chat_msg_queue, &s_chat_stage, wait);
    if (sent != pdTRUE && wait == 0) {
        /* 状态类消息属于“最新值有效”，队列满时丢掉最老的一条再重试，避免状态卡住 */
        if (xQueueReceive(chat_msg_queue, &s_chat_trash, 0) == pdTRUE) {
            sent = xQueueSend(chat_msg_queue, &s_chat_stage, 0);
        }
        ESP_LOGD(TAG, "聊天队列满，丢弃了一条旧消息");
    }

    /* 长度在锁内算好：出了临界区这个暂存区可能已经被别的发送者改写 */
    size_t stage_len = strlen(s_chat_stage.text);
    xSemaphoreGive(s_stage_lock);

    if (sent != pdTRUE) {
        if (type == CHAT_MSG_AI_DELTA) {
            ESP_LOGW(TAG, "聊天队列持续满，丢了 %u 字节流式增量", (unsigned)stage_len);
        }
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t app_bus_post_chatf(chat_msg_type_t type, const char *fmt, ...)
{
    if (chat_msg_queue == NULL || s_stage_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 直接在静态暂存区里格式化，不用 1KB 的局部 buf */
    xSemaphoreTake(s_stage_lock, portMAX_DELAY);

    s_chat_stage.type = type;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_chat_stage.text, sizeof(s_chat_stage.text), fmt, ap);
    va_end(ap);

    BaseType_t sent = xQueueSend(chat_msg_queue, &s_chat_stage, 0);
    if (sent != pdTRUE) {
        if (xQueueReceive(chat_msg_queue, &s_chat_trash, 0) == pdTRUE) {
            sent = xQueueSend(chat_msg_queue, &s_chat_stage, 0);
        }
    }

    xSemaphoreGive(s_stage_lock);
    return (sent == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t app_bus_post_typing_done(void)
{
    if (ui_event_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    app_ui_event_t ev = APP_UI_EVENT_TYPING_DONE;
    if (xQueueSend(ui_event_queue, &ev, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "界面事件队列满，打字完成事件未能送达");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
