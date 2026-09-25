/**
 * @file    app_llm.h
 * @brief   DeepSeek 大模型对话（Chat Completions 流式 + web_search Function Calling）
 *
 * 分层：app 层，用 esp_http_client + cJSON。
 *
 * 请求体（自动带上对话历史与工具定义）：
 *   {
 *     "model": "deepseek-chat",
 *     "messages": [
 *       {"role":"system","content":"你是一个友好的对话助手，请用简洁中文回答。"},
 *       ... 历史 user/assistant（预算内的最近若干轮）...
 *       {"role":"user","content":"<本次识别文本>"}
 *     ],
 *     "tools": [{"type":"function","function":{"name":"web_search", ...}}],
 *     "stream": true,
 *     "max_tokens": 512
 *   }
 *
 * 流式（打字机效果的来源）：
 *   stream=true 时会解析 SSE，每收到一段正文增量就**立刻**回调一次
 *   app_llm_delta_cb_t，界面据此逐字显示；同时正文也会累积进 reply。
 *   若流式没拿到任何内容（服务端不支持、网络中断等），会自动降级为整段请求，
 *   此时回调仍然会被调用一次（把整段当成一段增量），界面逻辑无需区分两种情况。
 *
 * 当模型返回 tool_calls 时，本模块会自动调用 app_web_search() 联网搜索，
 * 并把搜索结果作为 role=tool 的消息再问一次，最终把答复写进 reply。
 * 调用者（app_net_task）不需要关心是否发生了搜索。
 *
 * 线程：只有网络任务（app_net_task）调用。
 */

#ifndef __APP_LLM_H
#define __APP_LLM_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  流式正文增量回调
 *
 * @param  text 本次新增的正文片段（UTF-8，可能不以字符边界结尾，调用方需自行按字符拼接）
 * @param  ctx  调用 app_llm_chat_stream() 时传入的上下文
 *
 * @warning 回调是在**网络任务**上下文里同步执行的，里面不要做耗时操作，
 *          更不能去碰 LVGL 控件（要么投队列，要么自己 take LVGL 锁）。
 */
typedef void (*app_llm_delta_cb_t)(const char *text, void *ctx);

/**
 * @brief  初始化 LLM 模块（清空对话历史）
 * @return ESP_OK
 */
esp_err_t app_llm_init(void);

/**
 * @brief  发起一轮对话（流式；内部可能包含“联网搜索 + 追问”两次请求）
 *
 * @param  user_text   用户输入（ASR 识别文本，UTF-8）
 * @param  cb          正文增量回调，可为 NULL（只要最终结果）
 * @param  cb_ctx      传给 cb 的上下文
 * @param  reply       输出：AI 回复全文（UTF-8，已去掉首尾空白）
 * @param  reply_size  输出缓冲区大小
 * @return ESP_OK 成功；
 *         ESP_ERR_INVALID_ARG 参数错；
 *         ESP_ERR_INVALID_STATE 没配 API Key；
 *         ESP_ERR_TIMEOUT 超时；
 *         ESP_FAIL 网络错误 / 服务端返回错误（reply 里有中文错误说明）
 *
 * @note   成功时会把 user 与 assistant 两条消息追加进对话历史
 *         （工具轮次不落历史，原因见 app_llm.c 文件头说明）。
 * @note   搜索失败不会让整轮对话失败：会如实把失败告诉模型，让它自己回答。
 * @note   一旦已经通过 cb 送出过正文，就不会再降级重试，
 *         否则界面会出现“前半截流式 + 后半截整段”的重复文字。
 */
esp_err_t app_llm_chat_stream(const char *user_text, app_llm_delta_cb_t cb, void *cb_ctx,
                              char *reply, size_t reply_size);

/**
 * @brief  发起一轮对话（一次性拿到整段结果，内部走 app_llm_chat_stream()）
 * @note   界面想省事时用这个；用不到打字机效果的调用方也用它。
 */
esp_err_t app_llm_chat(const char *user_text, char *reply, size_t reply_size);

#ifdef __cplusplus
}
#endif

#endif /* __APP_LLM_H */
