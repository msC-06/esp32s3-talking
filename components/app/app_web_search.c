/**
 * @file    app_web_search.c
 * @brief   Tavily 联网搜索实现（POST https://api.tavily.com/search）
 *
 * 流程：组请求体 -> PSRAM 响应缓冲 -> esp_http_client_perform
 *       -> 事件回调拼完整响应 -> cJSON 取 title/content -> 拼纯文本摘要
 *
 * 内存：响应缓冲从 PSRAM 分配；请求体、cJSON 对象、响应缓冲在返回前全部释放。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_web_search.h"
#include "app_config.h"
#include "app_gb2312.h"             /* app_utf8_char_bytes：拼接时按字符边界截断 */

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "cJSON.h"

static const char *TAG = "WEB_SEARCH";

/* ============================ 内部：响应累积 ============================ */

/**
 * @brief  响应累积上下文
 * @note   buf 指向 PSRAM 缓冲，由 HTTP_EVENT_ON_DATA 回调逐段追加；
 *         每次请求都是一次完整的“填满 -> 解析 -> 清空”，所以可以直接复用。
 */
typedef struct {
    char  *buf;         /*!< PSRAM 缓冲 */
    size_t cap;         /*!< 缓冲容量 */
    size_t len;         /*!< 已拼好的字节数 */
    bool   overflow;    /*!< true = 响应比缓冲长，已被截断 */
} ws_resp_t;

/**
 * @brief  HTTP 事件回调：把响应体逐段拼接到 PSRAM 缓冲
 *
 * @note   esp_http_client 每解码出一段 body 就派发一次 HTTP_EVENT_ON_DATA，
 *         同一段只会派发一次（包括 fetch_headers 阶段收到的部分），
 *         所以这样拼出来的响应是完整且不重复的。
 */
static esp_err_t ws_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        ws_resp_t *r = (ws_resp_t *)evt->user_data;
        if (r != NULL && evt->data != NULL && evt->data_len > 0) {
            size_t room = (r->cap > r->len + 1) ? (r->cap - 1 - r->len) : 0;
            size_t n    = ((size_t)evt->data_len <= room) ? (size_t)evt->data_len : room;

            if (n > 0) {
                memcpy(r->buf + r->len, evt->data, n);
                r->len += n;
                r->buf[r->len] = '\0';
            }
            if (n < (size_t)evt->data_len) {
                r->overflow = true;
            }
        }
    }
    return ESP_OK;
}

/* ============================ 内部：文本拼接 ============================ */

/**
 * @brief  往摘要缓冲里追加字符串（同时受 limit 和 out_size 限制，自动补 '\0'）
 *
 * @param  out_buf 输出缓冲
 * @param  out_size 输出缓冲大小
 * @param  used     已用字节数
 * @param  limit    总长度上限（不含结尾 '\0'）
 * @param  src      要追加的字符串（可为 NULL，直接返回）
 * @return 追加后的已用字节数
 *
 * @note   截断位置**必须落在完整 UTF-8 字符的边界上**：
 *         这段文本最终会塞进 role=tool 的消息里发给 DeepSeek，
 *         浏览器/服务端对 JSON 里的非法 UTF-8 字节是零容忍的（直接 400 Invalid UTF-8）。
 *         之前这里是一个字节一个字节地拷，中文标题正好被切在半路，就踩了这个坑。
 */
static size_t ws_append(char *out_buf, size_t out_size, size_t used,
                        size_t limit, const char *src)
{
    if (src == NULL || out_buf == NULL) {
        return used;
    }
    if (limit > out_size - 1) {
        limit = out_size - 1;       /* 再兜一层，绝不允许写越界 */
    }

    while (*src != '\0' && used < limit) {
        int cb = app_utf8_char_bytes((unsigned char)*src);
        if (cb <= 0) {
            cb = 1;
        }
        if (used + (size_t)cb > limit) {
            break;                  /* 放不下一整个字符了：宁可少几个字，不留半个汉字 */
        }
        for (int i = 0; i < cb; i++) {
            out_buf[used++] = src[i];
        }
        src += cb;
    }
    out_buf[used] = '\0';
    return used;
}

/**
 * @brief  打印响应内容（诊断用：只在请求失败时调用）
 */
static void ws_dump_resp(const ws_resp_t *resp)
{
    /** 打印用静态缓冲（1KB，不占网络任务那 8KB 栈） */
    static char s_head[1024];

    if (resp == NULL || resp->len == 0) {
        ESP_LOGE(TAG, "响应体为空（overflow=%d）", resp ? (int)resp->overflow : -1);
        return;
    }
    if (resp->overflow) {
        ESP_LOGE(TAG, "响应超过缓冲上限（%u 字节）已被截断，请加大 APP_WEB_SEARCH_RESP_BUF_SIZE",
                 (unsigned)resp->cap);
    }
    if (resp->len <= 1024) {
        ESP_LOGE(TAG, "响应体共 %u 字节：%s", (unsigned)resp->len, resp->buf);
        return;
    }

    size_t head_len = sizeof(s_head) - 1;
    if (head_len > resp->len) {
        head_len = resp->len;
    }
    memcpy(s_head, resp->buf, head_len);
    s_head[head_len] = '\0';
    ESP_LOGE(TAG, "响应体共 %u 字节，开头：%s ... 结尾：%s",
             (unsigned)resp->len, s_head, resp->buf + resp->len - 128);
}

/* ============================ 对外接口 ============================ */

esp_err_t app_web_search(const char *query, char *out_buf, size_t out_size)
{
    /* ---------- 0. 参数与配置检查 ---------- */
    if (query == NULL || out_buf == NULL || out_size < 2) {
        ESP_LOGE(TAG, "参数非法");
        return ESP_FAIL;
    }
    out_buf[0] = '\0';

    if (query[0] == '\0') {
        ESP_LOGW(TAG, "搜索关键词为空，跳过");
        return ESP_FAIL;
    }

    if (strlen(TAVILY_API_KEY) == 0 || strcmp(TAVILY_API_KEY, "YOUR_TAVILY_API_KEY") == 0) {
        ESP_LOGE(TAG, "还没有配置 Tavily API Key（填 components/app/app_config_secret.h）");
        return ESP_FAIL;
    }

    /* 摘要长度上限：取“配置上限”和“调用者缓冲”里更小的那个 */
    size_t limit = (out_size - 1 < (size_t)APP_WEB_SEARCH_TEXT_MAX)
                   ? (out_size - 1) : (size_t)APP_WEB_SEARCH_TEXT_MAX;

    esp_err_t ret = ESP_FAIL;

    /* ---------- 1. 组请求体（用 cJSON，自动转义引号等特殊字符） ---------- */
    char *body = NULL;
    cJSON *req = cJSON_CreateObject();
    if (req == NULL) {
        ESP_LOGE(TAG, "请求体创建失败");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(req, "query", query);
    cJSON_AddNumberToObject(req, "max_results", APP_WEB_SEARCH_MAX_RESULTS);
    body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (body == NULL) {
        ESP_LOGE(TAG, "请求体序列化失败");
        return ESP_FAIL;
    }
    size_t body_len = strlen(body);

    /* ---------- 2. PSRAM 响应缓冲（长 JSON 也不会被截断） ---------- */
    char *psram_buf = (char *)heap_caps_malloc(APP_WEB_SEARCH_RESP_BUF_SIZE,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (psram_buf == NULL) {
        ESP_LOGE(TAG, "PSRAM 分配 %d 字节失败", APP_WEB_SEARCH_RESP_BUF_SIZE);
        free(body);
        return ESP_FAIL;
    }

    ws_resp_t resp = {
        .buf      = psram_buf,
        .cap      = APP_WEB_SEARCH_RESP_BUF_SIZE,
        .len      = 0,
        .overflow = false,
    };
    psram_buf[0] = '\0';

    /* ---------- 3. 发请求 ---------- */
    char auth[256];
    snprintf(auth, sizeof(auth), "Bearer %s", TAVILY_API_KEY);

    esp_http_client_config_t cfg = {
        .url            = TAVILY_SEARCH_URL,
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = APP_WEB_SEARCH_TIMEOUT_MS,
        .buffer_size    = 2048,
        /* 发出去的只有两行头 + 一个几百字节的 JSON（响应走事件回调直接进 PSRAM 缓冲），
         * 收/发缓冲都在内部堆里且活到 cleanup，和 TLS 峰值重叠，能省就省 */
        .buffer_size_tx = 512,
        .event_handler  = ws_http_event,        /* 响应体靠事件回调拼接 */
        .user_data      = &resp,
#if APP_TLS_USE_CA_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#else
        .skip_cert_common_name_check = true,
#endif
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "HTTP 客户端创建失败");
        free(body);
        heap_caps_free(psram_buf);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, (int)body_len);

    ESP_LOGI(TAG, "搜索：%s（最多 %d 条）", query, APP_WEB_SEARCH_MAX_RESULTS);

    int64_t t_begin = esp_timer_get_time();
    esp_err_t err   = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);
    int       elapsed_ms = (int)((esp_timer_get_time() - t_begin) / 1000);

    /* 响应体已经整段落在 psram_buf 里了，**立刻把连接（含 TLS 会话）拆掉再往下解析**。
     * TLS 会话（in 16KB + 收发缓冲 + 握手结构）和 cJSON 的整棵语法树都吃内部堆，
     * 不拆开就是两座山峰叠在一起 —— 这是「搜索那一步内部 RAM 见底」最便宜的一处：
     * 只是把 cleanup 提前，行为完全不变。 */
    esp_http_client_cleanup(client);
    client = NULL;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "请求失败：%s（耗时 %d ms）", esp_err_to_name(err), elapsed_ms);
        goto cleanup;
    }

    ESP_LOGI(TAG, "响应：HTTP %d，%u 字节，耗时 %d ms",
             status, (unsigned)resp.len, elapsed_ms);

    /* ---------- 4. 状态码检查 ---------- */
    if (status == 429) {
        ESP_LOGE(TAG, "被限流（429），请降低搜索频率或检查套餐额度");
        goto cleanup;
    }
    if (status == 401 || status == 403) {
        ESP_LOGE(TAG, "鉴权失败（HTTP %d），检查 TAVILY_API_KEY", status);
        goto cleanup;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "服务端返回异常状态码：%d", status);
        ws_dump_resp(&resp);
        goto cleanup;
    }
    if (resp.len == 0) {
        ESP_LOGE(TAG, "响应体为空");
        goto cleanup;
    }

    /* ---------- 5. 解析 JSON ---------- */
    cJSON *root = cJSON_Parse(psram_buf);
    if (root == NULL) {
        ESP_LOGE(TAG, "响应不是合法 JSON");
        ws_dump_resp(&resp);
        goto cleanup;
    }

    const cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(results)) {
        /* 出错时 Tavily 返回 {"detail":{...}}，把它打出来便于定位 */
        const cJSON *detail = cJSON_GetObjectItem(root, "detail");
        if (detail != NULL) {
            char *detail_str = cJSON_PrintUnformatted(detail);
            ESP_LOGE(TAG, "服务端返回错误：%s", detail_str ? detail_str : "?");
            free(detail_str);
        } else {
            ESP_LOGE(TAG, "响应里没有 results 数组");
            ws_dump_resp(&resp);
        }
        cJSON_Delete(root);
        goto cleanup;
    }

    /* ---------- 6. 只取 title + content，拼成纯文本摘要 ---------- */
    int    total = cJSON_GetArraySize(results);
    int    taken = 0;
    size_t used  = 0;

    for (int i = 0; i < total && taken < APP_WEB_SEARCH_MAX_RESULTS; i++) {
        const cJSON *item    = cJSON_GetArrayItem(results, i);
        const cJSON *title   = cJSON_GetObjectItem(item, "title");
        const cJSON *content = cJSON_GetObjectItem(item, "content");

        if (!cJSON_IsString(content) || content->valuestring == NULL) {
            continue;       /* 这条没有正文，跳过 */
        }

        char head[16];
        snprintf(head, sizeof(head), "[%d] ", taken + 1);
        used = ws_append(out_buf, out_size, used, limit, head);
        if (cJSON_IsString(title) && title->valuestring != NULL) {
            used = ws_append(out_buf, out_size, used, limit, title->valuestring);
            used = ws_append(out_buf, out_size, used, limit, "\n");
        }
        used = ws_append(out_buf, out_size, used, limit, content->valuestring);
        used = ws_append(out_buf, out_size, used, limit, "\n");

        taken++;
        if (used >= limit) {
            break;          /* 已经写满，后面的不取了 */
        }
    }

    cJSON_Delete(root);

    /* ---------- 7. 结果检查 ---------- */
    if (taken == 0) {
        ESP_LOGW(TAG, "没有搜索到结果（results 共 %d 条）", total);
        snprintf(out_buf, out_size, "（未搜索到相关结果）");
        ret = ESP_OK;       /* 搜索本身是成功的，让模型据此作答 */
        goto cleanup;
    }

    ESP_LOGI(TAG, "整理完成：%d 条，摘要 %u 字符%s",
             taken, (unsigned)used, (used >= limit) ? "（已截断）" : "");
    ret = ESP_OK;

cleanup:
    if (client != NULL) {
        esp_http_client_cleanup(client);    /* 正常路径已经在 perform 之后提前拆掉了 */
    }
    free(body);
    heap_caps_free(psram_buf);      /* 响应缓冲每次都释放，不留驻内存 */
    if (ret != ESP_OK) {
        out_buf[0] = '\0';          /* 失败时保证不会把脏数据交给上层 */
    }
    return ret;
}
