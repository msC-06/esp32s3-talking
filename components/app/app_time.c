/**
 * @file    app_time.c
 * @brief   系统时间（SNTP 对时）实现
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "app_time.h"
#include "app_config.h"
#include "app_wifi.h"               /* 等链路（app 层内部互相调用） */

#include "sdkconfig.h"              /* CONFIG_LWIP_SNTP_MAX_SERVERS */
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "APP_TIME";

/* ============================ 内部状态（全部 static） ============================ */

/** 时间是否“可信”的分界线：2020-01-01 00:00:00 UTC */
#define TIME_SYNCED_MIN_SEC     1577836800L

/** 农历/星期的中文名（tm_wday：0 = 周日） */
static const char *const s_weekday_cn[7] = {
    "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六",
};

static volatile bool s_synced_flag = false;   /*!< SNTP 回调置位（写者：lwip） */
static bool          s_sntp_inited = false;   /*!< 只允许 app_time 任务访问 */
static TaskHandle_t  s_task        = NULL;

/* ============================ 内部函数 ============================ */

/**
 * @brief  SNTP 对时成功回调
 *
 * @note   运行在 lwip 的 tcpip 线程里：**只置标志**，不打印、不阻塞、不分配内存。
 *         打印统一放到对时任务里做（那边日志上下文更清楚）。
 */
static void time_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_synced_flag = true;
}

/**
 * @brief  首次使用时配置 SNTP（服务器列表 + 回调 + 启动）
 */
static void time_sntp_setup(void)
{
    if (s_sntp_inited) {
        return;
    }

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, APP_TIME_SNTP_SERVER1);
#if (CONFIG_LWIP_SNTP_MAX_SERVERS > 1)
    esp_sntp_setservername(1, APP_TIME_SNTP_SERVER2);
#endif
#if (CONFIG_LWIP_SNTP_MAX_SERVERS > 2)
    esp_sntp_setservername(2, APP_TIME_SNTP_SERVER3);
#endif
    esp_sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_init();

    s_sntp_inited = true;
    ESP_LOGI(TAG, "SNTP 已启动：主服务器 %s，可用服务器槽位数 %d，时区 %s",
             APP_TIME_SNTP_SERVER1, (int)CONFIG_LWIP_SNTP_MAX_SERVERS, APP_TIME_TZ);
}

/**
 * @brief  对时任务：等链路 → 对时 → 定期再对一次
 */
static void time_task(void *arg)
{
    (void)arg;

    int    round = 0;
    for (;;) {
        /* 1. 没链路就别发请求（DHCP 还没拿到 IP，DNS 也解析不了） */
        if (!app_wifi_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* 2. 已经同步过就只按长周期复查，别频繁打扰 NTP 服务器 */
        if (app_time_is_synced()) {
            vTaskDelay(pdMS_TO_TICKS(APP_TIME_RESYNC_INTERVAL_MS));
            continue;
        }

        /* 3. 对时（阻塞在 sync_once 里等待结果） */
        round++;
        if (app_time_sync_once(APP_TIME_SYNC_TIMEOUT_MS) == ESP_OK) {
            char buf[48] = {0};
            app_time_now_str(buf, sizeof(buf));
            ESP_LOGI(TAG, "时间同步成功：%s（第 %d 次尝试）", buf, round);
            round = 0;
            vTaskDelay(pdMS_TO_TICKS(APP_TIME_RESYNC_INTERVAL_MS));
        } else {
            ESP_LOGW(TAG, "时间同步失败（第 %d 次），%d 秒后重试",
                     round, APP_TIME_RETRY_MS / 1000);
            vTaskDelay(pdMS_TO_TICKS(APP_TIME_RETRY_MS));
        }
    }
}

/* ============================ 对外接口 ============================ */

esp_err_t app_time_start(void)
{
#if !APP_TIME_ENABLE
    ESP_LOGW(TAG, "时间模块已关闭（APP_TIME_ENABLE=0），界面不会显示时间");
    return ESP_OK;
#else
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 时区必须在取时间之前设好：localtime_r() 依赖 TZ 环境变量 */
    setenv("TZ", APP_TIME_TZ, 1);
    tzset();

    if (xTaskCreatePinnedToCore(time_task, "app_time", APP_TIME_TASK_STACK, NULL,
                                APP_TIME_TASK_PRIO, &s_task,
                                APP_TIME_TASK_CORE) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "对时任务创建失败（内存不足？）");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "时间模块就绪（Core %d，优先级 %d，栈 %d 字节，时区 %s）",
             (int)APP_TIME_TASK_CORE, (int)APP_TIME_TASK_PRIO,
             APP_TIME_TASK_STACK, APP_TIME_TZ);
    return ESP_OK;
#endif
}

esp_err_t app_time_sync_once(uint32_t timeout_ms)
{
#if !APP_TIME_ENABLE
    (void)timeout_ms;
    return ESP_ERR_INVALID_STATE;
#else
    time_sntp_setup();

    /* 每次调用都重新发起请求：lwip 默认要等一个更新周期（CONFIG_LWIP_SNTP_UPDATE_DELAY
     * 默认 1 小时）才会重试，而我们开机时多半因为“还没连上”失败过，
     * 不主动重启就要等一小时才有时间。
     * 已经同步成功的情况由调用方（对时任务）负责不要再频繁调用。 */
    esp_sntp_restart();

    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if (app_time_is_synced()) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
    return app_time_is_synced() ? ESP_OK : ESP_ERR_TIMEOUT;
#endif
}

bool app_time_wait_synced(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if (app_time_is_synced()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
    return app_time_is_synced();
}

bool app_time_is_synced(void)
{
    /* 两种情况都算“时间可信”：
     *   1. SNTP 回调触发过（s_synced_flag）；
     *   2. 当前时间晚于 2020-01-01（对过时，或别人手动 set 过）。
     * 用“时间值”兜底比只看标志位可靠：标志位是 lwip 回调置的，
     * 万一回调没触发但时间已经设进去了，也不会误判成“没对时”。 */
    return s_synced_flag || (time(NULL) > (time_t)TIME_SYNCED_MIN_SEC);
}

esp_err_t app_time_now(struct tm *out)
{
    time_t now = time(NULL);
    struct tm t;

    localtime_r(&now, &t);
    if (out != NULL) {
        *out = t;
    }
    return ESP_OK;
}

int app_time_now_str(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return -1;
    }
    time_t     now = time(NULL);
    struct tm  t;
    localtime_r(&now, &t);

    int n = snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d %s",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec,
                     s_weekday_cn[((t.tm_wday % 7) + 7) % 7]);
    return (n > 0 && (size_t)n < len) ? n : -1;
}

int app_time_now_clock(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return -1;
    }
    time_t    now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    int n = snprintf(buf, len, "%02d:%02d", t.tm_hour, t.tm_min);
    return (n > 0 && (size_t)n < len) ? n : -1;
}

int app_time_format(char *buf, size_t len, const char *fmt)
{
    if (buf == NULL || len == 0 || fmt == NULL) {
        return -1;
    }
    time_t    now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    size_t n = strftime(buf, len, fmt, &t);
    return (n > 0) ? (int)n : -1;
}

esp_err_t app_time_llm_text(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!app_time_is_synced()) {
        /* 关键：这里**不能**把 1970 报给模型，否则它会一本正经地胡说。
         * 明确说“不知道”，模型一般会告诉用户自己无法确定当前时间。 */
        snprintf(buf, len, "\n（注意：设备还没和网络对时，你不知道当前日期和时间；"
                          "如果用户问到，请直接说明你无法确定。）");
        return ESP_ERR_INVALID_STATE;
    }

    time_t    now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    snprintf(buf, len,
             "\n当前时间：%04d年%02d月%02d日 %02d:%02d（%s）。"
             "用户问到日期、时间、星期时，一律以上面这个为准，不要自己推算。",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min,
             s_weekday_cn[((t.tm_wday % 7) + 7) % 7]);
    return ESP_OK;
}
