/**
 * @file    app_time.h
 * @brief   系统时间（SNTP 对时）：给界面显示时间、给大模型提供“现在几点”
 *
 * 分层：app 层。用 lwip 自带的 SNTP 客户端（esp_sntp.h），
 *       链路状态通过 app_wifi 查询（app 层内部互相调用）。
 *
 * 为什么需要它：
 *   ESP32-S3 没有带电池的 RTC，开机时间是 1970-01-01。
 *   于是问大模型“今天是几号”时，它只能凭空编一个日期（比如“2025年6月13日”）。
 *   有了这个模块，系统提示词里会带上真实时间，模型就能答对；
 *   界面上的待机提示也会顺带显示 HH:MM。
 *
 * 线程模型：
 *   app_time_start() 创建一个低优先级任务（Core 0，优先级 3），
 *   它自己等 WiFi 连上 → 发起 SNTP 对时 → 成功后再等 APP_TIME_RESYNC_INTERVAL_MS
 *   重新对一次；失败则每 APP_TIME_RETRY_MS 重试。
 *   为什么不用 lwip 默认的轮询周期（CONFIG_LWIP_SNTP_UPDATE_DELAY，默认 1 小时）：
 *   开机时 WiFi 还没连上，第一次对时必然失败，那样就要等一小时才有时间。
 *   本模块自己管重试节奏，连上就能拿到时间。
 *
 * 时间有效性判断：app_time_is_synced() 用“当前时间是否晚于 2020-01-01”判断，
 *   也就是 RTC 是否被对时（或其它途径）设过，比看标志位更可靠。
 */

#ifndef __APP_TIME_H
#define __APP_TIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 生命周期 ============================ */

/**
 * @brief  启动时间模块（创建对时任务，立即返回）
 *
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 任务创建失败；ESP_ERR_INVALID_STATE 已启动
 * @note   在 app_start() 里调用即可，不需要等 WiFi：任务内部会等链路。
 *         `APP_TIME_ENABLE=0` 时本函数直接返回 ESP_OK 且什么都不做。
 */
esp_err_t app_time_start(void);

/**
 * @brief  手动同步一次（阻塞）
 *
 * @param  timeout_ms 最长等待时间（毫秒）
 * @return ESP_OK 已同步；ESP_ERR_TIMEOUT 超时；ESP_ERR_INVALID_STATE 没开这个功能
 * @note   只有对时任务会调用它；其它模块想立刻拿到时间就用 app_time_wait_synced()。
 */
esp_err_t app_time_sync_once(uint32_t timeout_ms);

/**
 * @brief  等时间就绪（阻塞，最多 timeout_ms）
 * @return true = 时间可用（已对时或本来就是准的）
 */
bool app_time_wait_synced(uint32_t timeout_ms);

/* ============================ 查询 ============================ */

/**
 * @brief  系统时间是否已经可信
 * @note   两种情况都算可信：SNTP 对时回调触发过，或当前时间晚于 2020-01-01
 *         （后者能识别“对过时”和“别人手动 set 过”两种情况，比只看标志位可靠）。
 */
bool app_time_is_synced(void);

/**
 * @brief  当前本地时间（受 APP_TIME_TZ 影响）
 * @param  out 输出结构体（可为 NULL，只想知道成败）
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数错
 */
esp_err_t app_time_now(struct tm *out);

/* ============================ 格式化（都是 UTF-8） ============================ */

/**
 * @brief  完整时间字符串："2026-09-25 16:30:12 星期四"
 * @param  buf 输出缓冲（建议 >= 32 字节）
 * @param  len 缓冲大小
 * @return 写入的字符数（不含 '\0'）；失败返回 -1
 */
int app_time_now_str(char *buf, size_t len);

/**
 * @brief  只要时分："16:30"（界面状态行用）
 * @return 写入的字符数；失败返回 -1
 */
int app_time_now_clock(char *buf, size_t len);

/**
 * @brief  自定义格式（strftime 包装）
 * @param  fmt strftime 格式串，例如 "%Y/%m/%d"
 * @return 写入的字符数；失败返回 -1
 */
int app_time_format(char *buf, size_t len, const char *fmt);

/**
 * @brief  给大模型用的一句话（拼在 system 提示词后面）
 *
 * @param  buf 输出缓冲（建议 >= 128 字节）
 * @param  len 缓冲大小
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 时间还没同步（此时 buf 里是“时间未同步”的说明）；
 *         ESP_ERR_INVALID_ARG 参数错
 *
 * @note   没同步时**不编时间**，而是明确告诉模型“不知道现在几点”，
 *         这样它至少不会把 1970 当答案（配合 APP_TIME_INJECT_TO_LLM 使用）。
 */
esp_err_t app_time_llm_text(char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* __APP_TIME_H */
