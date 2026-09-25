/**
 * @file    app_chat_history.h
 * @brief   对话历史管理：内存里保存最近 10 轮（20 条）消息，开机清空
 *
 * 分层：app 层。只有网络任务（app_llm）会写入，界面只读条数，内部用互斥锁保护。
 *
 * 内存策略：
 *   - 每轮对话的文本都很小（<= APP_CHAT_HISTORY_MSG_MAX 字节），
 *     用 pvPortMalloc 从内部 RAM 分配，不用 PSRAM；
 *   - 超过 10 轮时，从最老的一轮（user + assistant 两条）开始丢弃；
 *   - 组请求体时还有一层**字节预算**（app_chat_history_to_json 的 budget_bytes）：
 *     历史 + 系统提示 + 工具定义要能塞进一次 HTTP 请求，
 *     内部 RAM 紧张时请求体过大会直接把连接打挂，所以宁可少带几轮。
 */

#ifndef __APP_CHAT_HISTORY_H
#define __APP_CHAT_HISTORY_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 一条历史消息 */
typedef struct {
    char  role[16];     /*!< "user" / "assistant" */
    char *content;      /*!< 文本内容（堆内存，由本模块管理） */
} app_chat_msg_t;

/* ============================ 生命周期 ============================ */

/**
 * @brief  初始化（清空）对话历史，并创建互斥锁
 * @note   每次开机调用一次，历史不落盘
 */
esp_err_t app_chat_history_init(void);

/**
 * @brief  清空历史（释放所有文本内存）
 */
void app_chat_history_clear(void);

/**
 * @brief  释放历史模块（等价于 clear，仅日志更明确）
 */
esp_err_t app_chat_history_deinit(void);

/* ============================ 读写 ============================ */

/**
 * @brief  追加一条消息
 *
 * @param  role    "user" / "assistant" / "system"
 * @param  content 文本（UTF-8），超长会被截断到 APP_CHAT_HISTORY_MSG_MAX
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数错；ESP_ERR_NO_MEM 内存不足
 *
 * @note   超过 10 轮时自动丢弃最老的一轮，保证历史里 user/assistant 成对。
 * @note   截断**按完整 UTF-8 字符**收尾：砍出半个汉字会让下一次请求
 *         的 JSON 里带上非法字节，服务端直接回 400。
 */
esp_err_t app_chat_history_add(const char *role, const char *content);

/** 当前保存的消息条数（不含 system） */
int app_chat_history_count(void);

/** 按索引取消息（0 是最老的）；越界返回 NULL */
const app_chat_msg_t *app_chat_history_get(int index);

/**
 * @brief  把历史消息追加到 cJSON 数组（app_llm 组请求体时用）
 *
 * @param  messages     cJSON_CreateArray() 出来的数组
 * @param  budget_bytes 历史部分的字节预算（<=0 表示不限）：
 *                      从**最新**往**最老**回溯累加，超出预算的更老消息直接不带
 * @return ESP_OK 成功；参数非法返回 ESP_ERR_INVALID_ARG
 *
 * @note   预算按“内容字节数 + 少量 JSON 包装开销”估算，不必十分精确，
 *         目的是给请求体总量兜个底。
 * @note   若最新的一条本身就超预算，仍然会带上它（至少保证本轮问题在请求里）。
 */
esp_err_t app_chat_history_to_json(cJSON *messages, size_t budget_bytes);

#ifdef __cplusplus
}
#endif

#endif /* __APP_CHAT_HISTORY_H */
