/**
 * @file    app_asr.h
 * @brief   语音识别（百度智能云 · 短语音识别标准版 REST API）
 *
 * 分层：app 层，用 ESP-IDF 的 esp_http_client + mbedtls(base64) + cJSON。
 *
 * 调用流程：
 *   1. 开机时（WiFi 连上后）app_asr_prefetch_token() 先把 access_token 取回来缓存；
 *   2. app_asr_recognize() 内部再检查 token 缓存，没有/快过期就当场取
 *      （POST https://aip.baidubce.com/oauth/2.0/token）；
 *   2. 再 POST 到 https://vop.baidu.com/server_api，
 *      Header: Content-Type: audio/pcm; rate=16000，
 *      Body:   {"format":"pcm","rate":16000,"channel":1,"cuid":"...",
 *               "token":"...","speech":"<base64 PCM>","len":<PCM字节数>}
 *      PCM 数据边编码边发（分块 base64），不在 RAM 里再存一份 640KB 的 body；
 *   3. 解析返回 JSON：err_no == 0 时 result[0] 就是识别文本。
 *
 * 线程：只有网络任务（app_net_task）调用，内部有 token 缓存，不加锁。
 */

#ifndef __APP_ASR_H
#define __APP_ASR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化 ASR 模块（清空 token 缓存）
 * @return ESP_OK
 */
esp_err_t app_asr_init(void);

/**
 * @brief  识别一段 16kHz / 16bit / 单声道 PCM
 *
 * @param  pcm       PCM 数据（16bit 小端单声道）
 * @param  bytes     PCM 字节数
 * @param  out       输出缓冲区：成功放识别文本，失败放中文错误说明（供界面显示）
 * @param  out_size  输出缓冲区大小
 * @return ESP_OK 识别成功；
 *         ESP_ERR_INVALID_ARG 参数非法；
 *         ESP_ERR_INVALID_SIZE 录音太短；
 *         ESP_ERR_INVALID_STATE 没配密钥 / 链路断开且重连超时；
 *         ESP_ERR_TIMEOUT 网络超时；
 *         ESP_FAIL 鉴权失败 / 服务端返回错误（详见 out 里的文字）
 *
 * @note   内部自带两级重试：取 token 超时重试一次；识别没拿到服务端响应时，
 *         链路正常就等 1.5 秒换新连接重试，链路断了则最多等
 *         APP_ASR_WAIT_LINK_MS 等 WiFi 重连成功后再重试。
 */
esp_err_t app_asr_recognize(const int16_t *pcm, size_t bytes, char *out, size_t out_size);

/**
 * @brief  开机预热：提前把百度 access_token 取回来
 *
 * @return ESP_OK 已有可用 token（含本来就有缓存的情况）；其它为失败原因
 *
 * @note   百度 token 有效期 30 天，取一次要 1~2 秒。放在开机（WiFi 连上之后）
 *         就等于把这段时间从“用户第一次说话”的关键路径上挪走。
 *         失败不影响后续识别（识别时会自己再取一次）。
 */
esp_err_t app_asr_prefetch_token(void);

/**
 * @brief  丢弃 token 缓存，下次识别会重新获取
 */
void app_asr_reset_token(void);

/**
 * @brief  当前是否已经缓存了可用的 token
 */
bool app_asr_has_token(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_ASR_H */
