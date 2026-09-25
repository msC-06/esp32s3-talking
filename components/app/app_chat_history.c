/**
 * @file    app_chat_history.c
 * @brief   对话历史管理实现（最多 10 轮 / 20 条，开机清空）
 */

#include <string.h>

#include "app_chat_history.h"
#include "app_config.h"
#include "app_gb2312.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "chat_his";

/* ============================ 内部状态（全部 static） ============================ */

static app_chat_msg_t s_msgs[APP_CHAT_HISTORY_MAX_MSGS];
static int            s_count = 0;
static SemaphoreHandle_t s_lock = NULL;

/* ============================ 内部函数 ============================ */

/** 释放第 index 条并把它之后的消息整体前移 */
static void history_drop(int index)
{
    if (index < 0 || index >= s_count) {
        return;
    }
    if (s_msgs[index].content != NULL) {
        vPortFree(s_msgs[index].content);
        s_msgs[index].content = NULL;
    }
    for (int i = index; i < s_count - 1; i++) {
        s_msgs[i] = s_msgs[i + 1];
    }
    s_count--;
    memset(&s_msgs[s_count], 0, sizeof(app_chat_msg_t));
}

/* ============================ 生命周期 ============================ */

esp_err_t app_chat_history_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            ESP_LOGE(TAG, "互斥锁创建失败");
            return ESP_ERR_NO_MEM;
        }
    }
    app_chat_history_clear();
    ESP_LOGI(TAG, "对话历史已清空（最多保留 %d 轮）", APP_CHAT_HISTORY_MAX_ROUNDS);
    return ESP_OK;
}

void app_chat_history_clear(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    for (int i = 0; i < s_count; i++) {
        if (s_msgs[i].content != NULL) {
            vPortFree(s_msgs[i].content);
        }
    }
    memset(s_msgs, 0, sizeof(s_msgs));
    s_count = 0;
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

esp_err_t app_chat_history_deinit(void)
{
    app_chat_history_clear();
    return ESP_OK;
}

/* ============================ 读写 ============================ */

esp_err_t app_chat_history_add(const char *role, const char *content)
{
    if (role == NULL || content == NULL || s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* 1. 超出一轮上限：丢掉最老的一条；如果最老是 user，就再丢一条 assistant，
     *    保证历史里 user/assistant 基本成对，不会出现“有问无答”。 */
    while (s_count >= APP_CHAT_HISTORY_MAX_MSGS) {
        bool oldest_is_user = (strcmp(s_msgs[0].role, "user") == 0);
        history_drop(0);
        if (oldest_is_user && s_count > 0 && strcmp(s_msgs[0].role, "assistant") == 0) {
            history_drop(0);
        }
    }

    /* 2. 分配并拷贝文本（小对象，用 pvPortMalloc 走内部 RAM） */
    size_t len = strlen(content);
    if (len >= APP_CHAT_HISTORY_MSG_MAX) {
        len = APP_CHAT_HISTORY_MSG_MAX - 1;
        /* 按**完整 UTF-8 字符**收尾：content[len] 是第一个“没被收进来”的字节，
         * 如果它是 0b10xxxxxx 续字节，说明正好切在汉字中间，往回收。
         * 半个汉字进了请求体 JSON，服务端会直接回 400。 */
        while (len > 0 && (((unsigned char)content[len]) & 0xC0U) == 0x80U) {
            len--;
        }
        ESP_LOGW(TAG, "消息过长，截断到 %d 字节（按字符边界）", (int)len);
    }

    char *copy = (char *)pvPortMalloc(len + 1);
    if (copy == NULL) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "历史消息内存分配失败");
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, content, len);
    copy[len] = '\0';

    /* 3. 追加 */
    app_chat_msg_t *slot = &s_msgs[s_count];
    strncpy(slot->role, role, sizeof(slot->role) - 1);
    slot->role[sizeof(slot->role) - 1] = '\0';
    slot->content = copy;
    s_count++;

    ESP_LOGD(TAG, "历史 +[%s] %d 字节，共 %d 条", slot->role, (int)len, s_count);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

int app_chat_history_count(void)
{
    return s_count;
}

const app_chat_msg_t *app_chat_history_get(int index)
{
    if (index < 0 || index >= s_count) {
        return NULL;
    }
    return &s_msgs[index];
}

esp_err_t app_chat_history_to_json(cJSON *messages, size_t budget_bytes)
{
    if (messages == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }

    /*
     * 先算“从哪条开始带”：从最新往最老回溯累加，超出预算的更老消息就不带了。
     * 一直保证**最新的一条一定带**（哪怕它自己就超预算），否则本轮问题都丢了。
     * 估算里给每条加了 32 字节的 JSON 包装开销（{"role":"","content":""}, 等）。
     */
    int    start = s_count;
    size_t used  = 0;
    while (start > 0) {
        const app_chat_msg_t *m   = &s_msgs[start - 1];
        size_t                cost = (m->content != NULL ? strlen(m->content) : 0) +
                                     strlen(m->role) + 32;
        if (budget_bytes > 0 && (used + cost) > budget_bytes && start < s_count) {
            break;
        }
        used += cost;
        start--;
    }
    if (start > 0) {
        ESP_LOGI(TAG, "历史预算 %u 字节：只带最近 %d/%d 条（约 %u 字节）",
                 (unsigned)budget_bytes, s_count - start, s_count, (unsigned)used);
    }

    for (int i = start; i < s_count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            break;
        }
        cJSON_AddStringToObject(item, "role", s_msgs[i].role);
        cJSON_AddStringToObject(item, "content", s_msgs[i].content ? s_msgs[i].content : "");
        cJSON_AddItemToArray(messages, item);
    }

    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    return ESP_OK;
}
