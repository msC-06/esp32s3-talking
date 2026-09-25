/**
 * @file    app_asr.c
 * @brief   百度短语音识别实现（token 缓存 + 分块 base64 上传 + JSON 解析）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_asr.h"
#include "app_config.h"
#include "app_http.h"
#include "app_mem.h"
#include "app_bus.h"                /* 链路重连时给界面发一条状态提示 */
#include "app_wifi.h"               /* 重试前判断链路状态（App 层内互相调用允许） */

#include "my_drivers/audio/audio_recorder.h"    /* 采样率常量 */

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_attr.h"               /* EXT_RAM_BSS_ATTR：大缓冲放 PSRAM */
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "mbedtls/base64.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "APP_ASR";

/* ============================ 内部状态（全部 static） ============================ */

/** access_token 缓存（有效期 30 天） */
static char    s_token[192];
/** token 到期的时间点（esp_timer_get_time() 基准，微秒；0 = 无有效 token） */
static int64_t s_token_expire_us = 0;

/** 最短可识别音频（16kHz/16bit 单声道 200ms = 6400 字节） */
#define ASR_MIN_BYTES       6400

/**
 * 下面这些缓冲放在静态区而不是函数栈上：
 * 调用本模块的网络任务栈只有 8KB，TLS 握手本身就要占好几 KB。
 * 只有 app_net_task 一个任务会调用本模块，所以用静态缓冲是安全的。
 *
 * EXT_RAM_BSS_ATTR：把这几块大缓冲放到 8MB PSRAM 的 .bss 里，
 * 不占内部 DIRAM（内部 RAM 还要留给任务栈、队列、WiFi/TLS）。
 * 需要 sdkconfig 打开 CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY；
 * 没打开时这个属性是空的，代码照常编译，只是回到内部 RAM。
 *
 * 缓冲大小说明：百度取 token 的响应里带一整串 scope（几十项权限），
 * 实测约 1.4KB，而且是 chunked 传输（没有 Content-Length），
 * 缓冲给小了会被截断成非法 JSON，所以这里给足。
 */
EXT_RAM_BSS_ATTR static char s_token_url[512];                            /*!< 取 token 的完整 URL */
EXT_RAM_BSS_ATTR static char s_token_resp[APP_ASR_TOKEN_RESP_BUF_SIZE];   /*!< 取 token 的响应体 */
EXT_RAM_BSS_ATTR static char s_asr_prefix[1024];                          /*!< 识别请求体 JSON 前缀 */
EXT_RAM_BSS_ATTR static char s_asr_resp[APP_ASR_RESP_BUF_SIZE];           /*!< 识别响应体 */
static char s_asr_suffix[64];                                             /*!< 识别请求体 JSON 后缀 */
static char s_asr_ctype[64];                                              /*!< 请求头 Content-Type */

/* ============================ 小工具 ============================ */

/**
 * @brief  构造 HTTP 客户端通用配置
 */
static void asr_fill_http_cfg(esp_http_client_config_t *cfg, const char *url, int timeout_ms)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->url         = url;
    cfg->method      = HTTP_METHOD_POST;
    cfg->timeout_ms  = timeout_ms;
    cfg->buffer_size = 2048;        /* 接收缓冲 */
    cfg->buffer_size_tx = 2048;     /* 发送头缓冲 */
#if APP_TLS_USE_CA_BUNDLE
    cfg->crt_bundle_attach = esp_crt_bundle_attach;     /* 用 IDF 内置根证书校验 */
#else
    cfg->skip_cert_common_name_check = true;            /* 开发阶段跳过校验 */
#endif
}

/**
 * @brief  保证把 len 字节全部写出去
 */
static esp_err_t asr_write_all(esp_http_client_handle_t client, const void *data, size_t len)
{
    const char *p       = (const char *)data;
    size_t      written = 0;

    while (written < len) {
        int n = esp_http_client_write(client, p + written, (int)(len - written));
        if (n <= 0) {
            ESP_LOGE(TAG, "写请求体失败（已写 %u/%u 字节）", (unsigned)written, (unsigned)len);
            return ESP_FAIL;
        }
        written += (size_t)n;
    }
    return ESP_OK;
}

/* ============================ token ============================ */

void app_asr_reset_token(void)
{
    s_token[0]       = '\0';
    s_token_expire_us = 0;
}

bool app_asr_has_token(void)
{
    if (s_token[0] == '\0' || s_token_expire_us == 0) {
        return false;
    }
    return esp_timer_get_time() < s_token_expire_us;
}

/**
 * @brief  获取（或复用）access_token
 *
 * @param  force  true = 强制重新获取
 * @return ESP_OK 成功（s_token 已填好）；其它为失败
 */
static esp_err_t asr_get_token(bool force)
{
    if (!force && app_asr_has_token()) {
        return ESP_OK;
    }

    if (strlen(BAIDU_ASR_API_KEY) == 0 || strcmp(BAIDU_ASR_API_KEY, "YOUR_BAIDU_API_KEY") == 0 ||
        strcmp(BAIDU_ASR_SECRET_KEY, "YOUR_BAIDU_SECRET_KEY") == 0) {
        ESP_LOGE(TAG, "还没有配置百度 API Key / Secret Key（填 components/app/app_config_secret.h）");
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(s_token_url, sizeof(s_token_url),
             "%s?grant_type=client_credentials&client_id=%s&client_secret=%s",
             BAIDU_ASR_TOKEN_URL, BAIDU_ASR_API_KEY, BAIDU_ASR_SECRET_KEY);

    ESP_LOGI(TAG, "正在获取百度 access_token ...");

    esp_http_client_config_t cfg;
    asr_fill_http_cfg(&cfg, s_token_url, BAIDU_ASR_TOKEN_TIMEOUT_MS);

    /* 响应体统一由事件回调累积（见 app_http.h 的说明） */
    app_http_resp_t resp;
    app_http_resp_init(&resp, s_token_resp, sizeof(s_token_resp));
    cfg.event_handler = app_http_event_handler;
    cfg.user_data     = &resp;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err      = ESP_FAIL;
    int64_t   t0       = esp_timer_get_time();
    bool      timed_out = false;

    do {
        /* Content-Length: 0，参数都在 URL 里 */
        esp_err_t oerr = esp_http_client_open(client, 0);
        if (oerr != ESP_OK) {
            ESP_LOGE(TAG, "连接 token 接口失败：%s", esp_err_to_name(oerr));
            timed_out = (oerr == ESP_ERR_TIMEOUT) || (oerr == ESP_ERR_HTTP_CONNECT);
            break;
        }
        if (esp_http_client_fetch_headers(client) < 0) {
            ESP_LOGE(TAG, "读取 token 响应头失败");
            timed_out = true;       /* fetch_headers 返回负数基本都是超时/连接被撕 */
            break;
        }
        int status = esp_http_client_get_status_code(client);
        app_http_pump(client);
        app_http_check_length(TAG, client, &resp);

        ESP_LOGI(TAG, "token 响应：HTTP %d，%u 字节（Content-Length=%lld）",
                 status, (unsigned)resp.len,
                 (long long)esp_http_client_get_content_length(client));

        if (resp.len == 0) {
            ESP_LOGE(TAG, "token 响应为空");
            break;
        }

        cJSON *root = app_http_parse_json(&resp);
        if (root == NULL) {
            app_http_dump(TAG, &resp);
            break;
        }

        const cJSON *token_item   = cJSON_GetObjectItem(root, "access_token");
        const cJSON *expires_item = cJSON_GetObjectItem(root, "expires_in");
        const cJSON *err_item     = cJSON_GetObjectItem(root, "error");

        if (cJSON_IsString(token_item) && token_item->valuestring != NULL) {
            strncpy(s_token, token_item->valuestring, sizeof(s_token) - 1);
            s_token[sizeof(s_token) - 1] = '\0';

            int expires_in = cJSON_IsNumber(expires_item) ? (int)expires_item->valuedouble : 2592000;
            if (expires_in <= APP_ASR_TOKEN_RENEW_SEC) {
                expires_in = APP_ASR_TOKEN_RENEW_SEC + 60;
            }
            s_token_expire_us = esp_timer_get_time() +
                                (int64_t)(expires_in - APP_ASR_TOKEN_RENEW_SEC) * 1000000LL;
            ESP_LOGI(TAG, "token 获取成功（有效期 %d 秒，本机缓存）", expires_in);
            err = ESP_OK;
        } else if (cJSON_IsString(err_item)) {
            const cJSON *desc = cJSON_GetObjectItem(root, "error_description");
            ESP_LOGE(TAG, "取 token 失败：%s（%s）", err_item->valuestring,
                     (desc != NULL && cJSON_IsString(desc)) ? desc->valuestring : "无描述");
        } else {
            app_http_dump(TAG, &resp);
        }
        cJSON_Delete(root);
    } while (0);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
    if (err != ESP_OK) {
        /* 分不清“网络慢/断”还是“Key 错”时看耗时：
         * 接近超时时间的多半是链路问题，返回 ESP_ERR_TIMEOUT，
         * 让上层重试一次，而不是直接甩给用户一句“检查密钥”。 */
        if (timed_out || elapsed_ms >= (int64_t)(BAIDU_ASR_TOKEN_TIMEOUT_MS * 8 / 10)) {
            ESP_LOGW(TAG, "取 token 失败（耗时 %lld ms，判定为超时/链路问题）", (long long)elapsed_ms);
            return ESP_ERR_TIMEOUT;
        }
        ESP_LOGE(TAG, "取 token 失败（耗时 %lld ms，判定为鉴权/参数问题）", (long long)elapsed_ms);
        return err;
    }

    ESP_LOGI(TAG, "取 token 耗时 %lld ms", (long long)elapsed_ms);
    return ESP_OK;
}

esp_err_t app_asr_prefetch_token(void)
{
    if (app_asr_has_token()) {
        return ESP_OK;
    }
    esp_err_t err = asr_get_token(false);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "开机预热：百度 token 已就绪（第一次识别不用再等）");
    } else {
        ESP_LOGW(TAG, "开机预热取 token 失败：%s（第一次识别时会自动再试）", esp_err_to_name(err));
    }
    return err;
}

/* ============================ 识别 ============================ */

/**
 * @brief  把 err_no 转成给用户看的中文提示
 */
static const char *asr_err_text(int err_no)
{
    switch (err_no) {
    case 3300: return "识别失败: 请求参数错误";
    case 3301: return "识别失败: 音频质量太差";
    case 3302: return "识别失败: 鉴权失败(检查Key)";
    case 3303: return "识别失败: 服务端错误";
    case 3304: return "识别失败: 请求超限";
    case 3305: return "识别失败: 服务未开通";
    case 3307: return "识别失败: 音频过长";
    case 3308: return "识别失败: 音频过短";
    case 3309: return "识别失败: 音频格式错误";
    default:   return "识别失败: 未知错误";
    }
}

/**
 * @brief  发一次识别请求（不含 token 获取）
 *
 * @param  out      成功时写识别文本，失败时写错误说明
 * @param  err_no   输出服务端 err_no（-1 = 本地/网络错误）
 */
static esp_err_t asr_request(const char *token, const int16_t *pcm, size_t bytes,
                             char *out, size_t out_size, int *err_no)
{
    *err_no = -1;

    /* 1. 先算好整包的 Content-Length：JSON 前缀 + base64 长度 + JSON 后缀 */
    int prefix_len = snprintf(s_asr_prefix, sizeof(s_asr_prefix),
                              "{\"format\":\"pcm\",\"rate\":%d,\"channel\":1,\"cuid\":\"%s\","
                              "\"token\":\"%s\",\"speech\":\"",
                              AUDIO_RECORDER_SAMPLE_RATE, BAIDU_ASR_CUID, token);
    int suffix_len = snprintf(s_asr_suffix, sizeof(s_asr_suffix), "\",\"len\":%u}", (unsigned)bytes);

    /* 前缀/后缀被 snprintf 截断的话 JSON 就废了，这里直接拦住 */
    if (prefix_len <= 0 || (size_t)prefix_len >= sizeof(s_asr_prefix) ||
        suffix_len <= 0 || (size_t)suffix_len >= sizeof(s_asr_suffix)) {
        ESP_LOGE(TAG, "请求体前缀/后缀缓冲不足（prefix=%d/%u，suffix=%d/%u）",
                 prefix_len, (unsigned)sizeof(s_asr_prefix),
                 suffix_len, (unsigned)sizeof(s_asr_suffix));
        return ESP_FAIL;
    }

    size_t b64_len     = 4U * ((bytes + 2U) / 3U);      /* base64 编码后的长度 */
    int    content_len = prefix_len + (int)b64_len + suffix_len;

    ESP_LOGI(TAG, "上传音频：%u 字节 PCM -> %u 字节 base64（HTTP 共 %d 字节）",
             (unsigned)bytes, (unsigned)b64_len, content_len);
    app_mem_log_brief("ASR 上传前");        /* 上传前记一笔内部堆余量，便于对账 */

    /* 2. base64 分块缓冲（3072 字节 -> 4096 字节，分块大小是 3 的倍数不会产生填充） */
    const size_t b64_cap = (APP_ASR_B64_CHUNK / 3U) * 4U + 8U;
    unsigned char *b64 = (unsigned char *)malloc(b64_cap);
    if (b64 == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg;
    asr_fill_http_cfg(&cfg, BAIDU_ASR_URL, BAIDU_ASR_TIMEOUT_MS);

    /* 响应体（err_no / result）由事件回调累积 */
    app_http_resp_t resp;
    app_http_resp_init(&resp, s_asr_resp, sizeof(s_asr_resp));
    cfg.event_handler = app_http_event_handler;
    cfg.user_data     = &resp;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        free(b64);
        return ESP_FAIL;
    }

    /* 百度要求：Content-Type: audio/pcm; rate=16000 */
    snprintf(s_asr_ctype, sizeof(s_asr_ctype), "audio/pcm; rate=%d", AUDIO_RECORDER_SAMPLE_RATE);
    esp_http_client_set_header(client, "Content-Type", s_asr_ctype);

    esp_err_t err = ESP_FAIL;

    do {
        if (esp_http_client_open(client, content_len) != ESP_OK) {
            ESP_LOGE(TAG, "连接识别接口失败（网络不通？）");
            snprintf(out, out_size, "网络连接失败");
            break;
        }

        /* 2.1 JSON 前缀 */
        if (asr_write_all(client, s_asr_prefix, (size_t)prefix_len) != ESP_OK) {
            snprintf(out, out_size, "上传失败");
            break;
        }

        /* 2.2 base64 编码后的 PCM（分块，边编边发，省内存） */
        const uint8_t *src    = (const uint8_t *)pcm;
        size_t         remain = bytes;
        bool           write_ok = true;

        while (remain > 0) {
            size_t chunk = (remain > APP_ASR_B64_CHUNK) ? (size_t)APP_ASR_B64_CHUNK : remain;
            size_t olen  = 0;

            if (mbedtls_base64_encode(b64, b64_cap, &olen, src, chunk) != 0) {
                ESP_LOGE(TAG, "base64 编码失败");
                write_ok = false;
                break;
            }
            if (asr_write_all(client, b64, olen) != ESP_OK) {
                write_ok = false;
                break;
            }
            src    += chunk;
            remain -= chunk;
        }
        if (!write_ok) {
            snprintf(out, out_size, "上传失败");
            break;
        }

        /* 2.3 JSON 后缀 */
        if (asr_write_all(client, s_asr_suffix, (size_t)suffix_len) != ESP_OK) {
            snprintf(out, out_size, "上传失败");
            break;
        }

        /* 3. 读响应（数据由事件回调累积到 resp） */
        if (esp_http_client_fetch_headers(client) < 0) {
            ESP_LOGE(TAG, "读取响应头失败");
            snprintf(out, out_size, "响应超时");
            break;
        }
        int status = esp_http_client_get_status_code(client);
        app_http_pump(client);
        app_http_check_length(TAG, client, &resp);

        ESP_LOGI(TAG, "识别响应：HTTP %d，%u 字节", status, (unsigned)resp.len);

        if (resp.len == 0) {
            ESP_LOGE(TAG, "识别响应为空（HTTP %d）", status);
            snprintf(out, out_size, "识别无响应(HTTP %d)", status);
            break;
        }

        /* 4. 解析 JSON */
        cJSON *root = app_http_parse_json(&resp);
        if (root == NULL) {
            app_http_dump(TAG, &resp);
            snprintf(out, out_size, "响应解析失败");
            break;
        }

        const cJSON *err_no_item = cJSON_GetObjectItem(root, "err_no");
        int          code        = cJSON_IsNumber(err_no_item) ? (int)err_no_item->valuedouble : -1;
        *err_no = code;

        if (code == 0) {
            const cJSON *result = cJSON_GetObjectItem(root, "result");
            const cJSON *first  = cJSON_IsArray(result) ? cJSON_GetArrayItem(result, 0) : NULL;
            if (cJSON_IsString(first) && first->valuestring != NULL && first->valuestring[0] != '\0') {
                strncpy(out, first->valuestring, out_size - 1);
                out[out_size - 1] = '\0';
                ESP_LOGI(TAG, "识别成功：%s", out);
                err = ESP_OK;
            } else {
                ESP_LOGW(TAG, "识别成功但结果为空（没听清？）");
                snprintf(out, out_size, "没听清，请再说一次");
                err = ESP_ERR_NOT_FOUND;
            }
        } else {
            const cJSON *err_msg = cJSON_GetObjectItem(root, "err_msg");
            ESP_LOGE(TAG, "识别失败：err_no=%d err_msg=%s", code,
                     cJSON_IsString(err_msg) ? err_msg->valuestring : "?");
            snprintf(out, out_size, "%s(%d)", asr_err_text(code), code);
        }
        cJSON_Delete(root);
    } while (0);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(b64);
    return err;
}

esp_err_t app_asr_init(void)
{
    app_asr_reset_token();
    ESP_LOGI(TAG, "ASR 模块就绪（接口：%s）", BAIDU_ASR_URL);
    return ESP_OK;
}

esp_err_t app_asr_recognize(const int16_t *pcm, size_t bytes, char *out, size_t out_size)
{
    if (pcm == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    if (bytes < ASR_MIN_BYTES) {
        ESP_LOGW(TAG, "录音太短（%u 字节），不上传", (unsigned)bytes);
        snprintf(out, out_size, "录音太短，请按住多说一会儿");
        return ESP_ERR_INVALID_SIZE;
    }

    int64_t t_start = esp_timer_get_time();

    /* 1. token（缓存命中直接用；开机已经预热过，这里通常是 0 秒返回） */
    esp_err_t err = asr_get_token(false);

    /* 1.1 取 token 超时：多半是刚断线重连后的第一包，等 1 秒再试一次 */
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "取 token 超时，1 秒后重试一次");
        vTaskDelay(pdMS_TO_TICKS(1000));
        err = asr_get_token(false);
    }

    if (err != ESP_OK) {
        /* 三种失败分开报，用户/开发者一眼能看出该改什么 */
        if (err == ESP_ERR_INVALID_STATE) {
            snprintf(out, out_size, "未配置百度密钥");
        } else if (err == ESP_ERR_TIMEOUT) {
            snprintf(out, out_size, "取token超时，请重试");
        } else if (err == ESP_ERR_NO_MEM) {
            snprintf(out, out_size, "内存不足");
        } else {
            snprintf(out, out_size, "百度鉴权失败(检查Key)");
        }
        ESP_LOGE(TAG, "取 token 失败：%s（%s，耗时 %lld ms）", esp_err_to_name(err), out,
                 (long long)((esp_timer_get_time() - t_start) / 1000));
        return err;
    }

    /* 2. 识别 */
    int       err_no = -1;
    esp_err_t ret    = asr_request(s_token, pcm, bytes, out, out_size, &err_no);

    /* 3. token 过期（3302）：强制刷新后重试一次 */
    if (ret != ESP_OK && err_no == 3302) {
        ESP_LOGW(TAG, "token 已失效，重新获取后重试 ...");
        if (asr_get_token(true) == ESP_OK) {
            err_no = -1;
            ret = asr_request(s_token, pcm, bytes, out, out_size, &err_no);
        }
    }

    /* 4. 传输层失败（err_no 仍是 -1：压根没拿到服务端响应，
     *    例如上传中途链路卡死 → “Poll timeout … 写请求体失败”）。
     *    这种多半是一次网络抖动，换一条新连接重试一次，别直接报废整轮对话。
     *    如果链路本身就断了（详情里的 reason=4 / mbedtls -0x4C 那种），
     *    先等 WiFi 重新连上再重试，否则必然再失败一次。 */
    if (ret != ESP_OK && err_no < 0) {
        if (!app_wifi_is_connected()) {
            ESP_LOGW(TAG, "识别失败且 WiFi 已断开，最多等 %d ms 等待重连后重试",
                     APP_ASR_WAIT_LINK_MS);
            app_bus_post_chat(CHAT_MSG_STATUS, "网络重连中，请稍候...");
            if (!app_wifi_wait_connected(APP_ASR_WAIT_LINK_MS)) {
                ESP_LOGE(TAG, "重连超时，本轮识别放弃：%s", out);
                snprintf(out, out_size, "WiFi 已断开");
                return ESP_ERR_INVALID_STATE;
            }
        } else {
            ESP_LOGW(TAG, "没拿到服务端响应（%s），1.5 秒后换新连接重试一次", out);
            vTaskDelay(pdMS_TO_TICKS(1500));
        }

        app_mem_log_brief("ASR 重试前");
        err_no = -1;
        ret = asr_request(s_token, pcm, bytes, out, out_size, &err_no);
        if (ret != ESP_OK && err_no < 0) {
            ESP_LOGE(TAG, "重试仍然失败：%s", out);
            app_mem_log_brief("ASR 重试后仍失败");
        }
    }

    ESP_LOGI(TAG, "识别结束：%s，总耗时 %lld ms",
             (ret == ESP_OK) ? "成功" : esp_err_to_name(ret),
             (long long)((esp_timer_get_time() - t_start) / 1000));
    return ret;
}
