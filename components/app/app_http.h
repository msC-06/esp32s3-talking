/**
 * @file    app_http.h
 * @brief   HTTP 响应体读取工具（app 层公共小工具）
 *
 * 为什么需要它：
 *   本工程的 ASR 上传需要“自己 open / write / read”来流式发送 640KB 的 base64 数据，
 *   不能只用 esp_http_client_perform()。而手工调用 esp_http_client_read() 时，
 *   响应体的交付依赖内部 raw_len/data_process 记账，容易出现“读到的字节数”
 *   与“真正拷进缓冲的字节数”不一致（表现为 JSON 被截断）。
 *
 *   所以这里统一改成 **事件回调累积**：
 *   http_on_body 每解码出一段 body 就会派发一次 HTTP_EVENT_ON_DATA，
 *   同一段数据只会派发一次；fetch_headers() 期间收到的 body 也会派发。
 *   我们把每段都追加进调用者的缓冲，再由 app_http_pump() 把 socket 读干净。
 *   这样无论响应是 Content-Length 还是 chunked，拿到的都是完整、已解码的 body。
 */

#ifndef __APP_HTTP_H
#define __APP_HTTP_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 响应体累积上下文（调用者提供缓冲，本模块只负责填充） */
typedef struct {
    char  *buf;         /*!< 调用者提供的缓冲，会被补 '\0' */
    size_t cap;         /*!< 缓冲容量（含结尾 '\0'） */
    size_t len;         /*!< 已累积的字节数（不含 '\0'） */
    bool   overflow;    /*!< true = 响应比缓冲长，已被截断 */
    /* ---- 下面两个字段用于 SSE 流式模式（见 app_http_set_line_cb） ---- */
    void (*line_cb)(const char *line, void *ctx);   /*!< 每收到完整一行就回调 */
    void  *line_ctx;                                /*!< 回调上下文 */
} app_http_resp_t;

/**
 * @brief  SSE 行回调：每收到一个完整行（\n 结尾）调用一次，\r\n 已去掉
 *
 * @param  line 行内容（'\0' 结尾，不含换行）
 * @param  ctx  注册时传入的上下文
 */
typedef void (*app_http_line_cb_t)(const char *line, void *ctx);

/**
 * @brief  打开“流式逐行模式”：响应边收边按行回调（SSE 用），同时仍攒进 buf
 *
 * @param  resp 上下文
 * @param  cb   行回调（传 NULL 恢复普通的整体累积模式）
 * @param  ctx  回调上下文
 *
 * @note   单行超过 APP_HTTP_SSE_LINE_MAX 时整行丢弃并告警（不会把半行交出去）。
 */
void app_http_set_line_cb(app_http_resp_t *resp, app_http_line_cb_t cb, void *ctx);

/**
 * @brief  初始化响应上下文
 * @param  resp 上下文
 * @param  buf  调用者提供的缓冲
 * @param  cap  缓冲容量
 */
void app_http_resp_init(app_http_resp_t *resp, char *buf, size_t cap);

/**
 * @brief  事件回调：把 HTTP_EVENT_ON_DATA 的数据追加到 resp
 *
 * @note   用法：cfg.event_handler = app_http_event_handler;
 *              cfg.user_data     = &resp;      （两者必须成对设置）
 */
esp_err_t app_http_event_handler(esp_http_client_event_t *evt);

/**
 * @brief  把响应读干净（数据由事件回调累积，这里只是把 socket 抽干）
 *
 * @return 本次从 socket 读出的字节数（不含 fetch_headers 期间收到的部分）；
 *         出错或读完返回 <= 0
 */
int app_http_pump(esp_http_client_handle_t client);

/**
 * @brief  校验收到的字节数与服务端声明的 Content-Length 是否一致
 *
 * @return true 一致（或是 chunked 响应无法比较）；false 不一致（响应可能被截断）
 * @note   只在日志里告警，不改变流程；用于定位“响应不完整”类问题。
 */
bool app_http_check_length(const char *tag, esp_http_client_handle_t client,
                           const app_http_resp_t *resp);

/**
 * @brief  打印响应内容（诊断用：失败时把整个响应打出来，便于定位）
 */
void app_http_dump(const char *tag, const app_http_resp_t *resp);

/**
 * @brief  解析响应里的 JSON 对象（比 cJSON_Parse 宽容一点）
 *
 * @param  resp 响应上下文
 * @return cJSON 对象（调用者负责 cJSON_Delete）；失败返回 NULL
 *
 * @note   先按原样解析；失败时再掐掉 JSON 对象前后可能存在的杂字符重试一次
 *         （个别服务端会在 JSON 前后夹带换行/分块残留）。
 *         注意：如果响应被截断（缺少结尾的 '}'），这里也救不回来，会返回 NULL。
 */
struct cJSON *app_http_parse_json(const app_http_resp_t *resp);

#ifdef __cplusplus
}
#endif

#endif /* __APP_HTTP_H */
