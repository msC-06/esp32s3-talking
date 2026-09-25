/**
 * @file    app_llm.c
 * @brief   DeepSeek 对话实现：SSE 流式输出（stream=true）+ web_search 工具调用
 *
 * 一轮完整对话（最多两次 HTTP 请求，两次都可以是流式）：
 *   第 1 次请求：system + 历史 + user + tools(web_search)，stream=true
 *     ├─ 模型直接回答        -> 每个 SSE 增量立刻通过回调交给界面（打字机效果的来源）
 *     └─ 模型返回 tool_calls -> 逐个执行（web_search -> app_web_search 联网搜索），
 *                               再发第 2 次请求带上
 *                               assistant(tool_calls) + tool(tool_call_id, 摘要)
 *                               （第 2 次同样流式，用户看到的答案就是这段流）
 *
 * 降级策略（保证一定能出结果）：
 *   如果流式一路下来**一个正文增量都没拿到**（SSE 不可用/中间设备吞了流/解析失败），
 *   自动改用 stream=false 再请求一次；此时整段文本会作为**一段**增量回调出去，
 *   界面照样用打字机动画逐字显示（也就是需求里的“伪打字机”）。
 *   注意：只在“还没往界面送过任何字”时才降级，避免半截流 + 整段拼接造成重复。
 *
 * 对话历史只存 user / assistant 两条：
 *   工具轮次（assistant.tool_calls + tool）**不落历史**。原因是历史满 10 轮会丢弃最老的一对，
 *   一旦把 tool_calls 和对应的 tool 消息拆散，DeepSeek 会因为“tool 消息没有对应的
 *   tool_calls”直接返回 400，之后每轮都失败。工具结果的价值已经体现在最终答复里了。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_llm.h"
#include "app_config.h"
#include "app_chat_history.h"
#include "app_http.h"
#include "app_time.h"               /* 把当前时间写进 system 提示词 */
#include "app_web_search.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"               /* EXT_RAM_BSS_ATTR：工具结果放 PSRAM */
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"

#include "cJSON.h"

static const char *TAG = "APP_LLM";

/* ============================ 工具定义 ============================ */

/** 联网搜索工具名（模型靠这个名字回调；描述见 app_config.h 的 APP_LLM_TOOL_DESC） */
#define LLM_TOOL_WEB_SEARCH     "web_search"
/** 一次最多执行几个 tool_calls */
#define LLM_TOOL_MAX            APP_LLM_MAX_TOOL_CALLS

/**
 * 一次工具执行的结果
 * @note  放静态区（不占网络任务那 8KB 栈）：只有 app_net_task 会调用本模块；
 *        同时用 EXT_RAM_BSS_ATTR 放到 PSRAM，不占内部 RAM
 *        （需开 CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY，没开就退回内部 RAM）。
 */
typedef struct {
    char id[64];                                /*!< tool_call_id（回传时必须原样带回） */
    char name[48];                              /*!< 工具名 */
    char content[APP_WEB_SEARCH_TEXT_MAX + 1];  /*!< 回给模型的文本 */
} llm_tool_result_t;

EXT_RAM_BSS_ATTR static llm_tool_result_t s_tool_results[LLM_TOOL_MAX];

/**
 * system 提示词缓冲（固定人设 + 当前时间）
 * @note  静态 + PSRAM：它在每次组请求体时都会被重写，
 *        512 字节的局部数组压在网络任务 8KB 栈上不划算。
 */
EXT_RAM_BSS_ATTR static char s_sys_prompt[APP_LLM_SYSTEM_PROMPT_MAX];

/* ============================ 流式收包上下文 ============================ */

/**
 * 流式/整段收包的统一上下文
 *
 * @note  流式和整段两种模式**最终都填进这个结构**：
 *        流式是 SSE 一行一行 push 进来，整段是把 message.content 一次性 push 进来，
 *        于是界面侧只需要处理“增量”这一种情况（打字机自然就工作了）。
 *        静态变量：只有网络任务用，且串行执行，安全又省栈。
 */
typedef struct {
    char              *reply;           /*!< 累积全文（调用者缓冲） */
    size_t             reply_size;      /*!< 缓冲大小 */
    size_t             reply_len;       /*!< 已累积字节数 */
    app_llm_delta_cb_t cb;              /*!< 增量回调（可为 NULL） */
    void              *cb_ctx;          /*!< 回调上下文 */
    int                deltas;          /*!< 本轮推送了几段正文 */
    bool               full;            /*!< 缓冲已满，后续增量丢弃 */
    bool               done;            /*!< 收到 [DONE] 或 finish_reason */
    /* ---- 流式 tool_calls 分片累积 ---- */
    int                tool_count;      /*!< 累积到的工具调用个数 */
    struct {
        char   id[64];
        char   name[48];
        char   args[APP_LLM_TOOL_ARGS_MAX];
        size_t args_len;
    } tools[LLM_TOOL_MAX];
} llm_stream_t;

static llm_stream_t s_stream;

/* ============================ 小工具 ============================ */

/**
 * @brief  去掉字符串首尾的空白字符（原地修改）
 */
static char *llm_trim(char *s)
{
    if (s == NULL) {
        return NULL;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    size_t len = strlen(s);
    while (len > 0) {
        char c = s[len - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            s[--len] = '\0';
        } else {
            break;
        }
    }
    return s;
}

/* ============================ 收包：push 正文 / 工具 ============================ */

/**
 * @brief  把一段新增正文累积进 reply，并立刻回调给界面
 *
 * @note   缓冲放不下时**整段丢弃**（而不是截一半），这样累积的文本永远是完整 UTF-8。
 */
static void llm_stream_push_text(llm_stream_t *st, const char *text)
{
    if (st == NULL || text == NULL || text[0] == '\0') {
        return;
    }
    if (st->full || st->reply == NULL) {
        return;
    }

    size_t len = strlen(text);
    if (st->reply_len + len + 1 > st->reply_size) {
        st->full = true;
        ESP_LOGW(TAG, "回复超过 %u 字节，后续内容丢弃", (unsigned)st->reply_size);
        return;
    }

    memcpy(st->reply + st->reply_len, text, len + 1);
    st->reply_len += len;
    st->deltas++;

    if (st->cb != NULL) {
        st->cb(text, st->cb_ctx);       /* 立刻交给界面：这就是打字机效果的来源 */
    }
}

/**
 * @brief  累积流式 tool_calls 分片（同名 id/name 会覆盖，arguments 会拼接）
 */
static void llm_stream_push_tools(llm_stream_t *st, const cJSON *tool_calls)
{
    const cJSON *frag = NULL;

    cJSON_ArrayForEach(frag, tool_calls) {
        const cJSON *idx  = cJSON_GetObjectItem(frag, "index");
        const cJSON *id   = cJSON_GetObjectItem(frag, "id");
        const cJSON *fn   = cJSON_GetObjectItem(frag, "function");
        const cJSON *name = (fn != NULL) ? cJSON_GetObjectItem(fn, "name") : NULL;
        const cJSON *args = (fn != NULL) ? cJSON_GetObjectItem(fn, "arguments") : NULL;

        int i = cJSON_IsNumber(idx) ? (int)idx->valuedouble : 0;
        if (i < 0 || i >= LLM_TOOL_MAX) {
            continue;
        }
        if (i + 1 > st->tool_count) {
            st->tool_count = i + 1;
        }

        if (cJSON_IsString(id) && id->valuestring != NULL) {
            strncpy(st->tools[i].id, id->valuestring, sizeof(st->tools[i].id) - 1);
            st->tools[i].id[sizeof(st->tools[i].id) - 1] = '\0';
        }
        if (cJSON_IsString(name) && name->valuestring != NULL) {
            strncpy(st->tools[i].name, name->valuestring, sizeof(st->tools[i].name) - 1);
            st->tools[i].name[sizeof(st->tools[i].name) - 1] = '\0';
        }
        if (cJSON_IsString(args) && args->valuestring != NULL) {
            size_t alen = strlen(args->valuestring);
            size_t room = sizeof(st->tools[i].args) - 1 - st->tools[i].args_len;
            if (alen > room) {
                alen = room;
            }
            memcpy(st->tools[i].args + st->tools[i].args_len, args->valuestring, alen);
            st->tools[i].args_len += alen;
            st->tools[i].args[st->tools[i].args_len] = '\0';
        }
    }
}

/**
 * @brief  开始新的一轮收包：保留已累积正文与回调，清掉工具/完成标记
 */
static void llm_stream_restart(llm_stream_t *st)
{
    char              *reply      = st->reply;
    size_t             reply_size = st->reply_size;
    size_t             reply_len  = st->reply_len;
    app_llm_delta_cb_t cb         = st->cb;
    void              *cb_ctx     = st->cb_ctx;
    bool               full       = st->full;

    memset(st, 0, sizeof(*st));

    st->reply      = reply;
    st->reply_size = reply_size;
    st->reply_len  = reply_len;
    st->cb         = cb;
    st->cb_ctx     = cb_ctx;
    st->full       = full;
}

/* ============================ SSE 解析 ============================ */

/**
 * @brief  SSE 单行处理：只关心 "data:" 行，取 choices[0].delta
 *
 * @note   DeepSeek 的流式格式：
 *             data: {"choices":[{"delta":{"content":"你"},"index":0}]}
 *             data: {"choices":[{"delta":{"tool_calls":[{...}]}}]}
 *             data: [DONE]
 */
static void llm_sse_line(const char *line, void *ctx)
{
    llm_stream_t *st = (llm_stream_t *)ctx;

    if (st == NULL || line == NULL || line[0] == '\0') {
        return;
    }
    if (line[0] == ':') {
        return;                                     /* SSE 注释/心跳 */
    }
    if (strncmp(line, "data:", 5) != 0) {
        return;                                     /* 忽略 event:/id:/retry: */
    }

    const char *payload = line + 5;
    while (*payload == ' ') {
        payload++;
    }
    if (*payload == '\0') {
        return;
    }
    if (strcmp(payload, "[DONE]") == 0) {
        st->done = true;
        return;
    }

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        ESP_LOGW(TAG, "SSE 数据行不是合法 JSON（%.60s）", payload);
        return;
    }

    const cJSON *choices = cJSON_GetObjectItem(root, "choices");
    const cJSON *c0      = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    if (c0 != NULL) {
        const cJSON *delta = cJSON_GetObjectItem(c0, "delta");
        if (delta != NULL) {
            const cJSON *content = cJSON_GetObjectItem(delta, "content");
            if (cJSON_IsString(content) && content->valuestring != NULL) {
                llm_stream_push_text(st, content->valuestring);
            }
            const cJSON *tc = cJSON_GetObjectItem(delta, "tool_calls");
            if (cJSON_IsArray(tc)) {
                llm_stream_push_tools(st, tc);
            }
        }
        const cJSON *fr = cJSON_GetObjectItem(c0, "finish_reason");
        if (cJSON_IsString(fr) && fr->valuestring != NULL) {
            st->done = true;
        }
    }
    cJSON_Delete(root);
}

/* ============================ 请求体组装 ============================ */

/**
 * @brief  新建请求体根对象和 messages 数组
 */
static cJSON *llm_new_body_root(cJSON **messages_out)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    if (messages == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    *messages_out = messages;
    return root;
}

/**
 * @brief  组出 system 提示词：固定人设 + 当前时间
 *
 * @note   时间必须**每次都重新取**（对时可能在开机几秒后才成功），
 *         而且大模型自己不知道“现在”，不告诉它就会瞎编日期
 *         （现象：问“今天是几号”，它答一个完全不搭的日期）。
 * @note   时间没同步时 app_time_llm_text() 会让提示词里写明“不知道当前时间”，
 *         这样模型至少不会把 1970 当答案。
 */
static const char *llm_system_prompt(void)
{
    size_t used = (size_t)snprintf(s_sys_prompt, sizeof(s_sys_prompt), "%s", APP_LLM_SYSTEM_PROMPT);

#if APP_TIME_INJECT_TO_LLM
    if (used < sizeof(s_sys_prompt) - 1) {
        char time_text[192] = {0};
        app_time_llm_text(time_text, sizeof(time_text));
        snprintf(s_sys_prompt + used, sizeof(s_sys_prompt) - used, "%s", time_text);
    }
#endif
    return s_sys_prompt;
}

/**
 * @brief  往 messages 里放“上下文”：system 提示词 + 历史对话 + 本次提问
 */
static void llm_add_context_messages(cJSON *messages, const char *user_text)
{
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", llm_system_prompt());
    cJSON_AddItemToArray(messages, sys);

    /* 历史对话（最多 10 轮，只含 user/assistant）：
     * 传预算进去，避免请求体把内部 RAM 吃光（见 APP_LLM_HISTORY_BUDGET 注释） */
    app_chat_history_to_json(messages, APP_LLM_HISTORY_BUDGET);

    cJSON *user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "role", "user");
    cJSON_AddStringToObject(user, "content", user_text);
    cJSON_AddItemToArray(messages, user);
}

/**
 * @brief  往请求体里加 tools 定义（只注册一个 web_search 函数）
 */
static void llm_add_tools(cJSON *root)
{
    cJSON *tools = cJSON_AddArrayToObject(root, "tools");
    if (tools == NULL) {
        return;
    }

    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");

    cJSON *fn = cJSON_AddObjectToObject(tool, "function");
    cJSON_AddStringToObject(fn, "name", LLM_TOOL_WEB_SEARCH);
    cJSON_AddStringToObject(fn, "description", APP_LLM_TOOL_DESC);

    cJSON *params = cJSON_AddObjectToObject(fn, "parameters");
    cJSON_AddStringToObject(params, "type", "object");

    cJSON *props = cJSON_AddObjectToObject(params, "properties");
    cJSON *query = cJSON_AddObjectToObject(props, "query");
    cJSON_AddStringToObject(query, "type", "string");
    cJSON_AddStringToObject(query, "description", "搜索关键词，尽量简短具体");

    cJSON *required = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("query"));

    cJSON_AddItemToArray(tools, tool);
}

/**
 * @brief  第 1 轮请求体：system + 历史 + user + tools
 *
 * @param  stream true = stream:true（SSE 流式）
 * @return 堆内存里的 JSON 字符串（调用者 free）；失败返回 NULL
 */
static char *llm_build_body(const char *user_text, bool stream)
{
    cJSON *messages = NULL;
    cJSON *root = llm_new_body_root(&messages);
    if (root == NULL) {
        return NULL;
    }

    llm_add_context_messages(messages, user_text);
    llm_add_tools(root);                    /* 第 1 轮把工具定义带上 */

    cJSON_AddStringToObject(root, "model", DEEPSEEK_MODEL);
    cJSON_AddBoolToObject(root, "stream", stream ? 1 : 0);
    cJSON_AddNumberToObject(root, "max_tokens", APP_LLM_MAX_TOKENS);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

/**
 * @brief  第 2 轮请求体：system + 历史 + user + assistant(tool_calls) + tool(...)
 *
 * @param  st 流式收包上下文（里面的 tools[] 是已经拼好的工具调用）
 * @return 堆内存里的 JSON 字符串（调用者 free）；失败返回 NULL
 *
 * @note   这里**不再带 tools**：已经搜过一次了，让模型基于结果直接作答，
 *         否则模型可能一轮一轮地搜下去。
 */
static char *llm_build_followup(const char *user_text, const llm_stream_t *st,
                                const llm_tool_result_t *results, int count, bool stream)
{
    cJSON *messages = NULL;
    cJSON *root = llm_new_body_root(&messages);
    if (root == NULL) {
        return NULL;
    }

    llm_add_context_messages(messages, user_text);

    /* 1. assistant 消息：content 为空，但必须把 tool_calls 原样带回去 */
    cJSON *assistant = cJSON_CreateObject();
    cJSON_AddStringToObject(assistant, "role", "assistant");
    cJSON_AddStringToObject(assistant, "content", "");

    cJSON *calls = cJSON_AddArrayToObject(assistant, "tool_calls");
    for (int i = 0; i < count && calls != NULL; i++) {
        cJSON *call = cJSON_CreateObject();
        cJSON_AddStringToObject(call, "id", st->tools[i].id);
        cJSON_AddStringToObject(call, "type", "function");
        cJSON *fn = cJSON_AddObjectToObject(call, "function");
        cJSON_AddStringToObject(fn, "name", st->tools[i].name);
        cJSON_AddStringToObject(fn, "arguments", st->tools[i].args);
        cJSON_AddItemToArray(calls, call);
    }
    cJSON_AddItemToArray(messages, assistant);

    /* 2. 每个 tool_call 对应一条 tool 消息（tool_call_id 必须匹配，否则接口报 400） */
    for (int i = 0; i < count; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "role", "tool");
        cJSON_AddStringToObject(tool, "tool_call_id", results[i].id);
        cJSON_AddStringToObject(tool, "content", results[i].content);
        cJSON_AddItemToArray(messages, tool);
    }

    cJSON_AddStringToObject(root, "model", DEEPSEEK_MODEL);
    cJSON_AddBoolToObject(root, "stream", stream ? 1 : 0);
    cJSON_AddNumberToObject(root, "max_tokens", APP_LLM_MAX_TOKENS);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

/* ============================ 响应辅助 ============================ */

/**
 * @brief  取 choices[0].message
 */
static const cJSON *llm_get_message(const cJSON *root)
{
    const cJSON *choices = cJSON_GetObjectItem(root, "choices");
    const cJSON *first   = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    return (first != NULL) ? cJSON_GetObjectItem(first, "message") : NULL;
}

/**
 * @brief  取出服务端返回的 error.message（例如额度不足、Key 无效）
 */
static void llm_extract_error(const cJSON *root, char *out, size_t out_size)
{
    const cJSON *error = cJSON_GetObjectItem(root, "error");
    const cJSON *msg   = cJSON_IsObject(error) ? cJSON_GetObjectItem(error, "message") : NULL;
    snprintf(out, out_size, "AI服务错误: %.200s",
             (msg != NULL && cJSON_IsString(msg)) ? msg->valuestring : "未知错误");
    ESP_LOGE(TAG, "服务端返回错误：%s", cJSON_IsString(msg) ? msg->valuestring : "未知");
}

/**
 * @brief  把**非流式**响应解析进流式上下文（正文 + 工具调用）
 *
 * @note   走同一套 push 接口，所以降级路径下界面收到的是一段完整文本，
 *         打字机照样能逐字显示（需求里的“伪打字机动画”）。
 */
static esp_err_t llm_collect_nonstream(const app_http_resp_t *resp, llm_stream_t *st)
{
    cJSON *root = app_http_parse_json(resp);
    if (root == NULL) {
        app_http_dump(TAG, resp);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *message = llm_get_message(root);
    if (message == NULL) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const cJSON *content = cJSON_GetObjectItem(message, "content");
    if (cJSON_IsString(content) && content->valuestring != NULL) {
        llm_stream_push_text(st, content->valuestring);
    }

    const cJSON *tc = cJSON_GetObjectItem(message, "tool_calls");
    if (cJSON_IsArray(tc)) {
        llm_stream_push_tools(st, tc);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/* ============================ 发请求 ============================ */

/**
 * @brief  发一次 chat/completions 请求
 *
 * @param  body   请求体
 * @param  resp   响应上下文（每次调用前清空；流式模式设置会被保留）
 * @param  status 输出 HTTP 状态码（-1 = 没拿到响应）
 * @return ESP_OK 拿到了 HTTP 响应（状态码看 status）；其它为网络/超时等传输层错误
 */
static esp_err_t llm_request(const char *body, app_http_resp_t *resp, int *status)
{
    int body_len = (int)strlen(body);

    /* 清空复用，但保留“流式逐行”设置 */
    app_http_line_cb_t line_cb  = resp->line_cb;
    void              *line_ctx = resp->line_ctx;
    app_http_resp_init(resp, resp->buf, resp->cap);
    if (line_cb != NULL) {
        app_http_set_line_cb(resp, line_cb, line_ctx);
    }

    char auth[256];
    snprintf(auth, sizeof(auth), "Bearer %s", DEEPSEEK_API_KEY);

    esp_http_client_config_t cfg = {
        .url            = DEEPSEEK_API_URL,
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = APP_LLM_TIMEOUT_MS,
        .buffer_size    = 4096,
        .buffer_size_tx = 2048,
        .event_handler  = app_http_event_handler,       /* 逐段回调（流式时按行切分） */
        .user_data      = resp,
#if APP_TLS_USE_CA_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#else
        .skip_cert_common_name_check = true,
#endif
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "HTTP 客户端创建失败");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", (line_cb != NULL) ? "text/event-stream" : "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, body_len);

    int64_t   t_begin = esp_timer_get_time();
    esp_err_t err     = esp_http_client_perform(client);
    int       code    = esp_http_client_get_status_code(client);
    int       elapsed = (int)((esp_timer_get_time() - t_begin) / 1000);

    if (status != NULL) {
        *status = (err == ESP_OK) ? code : -1;
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "响应：HTTP %d，耗时 %d ms%s", code, elapsed,
                 (line_cb != NULL) ? "（流式）" : "");
    } else {
        ESP_LOGE(TAG, "请求失败：%s（耗时 %d ms）", esp_err_to_name(err), elapsed);
    }

    esp_http_client_cleanup(client);
    return (err == ESP_OK) ? ESP_OK : err;
}

/* ============================ 工具执行 ============================ */

/**
 * @brief  从 arguments JSON 里解析出 query
 *
 * @note   arguments 是一个 JSON **字符串**（形如 "{\"query\":\"今天天气\"}"）。
 */
static esp_err_t llm_extract_query(const char *args_json, char *out, size_t out_size)
{
    if (args_json == NULL || args_json[0] == '\0') {
        return ESP_FAIL;
    }

    cJSON *a = cJSON_Parse(args_json);
    if (a == NULL) {
        ESP_LOGW(TAG, "工具参数不是合法 JSON：%.80s", args_json);
        return ESP_FAIL;
    }

    const cJSON *q = cJSON_GetObjectItem(a, "query");
    esp_err_t ret = ESP_FAIL;
    if (cJSON_IsString(q) && q->valuestring != NULL && q->valuestring[0] != '\0') {
        strncpy(out, q->valuestring, out_size - 1);
        out[out_size - 1] = '\0';
        ret = ESP_OK;
    }
    cJSON_Delete(a);
    return ret;
}

/**
 * @brief  执行累积到的 tool_calls（目前只实现了 web_search）
 *
 * @param  st 流式上下文
 * @return 实际执行（处理）的条数
 */
static int llm_execute_tools(const llm_stream_t *st)
{
    int n = st->tool_count;
    if (n > LLM_TOOL_MAX) {
        n = LLM_TOOL_MAX;
    }

    for (int i = 0; i < n; i++) {
        llm_tool_result_t *r = &s_tool_results[i];
        memset(r, 0, sizeof(*r));

        strncpy(r->id, st->tools[i].id, sizeof(r->id) - 1);
        strncpy(r->name, st->tools[i].name, sizeof(r->name) - 1);

        /* 不认识的工具：如实告诉模型，让它自己回答 */
        if (strcmp(r->name, LLM_TOOL_WEB_SEARCH) != 0) {
            ESP_LOGW(TAG, "模型请求了未实现的工具：%s", r->name);
            /* 注意：这里用 st->tools[i].name（在 s_stream 里）而不是 r->name，
             * 否则 snprintf 的目标和参数都落在 s_tool_results 里，会被 -Wrestrict 拦下 */
            snprintf(r->content, sizeof(r->content), "工具 %s 不可用，请直接回答。",
                     (st->tools[i].name[0] != '\0') ? st->tools[i].name : "?");
            continue;
        }

        char query[512] = {0};
        if (llm_extract_query(st->tools[i].args, query, sizeof(query)) != ESP_OK) {
            snprintf(r->content, sizeof(r->content), "联网搜索失败：没有解析到搜索关键词。");
            continue;
        }

        ESP_LOGI(TAG, "调用工具 %s：query=\"%s\"", r->name, query);
        if (app_web_search(query, r->content, sizeof(r->content)) != ESP_OK) {
            ESP_LOGW(TAG, "联网搜索失败");
            snprintf(r->content, sizeof(r->content), "联网搜索失败：网络或服务异常。");
        } else {
            ESP_LOGI(TAG, "搜索结果 %u 字符", (unsigned)strlen(r->content));
        }
    }
    return n;
}

/* ============================ 一次完整对话 ============================ */

/**
 * @brief  跑完一轮对话（内部最多两次请求），把正文交给回调与 reply
 *
 * @param  stream     true = 用 SSE 流式
 * @param  got_delta  输出：true = 已经往界面（回调）送过正文
 */
static esp_err_t llm_run(const char *user_text, bool stream,
                         app_llm_delta_cb_t cb, void *cb_ctx,
                         char *reply, size_t reply_size, bool *got_delta)
{
    esp_err_t       err      = ESP_FAIL;
    char           *body     = NULL;
    char           *resp_buf = NULL;
    app_http_resp_t resp;
    int             status   = 0;

    reply[0] = '\0';

    /* 1. 请求体 */
    body = llm_build_body(user_text, stream);
    if (body == NULL) {
        snprintf(reply, reply_size, "内存不足");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "第1轮请求：%u 字节（%s），历史 %d 条",
             (unsigned)strlen(body), stream ? "流式" : "整段", app_chat_history_count());

    /* 2. 接收缓冲放 PSRAM */
    resp_buf = (char *)heap_caps_malloc(APP_LLM_RESP_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (resp_buf == NULL) {
        resp_buf = (char *)malloc(APP_LLM_RESP_BUF_SIZE);       /* 退化到内部 RAM */
    }
    if (resp_buf == NULL) {
        free(body);
        snprintf(reply, reply_size, "内存不足");
        return ESP_ERR_NO_MEM;
    }
    app_http_resp_init(&resp, resp_buf, APP_LLM_RESP_BUF_SIZE);

    /* 3. 收包上下文：流式与整段都填进 s_stream */
    memset(&s_stream, 0, sizeof(s_stream));
    s_stream.reply      = reply;
    s_stream.reply_size = reply_size;
    s_stream.cb         = cb;
    s_stream.cb_ctx     = cb_ctx;
    if (stream) {
        app_http_set_line_cb(&resp, llm_sse_line, &s_stream);
    }

    /* ---------- 4. 第 1 轮 ---------- */
    err = llm_request(body, &resp, &status);
    if (err != ESP_OK) {
        snprintf(reply, reply_size, "AI网络连接失败");
        goto done;
    }
    if (status != 200) {
        cJSON *root = app_http_parse_json(&resp);
        if (root != NULL && cJSON_GetObjectItem(root, "error") != NULL) {
            llm_extract_error(root, reply, reply_size);
        } else {
            snprintf(reply, reply_size, "AI服务异常(HTTP %d)", status);
        }
        if (root != NULL) {
            cJSON_Delete(root);
        }
        err = ESP_FAIL;
        goto done;
    }

    if (stream) {
        if (s_stream.deltas == 0 && s_stream.tool_count == 0) {
            ESP_LOGW(TAG, "流式响应里既没有正文也没有工具调用（done=%d）", (int)s_stream.done);
            snprintf(reply, reply_size, "AI无响应(流式)");
            err = ESP_FAIL;             /* 交给上层降级重试 */
            goto done;
        }
        ESP_LOGI(TAG, "流式第1轮结束：正文 %u 字节 / %d 段，工具 %d 个，done=%d",
                 (unsigned)s_stream.reply_len, s_stream.deltas,
                 s_stream.tool_count, (int)s_stream.done);
    } else {
        err = llm_collect_nonstream(&resp, &s_stream);
        if (err != ESP_OK) {
            if (reply[0] == '\0') {
                snprintf(reply, reply_size, "AI回复解析失败");
            }
            goto done;
        }
        ESP_LOGI(TAG, "整段第1轮结束：正文 %u 字节，工具 %d 个",
                 (unsigned)s_stream.reply_len, s_stream.tool_count);
    }

    if (s_stream.deltas > 0 && got_delta != NULL) {
        *got_delta = true;
    }

    /* ---------- 5. 没有工具调用：正文就是最终答复 ---------- */
    if (s_stream.tool_count == 0) {
        if (s_stream.deltas == 0) {
            snprintf(reply, reply_size, "AI没有返回内容");
            err = ESP_FAIL;
        } else {
            char *clean = llm_trim(reply);
            if (clean != reply) {
                memmove(reply, clean, strlen(clean) + 1);
            }
            err = ESP_OK;
        }
        goto done;
    }

    /* ---------- 6. 有工具调用：执行搜索 + 追问（同样按需流式） ---------- */
    ESP_LOGI(TAG, "模型要求调用 %d 个工具", s_stream.tool_count);
    int tool_count = llm_execute_tools(&s_stream);
    if (tool_count <= 0) {
        snprintf(reply, reply_size, "AI工具调用异常");
        err = ESP_FAIL;
        goto done;
    }

    char *body2 = llm_build_followup(user_text, &s_stream, s_tool_results, tool_count, stream);
    if (body2 == NULL) {
        snprintf(reply, reply_size, "内存不足");
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    free(body);
    body = body2;

    /* 工具参数在 body2 里已经用过了，这里只重置收包状态（保留累积正文与回调） */
    llm_stream_restart(&s_stream);

    ESP_LOGI(TAG, "带搜索结果追问：%u 字节（%s）", (unsigned)strlen(body), stream ? "流式" : "整段");
    err = llm_request(body, &resp, &status);
    if (err != ESP_OK || status != 200) {
        /* 把服务端返回的错误正文解析出来打日志：
         * 400 的原因（参数/UTF-8/上下文超限等）只有这里有，别吞掉 */
        cJSON *eroot = app_http_parse_json(&resp);
        if (eroot != NULL && cJSON_GetObjectItem(eroot, "error") != NULL) {
            char detail[256];
            llm_extract_error(eroot, detail, sizeof(detail));
            snprintf(reply, reply_size, "%.200s", detail);
            cJSON_Delete(eroot);
        } else {
            if (eroot != NULL) {
                cJSON_Delete(eroot);
            }
            app_http_dump(TAG, &resp);      /* 连 JSON 都不是就把原文打出来 */
            snprintf(reply, reply_size, "搜索后追问失败(HTTP %d)", status);
        }
        ESP_LOGW(TAG, "追问失败（HTTP %d）：%s", status, reply);
        err = ESP_FAIL;
        goto done;
    }

    if (stream) {
        if (s_stream.deltas == 0) {
            ESP_LOGW(TAG, "追问的流式响应里没有正文");
            snprintf(reply, reply_size, "AI没有返回内容");
            err = ESP_FAIL;
            goto done;
        }
    } else {
        err = llm_collect_nonstream(&resp, &s_stream);
        if (err != ESP_OK || s_stream.deltas == 0) {
            snprintf(reply, reply_size, "AI没有返回内容");
            err = ESP_FAIL;
            goto done;
        }
    }

    if (got_delta != NULL) {
        *got_delta = true;
    }
    {
        char *clean = llm_trim(reply);
        if (clean != reply) {
            memmove(reply, clean, strlen(clean) + 1);
        }
    }
    err = ESP_OK;

done:
    if (body != NULL) {
        free(body);
    }
    if (resp_buf != NULL) {
        free(resp_buf);
    }
    return err;
}

/* ============================ 对外接口 ============================ */

esp_err_t app_llm_init(void)
{
    esp_err_t err = app_chat_history_init();
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "LLM 模块就绪（模型 %s，工具 %s，流式 %s）",
             DEEPSEEK_MODEL, LLM_TOOL_WEB_SEARCH,
             APP_LLM_STREAM_ENABLE ? "开" : "关");
    return ESP_OK;
}

esp_err_t app_llm_chat_stream(const char *user_text, app_llm_delta_cb_t cb, void *cb_ctx,
                              char *reply, size_t reply_size)
{
    if (user_text == NULL || reply == NULL || reply_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    reply[0] = '\0';

    if (strcmp(DEEPSEEK_API_KEY, "YOUR_DEEPSEEK_API_KEY") == 0 || strlen(DEEPSEEK_API_KEY) == 0) {
        ESP_LOGE(TAG, "还没有配置 DeepSeek API Key（填 components/app/app_config_secret.h）");
        snprintf(reply, reply_size, "未配置DeepSeek密钥");
        return ESP_ERR_INVALID_STATE;
    }

    bool      got_delta = false;
    esp_err_t err       = ESP_FAIL;

    /* 1. 先试流式 */
#if APP_LLM_STREAM_ENABLE
    err = llm_run(user_text, true, cb, cb_ctx, reply, reply_size, &got_delta);
#else
    err = ESP_FAIL;
#endif

    /* 2. 流式没拿到东西就降级为整段（只在还没往界面送过字时才降级，
     *    否则会把“半截流 + 整段”拼在一起，出现重复文字） */
    if (err != ESP_OK && !got_delta) {
        ESP_LOGW(TAG, "流式没拿到内容（%s），降级为一次性返回", reply);
        reply[0] = '\0';
        err = llm_run(user_text, false, cb, cb_ctx, reply, reply_size, &got_delta);
    }

    if (err == ESP_OK) {
        /* 成功的这一轮：user + assistant 一起进历史 */
        app_chat_history_add("user", user_text);
        app_chat_history_add("assistant", reply);
        ESP_LOGI(TAG, "AI 回复（%u 字节，%s）：%s", (unsigned)strlen(reply),
                 got_delta ? "流式" : "整段", reply);
    }
    return err;
}

esp_err_t app_llm_chat(const char *user_text, char *reply, size_t reply_size)
{
    return app_llm_chat_stream(user_text, NULL, NULL, reply, reply_size);
}
