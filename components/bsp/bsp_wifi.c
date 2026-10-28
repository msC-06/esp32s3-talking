/**
 * @file    bsp_wifi.c
 * @brief   ESP32-S3 板级 WiFi（STA 模式）驱动实现
 *
 * 逻辑参考旧版 BSP（esp32_s3_szp / IDF v5.1.4）的 wifi_task.c，
 * 按 IDF v5.5.5 的 API 重写，并做如下改进：
 *   1. 事件回调 + 事件组，不用轮询等待；
 *   2. 扫描结果转换成对上层友好的结构体，由调用者提供缓冲区；
 *   3. 支持断线自动重连（可开关）；
 *   4. 所有 API 都可以安全地重复调用。
 */

#include <string.h>
#include "bsp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"

static const char *TAG = "bsp_wifi";

/* ============================ 内部宏 ============================ */

#define WIFI_CONNECTED_BIT      BIT0    /* 已拿到 IP */
#define WIFI_FAIL_BIT           BIT1    /* 连接失败 */
#define WIFI_DISCONNECTED_BIT   BIT2    /* 掉线（用于自动重连任务） */
#define WIFI_STOP_BIT           BIT3    /* 通知内部任务退出 */

#define BSP_WIFI_MAX_RETRY      3       /* 自动重连的最大尝试次数 */
#define BSP_WIFI_RECONNECT_MS   3000    /* 掉线后重连间隔 */
#define BSP_WIFI_TMP_MAX_AP     64      /* 扫描临时缓冲上限，防止调用者传入超大值 */

/* ============================ 内部状态 ============================ */

static bool                     s_initialized        = false;
static bool                     s_connected          = false;
static bool                     s_auto_reconnect     = true;
static uint32_t                 s_connect_timeout_ms = BSP_WIFI_CONNECT_TIMEOUT_MS;

static esp_netif_t             *s_sta_netif          = NULL;
static EventGroupHandle_t       s_wifi_event_group   = NULL;
static SemaphoreHandle_t        s_connect_mutex      = NULL;
static TaskHandle_t             s_reconnect_task     = NULL;

static esp_event_handler_instance_t s_wifi_evt_inst  = NULL;
static esp_event_handler_instance_t s_ip_evt_inst    = NULL;

/* 当前/上一次连接信息（自动重连需要） */
static char                     s_ssid[33]           = {0};
static char                     s_password[65]       = {0};
static char                     s_ip_str[16]         = {0};
static int8_t                   s_rssi               = 0;

/* ============================ 工具函数 ============================ */

static void bsp_wifi_reset_state(void)
{
    s_connected = false;
    s_rssi      = 0;
    memset(s_ip_str, 0, sizeof(s_ip_str));
    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group,
                             WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_DISCONNECTED_BIT);
    }
}

/* ============================ 事件回调 ============================ */

static void bsp_wifi_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA 启动");
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "已连接到 AP，等待分配 IP ...");
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *evt = (wifi_event_sta_disconnected_t *)event_data;
            s_connected = false;
            s_rssi      = 0;
            memset(s_ip_str, 0, sizeof(s_ip_str));
            ESP_LOGW(TAG, "与 AP 断开连接，reason=%d", evt ? evt->reason : -1);

            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT | WIFI_DISCONNECTED_BIT);

            /* 主动 disconnect（reason=8 ASSOC_LEAVE）时不触发自动重连 */
            if (evt && evt->reason == WIFI_REASON_ASSOC_LEAVE) {
                xEventGroupClearBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
            }
            break;
        }

        case WIFI_EVENT_SCAN_DONE:
            ESP_LOGD(TAG, "扫描完成");
            break;

        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
            snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&evt->ip_info.ip));
            s_connected = true;
            s_rssi      = bsp_wifi_get_rssi();
            xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT | WIFI_DISCONNECTED_BIT);
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            ESP_LOGI(TAG, "获取到 IP: %s", s_ip_str);
        } else if (event_id == IP_EVENT_STA_LOST_IP) {
            s_connected = false;
            memset(s_ip_str, 0, sizeof(s_ip_str));
            ESP_LOGW(TAG, "IP 丢失");
        }
    }
}

/* ============================ 自动重连任务 ============================ */

static void bsp_wifi_reconnect_task(void *arg)
{
    int retry = 0;

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               WIFI_DISCONNECTED_BIT | WIFI_STOP_BIT,
                                               pdFALSE, pdFALSE, portMAX_DELAY);

        if (bits & WIFI_STOP_BIT) {
            break;
        }

        if (!s_auto_reconnect || s_ssid[0] == '\0') {
            xEventGroupClearBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(BSP_WIFI_RECONNECT_MS));

        /* 等待期间可能已经连上或被要求退出 */
        if ((xEventGroupGetBits(s_wifi_event_group) & (WIFI_CONNECTED_BIT | WIFI_STOP_BIT)) != 0) {
            xEventGroupClearBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
            retry = 0;
            continue;
        }

        if (++retry > BSP_WIFI_MAX_RETRY) {
            ESP_LOGE(TAG, "自动重连 %d 次均失败，停止重连", BSP_WIFI_MAX_RETRY);
            xEventGroupClearBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
            retry = 0;
            continue;
        }

        ESP_LOGW(TAG, "自动重连中（第 %d 次）...", retry);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_connect 失败: %s", esp_err_to_name(err));
        } else {
            /* 等待本次结果，最多 10 秒 */
            EventBits_t r = xEventGroupWaitBits(s_wifi_event_group,
                                                WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT,
                                                pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
            if (r & WIFI_CONNECTED_BIT) {
                ESP_LOGI(TAG, "自动重连成功");
                xEventGroupClearBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
                retry = 0;
            }
        }

        if (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) {
            xEventGroupClearBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
            retry = 0;
        }
    }

    s_reconnect_task = NULL;
    vTaskDelete(NULL);
}

/* ============================ 初始化 ============================ */

esp_err_t bsp_wifi_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "WiFi 已经初始化过了");
        return ESP_OK;
    }

    esp_err_t err;

    /* 1. NVS：WiFi 需要保存 PHY 校准数据 */
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "nvs_flash_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 2. 事件组（连接结果通知）与互斥锁 */
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL) {
            ESP_LOGE(TAG, "创建事件组失败");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_connect_mutex == NULL) {
        s_connect_mutex = xSemaphoreCreateMutex();
        if (s_connect_mutex == NULL) {
            ESP_LOGE(TAG, "创建互斥锁失败");
            return ESP_ERR_NO_MEM;
        }
    }

    /* 3. TCP/IP 协议栈与默认事件循环 */
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init 失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "创建默认事件循环失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 4. 创建 STA 网络接口 */
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL) {
            ESP_LOGE(TAG, "创建 STA netif 失败");
            return ESP_FAIL;
        }
    }

    /* 5. 注册事件回调 */
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              bsp_wifi_event_handler, NULL, &s_wifi_evt_inst);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 WIFI_EVENT 回调失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                              bsp_wifi_event_handler, NULL, &s_ip_evt_inst);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 IP_EVENT 回调失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 6. 初始化 WiFi 驱动 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 不把 SSID/密码写入 flash，避免依赖分区表里的 nvs 数据 */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    /* 7. STA 模式 + 国家码 + 关闭省电（省电模式会明显增加延迟） */
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode 失败: %s", esp_err_to_name(err));
        return err;
    }

    wifi_country_t country = {
        .cc     = "CN",
        .schan  = 1,
        .nchan  = 13,
        .policy = WIFI_COUNTRY_POLICY_AUTO,
    };
    esp_wifi_set_country(&country);     /* 失败不影响使用，不当作致命错误 */

    /*
     * 省电模式：用 WIFI_PS_MIN_MODEM，不要用 WIFI_PS_NONE。
     *
     * 现象（踩过的坑）：连接手机/路由热点时，空闲十几秒后 AP 主动把设备踢下线，
     *   日志是 “与 AP 断开连接, reason=4”，紧接着 TLS 握手报
     *   “mbedtls_ssl_handshake returned -0x004C”（= MBEDTLS_ERR_NET_RECV_FAILED），
     *   于是第一次说话必然识别失败。
     *
     * 原因：WIFI_PS_NONE 表示“永不休眠”，但设备空闲时**一个包都不发**，
     *   部分 AP 会把长时间没有任何帧的客户端当成僵尸终端清掉（reason=4 就是
     *   “due to inactivity”）；而 MIN_MODEM 会在 DTIM 周期醒来收 beacon
     *   并回一个 NULL 数据帧，AP 就认为终端还活着，不会踢。
     *   代价只是平均电流略高一点，对插电/充电宝供电的手持设备没有影响。
     */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    /* 8. 启动 WiFi */
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start 失败: %s", esp_err_to_name(err));
        return err;
    }

    bsp_wifi_reset_state();
    s_initialized = true;

    /* 9. 自动重连任务 */
    if (s_reconnect_task == NULL) {
        if (xTaskCreate(bsp_wifi_reconnect_task, "bsp_wifi_rc", 3 * 1024, NULL, 5,
                        &s_reconnect_task) != pdPASS) {
            ESP_LOGW(TAG, "创建自动重连任务失败（不影响基本功能）");
            s_reconnect_task = NULL;
        }
    }

    ESP_LOGI(TAG, "WiFi 初始化完成（STA 模式）");
    return ESP_OK;
}

esp_err_t bsp_wifi_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    if (s_wifi_event_group && s_reconnect_task) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_STOP_BIT);
        /* 等任务自己退出 */
        for (int i = 0; i < 20 && s_reconnect_task; i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_wifi_evt_inst) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_evt_inst);
        s_wifi_evt_inst = NULL;
    }
    if (s_ip_evt_inst) {
        esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, s_ip_evt_inst);
        s_ip_evt_inst = NULL;
    }
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    s_initialized = false;
    bsp_wifi_reset_state();
    s_ssid[0]     = '\0';
    s_password[0] = '\0';

    ESP_LOGI(TAG, "WiFi 已反初始化");
    return ESP_OK;
}

bool bsp_wifi_is_initialized(void)
{
    return s_initialized;
}

/* ============================ 连接 ============================ */

esp_err_t bsp_wifi_connect(const char *ssid, const char *password)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "WiFi 尚未初始化，请先调用 bsp_wifi_init()");
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid == NULL || strlen(ssid) == 0 || strlen(ssid) > 32) {
        ESP_LOGE(TAG, "SSID 非法");
        return ESP_ERR_INVALID_ARG;
    }
    if (password != NULL && strlen(password) > 64) {
        ESP_LOGE(TAG, "密码过长");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_connect_mutex == NULL ||
        xSemaphoreTake(s_connect_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "获取连接互斥锁失败（可能正在连接中）");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;

    /* 保存连接信息，供自动重连使用 */
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    if (password) {
        strncpy(s_password, password, sizeof(s_password) - 1);
        s_password[sizeof(s_password) - 1] = '\0';
    } else {
        s_password[0] = '\0';
    }

    /* 组包 */
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, s_ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (s_password[0] != '\0') {
        strncpy((char *)wifi_config.sta.password, s_password, sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    wifi_config.sta.pmf_cfg.capable  = false;
    wifi_config.sta.pmf_cfg.required = false;
    wifi_config.sta.scan_method      = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method      = WIFI_CONNECT_AP_BY_SIGNAL;   /* 同名 AP 取信号最强的 */

    bsp_wifi_reset_state();

    /* 先断开旧连接，避免状态残留 */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config 失败: %s", esp_err_to_name(err));
        xSemaphoreGive(s_connect_mutex);
        return err;
    }

    ESP_LOGI(TAG, "正在连接 \"%s\" ...", s_ssid);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect 失败: %s", esp_err_to_name(err));
        xSemaphoreGive(s_connect_mutex);
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(s_connect_timeout_ms));
    xSemaphoreGive(s_connect_mutex);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "连接 \"%s\" 成功，IP=%s", s_ssid, s_ip_str);
        return ESP_OK;
    }
    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "连接 \"%s\" 失败（密码错误或 AP 不可达）", s_ssid);
        return ESP_FAIL;
    }

    ESP_LOGE(TAG, "连接 \"%s\" 超时（%u ms）", s_ssid, (unsigned)s_connect_timeout_ms);
    return ESP_ERR_TIMEOUT;
}

esp_err_t bsp_wifi_connect_with_retry(const char *ssid, const char *password, int max_retry)
{
    if (max_retry < 1) {
        max_retry = 1;
    }

    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < max_retry; i++) {
        err = bsp_wifi_connect(ssid, password);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_STATE) {
            return err;     /* 参数/状态错误重试也没用 */
        }
        ESP_LOGW(TAG, "第 %d/%d 次连接失败，%d ms 后重试", i + 1, max_retry, BSP_WIFI_RECONNECT_MS);
        vTaskDelay(pdMS_TO_TICKS(BSP_WIFI_RECONNECT_MS));
    }
    return err;
}

esp_err_t bsp_wifi_disconnect(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 主动断开，不再触发自动重连 */
    s_auto_reconnect = false;
    esp_err_t err = esp_wifi_disconnect();
    s_auto_reconnect = true;
    bsp_wifi_reset_state();
    return err;
}

bool bsp_wifi_is_connected(void)
{
    if (!s_initialized || !s_connected) {
        return false;
    }
    /* 再向驱动确认一次，避免状态不同步 */
    wifi_ap_record_t ap;
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
}

void bsp_wifi_set_auto_reconnect(bool enable)
{
    s_auto_reconnect = enable;
    if (!enable && s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
    }
}

void bsp_wifi_set_connect_timeout(uint32_t timeout_ms)
{
    s_connect_timeout_ms = (timeout_ms == 0) ? BSP_WIFI_CONNECT_TIMEOUT_MS : timeout_ms;
}

esp_err_t bsp_wifi_set_power_save(bool enable)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* MIN_MODEM 是开机时的默认档；省电时切到 MAX_MODEM（只在 DTIM 醒来） */
    wifi_ps_type_t ps  = enable ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM;
    esp_err_t      err = esp_wifi_set_ps(ps);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "切换 WiFi 省电模式失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WiFi 省电模式：%s", enable ? "MAX_MODEM（省电）" : "MIN_MODEM（默认）");
    return ESP_OK;
}

/* ============================ 扫描 ============================ */

int bsp_wifi_scan(bsp_wifi_ap_info_t *buffer, uint16_t max_count)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "WiFi 尚未初始化");
        return -3;
    }
    if (buffer == NULL || max_count == 0) {
        ESP_LOGE(TAG, "扫描缓冲区非法");
        return -2;
    }
    if (max_count > BSP_WIFI_TMP_MAX_AP) {
        max_count = BSP_WIFI_TMP_MAX_AP;
    }

    wifi_scan_config_t scan_config = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,                       /* 全信道扫描 */
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time   = {
            .active = {
                .min = 100,
                .max = 300,
            },
        },
    };

    ESP_LOGI(TAG, "开始扫描 WiFi ...");
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);   /* true = 阻塞直到扫描结束 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "扫描失败: %s", esp_err_to_name(err));
        return -1;
    }

    uint16_t ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "获取 AP 数量失败: %s", esp_err_to_name(err));
        esp_wifi_clear_ap_list();
        return -1;
    }

    if (ap_count == 0) {
        ESP_LOGW(TAG, "没有扫描到任何 AP");
        esp_wifi_clear_ap_list();
        return 0;
    }

    uint16_t want = (ap_count > max_count) ? max_count : ap_count;

    wifi_ap_record_t *records = (wifi_ap_record_t *)calloc(want, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        ESP_LOGE(TAG, "扫描结果缓冲区分配失败（%u 条）", (unsigned)want);
        esp_wifi_clear_ap_list();
        return -1;
    }

    uint16_t got = want;
    err = esp_wifi_scan_get_ap_records(&got, records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取扫描结果失败: %s", esp_err_to_name(err));
        free(records);
        esp_wifi_clear_ap_list();
        return -1;
    }

    for (uint16_t i = 0; i < got; i++) {
        memset(&buffer[i], 0, sizeof(bsp_wifi_ap_info_t));
        memcpy(buffer[i].ssid, records[i].ssid, sizeof(buffer[i].ssid) - 1);
        memcpy(buffer[i].bssid, records[i].bssid, sizeof(buffer[i].bssid));
        buffer[i].rssi     = records[i].rssi;
        buffer[i].channel  = records[i].primary;
        buffer[i].authmode = records[i].authmode;
    }

    free(records);
    esp_wifi_clear_ap_list();       /* 释放驱动里剩余的扫描结果 */

    ESP_LOGI(TAG, "扫描完成：共 %u 个 AP，返回 %u 个", (unsigned)ap_count, (unsigned)got);
    for (uint16_t i = 0; i < got; i++) {
        ESP_LOGI(TAG, "  [%2u] %-32s  RSSI=%4d dBm  CH=%2u  %s",
                 (unsigned)(i + 1), buffer[i].ssid, buffer[i].rssi, buffer[i].channel,
                 buffer[i].authmode == WIFI_AUTH_OPEN ? "OPEN" : "ENCRYPTED");
    }

    return (int)got;
}

/* ============================ 状态查询 ============================ */

esp_err_t bsp_wifi_get_ap_info(bsp_wifi_ap_info_t *info)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_ap_record_t ap;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err != ESP_OK) {
        return ESP_FAIL;
    }

    if (info) {
        memset(info, 0, sizeof(bsp_wifi_ap_info_t));
        memcpy(info->ssid, ap.ssid, sizeof(info->ssid) - 1);
        memcpy(info->bssid, ap.bssid, sizeof(info->bssid));
        info->rssi     = ap.rssi;
        info->channel  = ap.primary;
        info->authmode = ap.authmode;
    }
    return ESP_OK;
}

int8_t bsp_wifi_get_rssi(void)
{
    if (!s_initialized) {
        return 0;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    s_rssi = ap.rssi;
    return ap.rssi;
}

esp_err_t bsp_wifi_get_ip(char *ip_str, size_t len)
{
    if (ip_str == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || !s_connected || s_ip_str[0] == '\0') {
        return ESP_FAIL;
    }
    strncpy(ip_str, s_ip_str, len - 1);
    ip_str[len - 1] = '\0';
    return ESP_OK;
}

const char *bsp_wifi_get_ssid(void)
{
    return s_ssid;
}
