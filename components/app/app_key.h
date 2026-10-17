/**
 * @file    app_key.h
 * @brief   录音按键（GPIO0）：20ms 消抖状态机 + 按键驱动的业务状态机任务
 *
 * 分层：app 层，按键电平通过 bsp 的 bsp_gpio 接口读取，不直接操作寄存器。
 *
 * 两部分组成：
 *   1. **按键扫描状态机**（app_key_init 里的 esp_timer 20ms 周期回调）
 *      IDLE -> DEBOUNCE_PRESS -> PRESSED -> DEBOUNCE_RELEASE -> RELEASED
 *      消抖后把 按下 / 松开 / 超时 三种事件投递到 key_event_queue；
 *      按下过程中计时，超过 APP_KEY_MAX_RECORD_MS（15 秒）产生 TIMEOUT 事件，
 *      防止用户忘松按键导致一直录音。
 *   2. **app_key_task**（Core 1，优先级 4）
 *      消费按键事件，驱动业务状态机：
 *        PRESS   -> 通知音频任务开始录音，状态 = RECORDING
 *                   （**WiFi 未连接时不录音**：只投递 CHAT_MSG_WIFI_ALERT，
 *                    界面收到后弹出「WiFi未连接」提示，直到连上才恢复）
 *        RELEASE -> 通知音频任务停止录音并上交 PCM，状态 = UPLOADING
 *                   （松手时 WiFi 已断开 -> 丢弃本次录音，不尝试上传）
 *        TIMEOUT -> 同 RELEASE（但不丢弃数据）
 *      同时消费 ASR / LLM 结果队列与界面回报事件：
 *        ASR 失败 -> 报错回 IDLE；成功 -> 状态 = LLM_REQUEST
 *        LLM 正文由网络任务直接以流式增量投给界面（本任务不重复显示），
 *        本任务只做收尾：界面还在逐字显示就先停在 AI_TYPING 等它打完
 *        （APP_UI_EVENT_TYPING_DONE 或超时兜底），再回 IDLE。
 */

#ifndef __APP_KEY_H
#define __APP_KEY_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化按键：配置 GPIO + 启动 20ms 周期扫描定时器（esp_timer）
 * @return ESP_OK 成功；其它为 GPIO 或 esp_timer 的错误码
 */
esp_err_t app_key_init(void);

/**
 * @brief  停止扫描并反初始化
 */
esp_err_t app_key_deinit(void);

/**
 * @brief  消抖后的按键状态（true = 已经稳定按下）
 */
bool app_key_is_pressed(void);

/**
 * @brief  按键业务状态机任务（Core 1，优先级 4）
 *
 * @param  arg 未使用
 * @note   由 app_start() 用 xTaskCreatePinnedToCore() 创建，不要重复创建。
 */
void app_key_task(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* __APP_KEY_H */
