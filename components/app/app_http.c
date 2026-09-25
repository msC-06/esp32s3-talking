/**
 * @file    app_http.c
 * @brief   HTTP 响应体累积与读取实现
 */

#include <stdlib.h>
#include <string.h>

#include "app_http.h"
#include "app_config.h"     /* APP_HTTP_SSE_LINE_MAX */

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "APP_HTTP";

/* ============================ SSE 行缓冲 ============================ */

/**
 * 行缓冲放静态区
 * @note  SSE 只有 app_llm 一个使用者，而且都在网络任务里串行执行，
 *        所以静态缓冲是安全的（也避免在任务栈上占 1KB）。
 */
static char   s_sse_line[APP_HTTP_SSE_LINE_MAX];
static size_t s_sse_len  = 0;
static bool   s_sse_drop = false;   /*!< 当前行太长，丢弃到下一个换行 */

/**
 * @brief  把一段新收到的数据按行切分，逐行回调
 */
static void app_http_feed_lines(const char *data, size_t len, app_http_resp_t *resp)
{
    for (size_t i = 0; i < len; i++) {
        const char c = data[i];

        if (c == '\n') {
            if (!s_sse_drop) {
                size_t n = s_sse_len;
                if (n > 0 && s_sse_line[n - 1] == '\r') {
                    n--;                    /* 去掉 CRLF 里的 \r */
                }
                s_sse_line[n] = '\0';
                if (resp->line_cb != NULL) {
                    resp->line_cb(s_sse_line, resp->line_ctx);
                }
            }
            s_sse_len  = 0;
            s_sse_drop = false;
        } else if (s_sse_drop) {
            /* 丢弃这一行的剩余部分 */
        } else if (s_sse_len + 1 < sizeof(s_sse_line)) {
            s_sse_line[s_sse_len++] = c;
        } else {
            ESP_LOGW(TAG, "SSE 单行超过 %u 字节，整行丢弃", (unsigned)sizeof(s_sse_line));
            s_sse_drop = true;
        }
    }
}

/** 单次 dump 最多完整打印多少字节（超出就只打头尾） */
#define APP_HTTP_DUMP_MAX   4096

/* ============================ 对外接口 ============================ */

void app_http_resp_init(app_http_resp_t *resp, char *buf, size_t cap)
{
    if (resp == NULL) {
        return;
    }
    resp->buf      = buf;
    resp->cap      = cap;
    resp->len      = 0;
    resp->overflow = false;
    resp->line_cb  = NULL;
    resp->line_ctx = NULL;
    if (buf != NULL && cap > 0) {
        buf[0] = '\0';
    }
}

void app_http_set_line_cb(app_http_resp_t *resp, app_http_line_cb_t cb, void *ctx)
{
    if (resp == NULL) {
        return;
    }
    resp->line_cb  = cb;
    resp->line_ctx = ctx;
    s_sse_len      = 0;
    s_sse_drop     = false;
}

esp_err_t app_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt == NULL) {
        return ESP_OK;
    }

    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        app_http_resp_t *resp = (app_http_resp_t *)evt->user_data;
        if (resp == NULL || evt->data == NULL || evt->data_len <= 0) {
            return ESP_OK;
        }

        /* 流式模式：逐行回调（SSE 用）。仍继续把原始响应攒进 buf，
         * 这样服务端返回非 200（错误 JSON 不是 SSE 格式）时还能解析出错误信息 */
        if (resp->line_cb != NULL) {
            app_http_feed_lines((const char *)evt->data, (size_t)evt->data_len, resp);
        }

        if (resp->buf != NULL) {
            size_t room = (resp->cap > resp->len + 1) ? (resp->cap - 1 - resp->len) : 0;
            size_t n    = ((size_t)evt->data_len <= room) ? (size_t)evt->data_len : room;

            if (n > 0) {
                memcpy(resp->buf + resp->len, evt->data, n);
                resp->len += n;
                resp->buf[resp->len] = '\0';
            }
            if (n < (size_t)evt->data_len) {
                resp->overflow = true;
            }
        }
    }
    return ESP_OK;
}

int app_http_pump(esp_http_client_handle_t client)
{
    char sink[512];
    int  pumped = 0;

    if (client == NULL) {
        return -1;
    }

    /* 正常情况下 1~2 次就能把 body 抽干；上限只是防御服务端不收尾 */
    for (int i = 0; i < 4096; i++) {
        int r = esp_http_client_read(client, sink, sizeof(sink));
        if (r <= 0) {
            break;      /* 0 = 读完；<0 = 超时/出错，数据已经通过事件回调拿到了 */
        }
        pumped += r;
    }
    return pumped;
}

bool app_http_check_length(const char *tag, esp_http_client_handle_t client,
                           const app_http_resp_t *resp)
{
    if (tag == NULL || client == NULL || resp == NULL) {
        return false;
    }

    int64_t declared = esp_http_client_get_content_length(client);
    if (declared <= 0) {
        return true;        /* chunked 或没有 Content-Length，没法比较 */
    }
    if ((size_t)declared != resp->len) {
        ESP_LOGW(tag, "响应长度不符：服务端声明 %lld 字节，实际收到 %u 字节（可能被截断）",
                 (long long)declared, (unsigned)resp->len);
        return false;
    }
    return true;
}

void app_http_dump(const char *tag, const app_http_resp_t *resp)
{
    /** 打印用的静态缓冲（1KB，刚好够看清响应开头，也不占调用者栈） */
    static char s_head[1024];

    if (resp == NULL) {
        return;
    }
    if (resp->len == 0) {
        ESP_LOGE(tag, "响应体为空（overflow=%d）", (int)resp->overflow);
        return;
    }
    if (resp->overflow) {
        ESP_LOGE(tag, "响应超过缓冲上限（%u 字节），已被截断，请加大接收缓冲！",
                 (unsigned)resp->cap);
    }

    if (resp->len <= APP_HTTP_DUMP_MAX) {
        ESP_LOGE(tag, "响应体共 %u 字节%s：%s",
                 (unsigned)resp->len, resp->overflow ? "（已截断）" : "", resp->buf);
        return;
    }

    /* 太长时打印头尾各一段 */
    size_t head_len = sizeof(s_head) - 1;
    if (head_len > resp->len) {
        head_len = resp->len;
    }
    memcpy(s_head, resp->buf, head_len);
    s_head[head_len] = '\0';
    ESP_LOGE(tag, "响应体共 %u 字节%s，开头：%s ... 结尾：%s",
             (unsigned)resp->len, resp->overflow ? "（已截断）" : "",
             s_head, resp->buf + resp->len - 128);
}

struct cJSON *app_http_parse_json(const app_http_resp_t *resp)
{
    if (resp == NULL || resp->buf == NULL || resp->len == 0) {
        return NULL;
    }

    /* 1. 正常解析 */
    cJSON *root = cJSON_Parse(resp->buf);
    if (root != NULL) {
        return root;
    }

    /* 2. 兜底：掐掉第一个 '{' 之前、最后一个 '}' 之后的杂字符再试 */
    const char *begin = strchr(resp->buf, '{');
    const char *end   = strrchr(resp->buf, '}');
    if (begin == NULL || end == NULL || end <= begin) {
        return NULL;        /* 连大括号都不完整（例如响应被截断），救不回来 */
    }

    size_t len = (size_t)(end - begin) + 1U;
    char  *tmp = (char *)malloc(len + 1U);
    if (tmp == NULL) {
        return NULL;
    }
    memcpy(tmp, begin, len);
    tmp[len] = '\0';

    root = cJSON_Parse(tmp);
    if (root != NULL) {
        ESP_LOGW(TAG, "响应前后有杂字符，已裁剪后解析成功（裁剪掉 %u 字节）",
                 (unsigned)(resp->len - len));
    }
    free(tmp);
    return root;
}
