/**
 * @file    app_net.c
 * @brief   网络请求任务实现（ASR + LLM 流式串联）
 */

#include <string.h>

#include "app_net.h"
#include "app_bus.h"
#include "app_config.h"
#include "app_asr.h"
#include "app_llm.h"
#include "app_wifi.h"

#include "esp_log.h"
#include "esp_attr.h"               /* EXT_RAM_BSS_ATTR：大缓冲放 PSRAM */
#include "app_mem.h"                /* 关键路径打点：内部堆余量 */
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "APP_NET";

/** 发请求前最多等 WiFi 多久（毫秒） */
#define NET_WAIT_WIFI_MS    20000

/* ============================ 内部状态 ============================ */

/**
 * 文本缓冲放静态区，不占任务栈：
 * net 任务栈只有 8KB，TLS 握手本身就要好几 KB。
 * EXT_RAM_BSS_ATTR 把它们放到 PSRAM，不占内部 RAM（需开
 * CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY；没开就退回内部 RAM）。
 */
EXT_RAM_BSS_ATTR static char s_asr_text[APP_TEXT_MAX_LEN];
EXT_RAM_BSS_ATTR static char s_llm_reply[APP_TEXT_MAX_LEN];

/* ============================ 内部函数 ============================ */

/**
 * @brief  LLM 流式正文增量回调（在网络任务上下文里同步执行）
 *
 * @note   这里**只做一件事**：把这一小段正文投递给界面队列（打字机自己逐字显示）。
 *         不在回调里碰 LVGL 控件——跨线程操作控件会随机崩，
 *         而且阻塞在这里会拖慢 HTTP 收包。
 */
static void net_llm_delta_cb(const char *text, void *ctx)
{
    (void)ctx;
    if (text == NULL || text[0] == '\0') {
        return;
    }
    ESP_LOGD(TAG, "流式增量 %u 字节", (unsigned)strlen(text));
    app_bus_post_chat(CHAT_MSG_AI_DELTA, text);
}

/* ============================ 对外接口 ============================ */

esp_err_t app_net_init(void)
{
    esp_err_t err = app_asr_init();
    if (err != ESP_OK) {
        return err;
    }
    return app_llm_init();
}

void app_net_task(void *arg)
{
    (void)arg;

    QueueHandle_t q = app_bus_pcm_queue();
    if (q == NULL) {
        ESP_LOGE(TAG, "录音数据队列未初始化，任务退出");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "网络任务启动（Core %d，优先级 %d）", xPortGetCoreID(), uxTaskPriorityGet(NULL));

    /* 0. 开机预热：等 WiFi 上来后先把百度 access_token 取回来。
     *    百度 token 有效期 30 天，一次请求 1~2 秒，
     *    放在这里就等于把这 1~2 秒从“用户第一次说话”的关键路径上挪走了。 */
    if (app_wifi_wait_connected(NET_WAIT_WIFI_MS)) {
        app_asr_prefetch_token();
    } else {
        ESP_LOGW(TAG, "开机等 WiFi 超时，等第一次识别时再取 token");
    }

    for (;;) {
        app_pcm_msg_t msg;

        /* 1. 等一段录音数据（阻塞，不占 CPU） */
        if (xQueueReceive(q, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        ESP_LOGI(TAG, "收到录音：%u 字节 / %u ms", (unsigned)msg.bytes, (unsigned)msg.duration_ms);

        /* 2. WiFi 未连接：**不尝试上传**（需求：未连接时不再尝试上传，
         *    直到 WiFi 连上才恢复）。正常路径下按键侧（app_key）就已经拦住了，
         *    这里兜住“录音途中掉线、松手后才走到这儿”的窗口。
         *    注意：msg.pcm 指向录音器的静态 PSRAM 缓冲（下次录音会被覆盖），
         *    不需要也不能在这里 free。 */
        if (!app_wifi_is_connected()) {
            ESP_LOGW(TAG, "WiFi 未连接，跳过本次上传（%u 字节录音直接丢弃）",
                     (unsigned)msg.bytes);
            app_bus_post_asr(false, "WiFi 未连接，请先连接 WiFi");
            continue;
        }

        /* 3. 云端识别（百度 ASR） */
        app_mem_log_brief("识别前");        /* 打点：这一轮的内部堆余量 */
        s_asr_text[0] = '\0';
        app_mem_peak_begin();
        esp_err_t err = app_asr_recognize((const int16_t *)msg.pcm, msg.bytes,
                                         s_asr_text, sizeof(s_asr_text));
        app_mem_peak_end("语音识别");       /* ASR 是两条连接（token + 上传），顺带看一眼它的价格 */
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ASR 失败: %s（%s）", esp_err_to_name(err), s_asr_text);
            app_bus_post_asr(false, s_asr_text);
            continue;
        }

        /* 4. 拿到识别结果就**先让用户的话上屏**，再去问大模型。
         *    关键：这里直接投递气泡，不等 app_key_task 轮询转发，
         *    也不等 LLM 请求结束——用户会先看到自己说的话，几秒后才看到回答。 */
        app_bus_post_asr(true, s_asr_text);                     /* 给业务状态机（切状态用） */
        app_bus_post_chat(CHAT_MSG_USER, s_asr_text);           /* 立刻上屏：用户气泡 */

        /* 5. 大模型对话（DeepSeek 流式：正文一段一段通过回调推给界面打字机）。
         *    历史/工具调用等都在 app_llm 内部处理，这里只管把结果转成总线消息。 */
        app_mem_log_brief("对话前");
        s_llm_reply[0] = '\0';
        err = app_llm_chat_stream(s_asr_text, net_llm_delta_cb, NULL,
                                  s_llm_reply, sizeof(s_llm_reply));

        /* 顺序很重要：先投“结果”，再投“流结束”。
         *   - app_key_task 收到结果时，界面通常还在逐字显示 → 它会先进 AI_TYPING 等着；
         *   - 界面收到 AI_END 才把剩下的字打完，然后回报 TYPING_DONE → 回待机。
         * 反过来先发 AI_END 的话，界面可能已经把字打完并回报完成，
         * 那时状态机才收到结果，就会把状态重新推成 AI_TYPING 而永远等不到回报。 */
        if (err == ESP_OK) {
            app_bus_post_llm(true, s_llm_reply);
        } else {
            ESP_LOGW(TAG, "LLM 失败: %s（%s）", esp_err_to_name(err), s_llm_reply);
            app_bus_post_llm(false, s_llm_reply);
        }
        app_bus_post_chat(CHAT_MSG_AI_END, NULL);
    }
}
