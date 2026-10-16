/**
 * @file    app_wifi_store.c
 * @brief   已连接过的 WiFi 名单（SSID + 密码）持久化在内部 NVS
 *
 * 实现要点：
 *   1. **两边都不用**：只写 NVS，不写 TF 卡 —— 密码这种东西放明文文本文件在卡上
 *      更不安全，而且卡可能被拔走；NVS 是 flash 上的键值库，够用且不依赖卡。
 *   2. 名单整个放在内存里（10 条 × ~104 字节 ≈ 1KB），读的时候只读一次，
 *      之后查询全是内存操作（界面每画一行就要问一次“这个存过没”）。
 *   3. 写的时候**整表刷一遍**（30 来个键 + 一次 commit）：
 *      这个操作一次只有几十毫秒，且只在“连接成功”时发生，不值得为它做增量写入。
 *   4. 一把互斥锁保护全部读写：调用者可能是 WiFi 任务（连接/扫描）或 LVGL 任务（画列表）。
 *      锁只包住内存拷贝和 NVS 调用，持锁时间很短。
 *
 * 关于 seq（决定谁被覆盖）：
 *   它不是“保存时间”，而是“**最近使用**序号”，每次连接成功都会刷新，
 *   所以淘汰的是最久没用过的那个网络 —— 符合“存满覆盖最久”的需求，
 *   而且经常用的那几个不会被新网络挤掉。
 */

#include <stdio.h>
#include <string.h>

#include "app_wifi_store.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "APP_WIFI_NVS";

/* ============================ NVS 键名 ============================ */

#define STORE_KEY_COUNT   "cnt"     /*!< 记录条数（u8） */
#define STORE_KEY_SEQ     "csq"     /*!< 全局序号计数器（u32） */

#define STORE_KEY_SSID_FMT "ssid%d"
#define STORE_KEY_PASS_FMT "pw%d"
#define STORE_KEY_SEQ_FMT  "sq%d"
#define STORE_KEY_MAX_LEN  16       /*!< NVS 键最长 15 字节 + '\0' */

/* ============================ 内部状态 ============================ */

/** 内存里的名单（下标 0..s_count-1 有效） */
static app_wifi_cred_t   s_list[APP_WIFI_STORE_MAX];
static int               s_count = 0;
/** 全局序号计数器：每用一次 +1，写到 NVS 的 csq */
static uint32_t          s_seq   = 0;

static nvs_handle_t      s_nvs   = 0;
static bool              s_ready = false;
static SemaphoreHandle_t s_lock  = NULL;
static bool              s_lock_warned = false;

/* ============================ 锁 ============================ */

static void store_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    } else if (!s_lock_warned) {
        s_lock_warned = true;
        ESP_LOGW(TAG, "互斥锁不可用，名单读写没有加锁");
    }
}

static void store_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

/* ============================ 文本工具 ============================ */

/**
 * @brief  安全拷贝字符串（永远补 '\0'，不依赖 strncpy 的怪脾气）
 */
static void store_copy_str(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n > dst_size - 1) {
        n = dst_size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ============================ NVS 读写 ============================ */

/**
 * @brief  打开 NVS 与命名空间（幂等）
 */
static bool store_nvs_open(void)
{
    if (s_ready) {
        return true;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建（%s），擦除后重来", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化失败：%s（WiFi 名单这次存不住）", esp_err_to_name(err));
        return false;
    }

    err = nvs_open(APP_WIFI_STORE_NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 打开命名空间 %s 失败：%s",
                 APP_WIFI_STORE_NVS_NAMESPACE, esp_err_to_name(err));
        s_nvs = 0;
        return false;
    }

    s_ready = true;
    return true;
}

/**
 * @brief  从 NVS 读第 i 条记录
 * @return true 读到了（SSID 非空）
 */
static bool store_read_entry(int i, app_wifi_cred_t *out)
{
    char     key[STORE_KEY_MAX_LEN];
    char     ssid[APP_WIFI_SSID_MAX_LEN + 1] = {0};
    char     pass[APP_WIFI_PASS_MAX_LEN + 1] = {0};
    size_t   sl = sizeof(ssid);
    size_t   pl = sizeof(pass);
    uint32_t q  = 0;

    snprintf(key, sizeof(key), STORE_KEY_SSID_FMT, i);
    if (nvs_get_str(s_nvs, key, ssid, &sl) != ESP_OK || ssid[0] == '\0') {
        return false;
    }

    snprintf(key, sizeof(key), STORE_KEY_PASS_FMT, i);
    if (nvs_get_str(s_nvs, key, pass, &pl) != ESP_OK) {
        pass[0] = '\0';             /* 开放网络/没存密码 */
    }

    snprintf(key, sizeof(key), STORE_KEY_SEQ_FMT, i);
    if (nvs_get_u32(s_nvs, key, &q) != ESP_OK) {
        q = 0;
    }

    memset(out, 0, sizeof(*out));
    store_copy_str(out->ssid, sizeof(out->ssid), ssid);
    store_copy_str(out->password, sizeof(out->password), pass);
    out->seq = q;
    return true;
}

/**
 * @brief  把整张表写回 NVS（含条数与序号计数器）
 */
static esp_err_t store_flush(void)
{
    char      key[STORE_KEY_MAX_LEN];
    esp_err_t err = nvs_set_u8(s_nvs, STORE_KEY_COUNT, (uint8_t)s_count);

    if (err == ESP_OK) {
        err = nvs_set_u32(s_nvs, STORE_KEY_SEQ, s_seq);
    }

    for (int i = 0; err == ESP_OK && i < APP_WIFI_STORE_MAX; i++) {
        snprintf(key, sizeof(key), STORE_KEY_SSID_FMT, i);
        if (i < s_count) {
            err = nvs_set_str(s_nvs, key, s_list[i].ssid);
        } else {
            /* 用不到的槽位直接删掉，避免下次读回旧数据 */
            esp_err_t e = nvs_erase_key(s_nvs, key);
            (void)e;                /* 不存在也正常，忽略 */
        }
        if (err != ESP_OK) {
            break;
        }

        snprintf(key, sizeof(key), STORE_KEY_PASS_FMT, i);
        if (i < s_count) {
            err = nvs_set_str(s_nvs, key, s_list[i].password);
        } else {
            esp_err_t e = nvs_erase_key(s_nvs, key);
            (void)e;
        }
        if (err != ESP_OK) {
            break;
        }

        snprintf(key, sizeof(key), STORE_KEY_SEQ_FMT, i);
        if (i < s_count) {
            err = nvs_set_u32(s_nvs, key, s_list[i].seq);
        } else {
            esp_err_t e = nvs_erase_key(s_nvs, key);
            (void)e;
        }
    }

    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    return err;
}

/* ============================ 内部查找 ============================ */

/** 找 SSID 对应的下标；没有返回 -1（调用者必须已持锁） */
static int store_find_idx(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return -1;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_list[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

/** 找序号最小（最久没用过）的那条；列表为空返回 -1（调用者必须已持锁） */
static int store_oldest_idx(void)
{
    int idx = -1;

    for (int i = 0; i < s_count; i++) {
        if (idx < 0 || s_list[i].seq < s_list[idx].seq) {
            idx = i;
        }
    }
    return idx;
}

/* ============================ 对外接口 ============================ */

esp_err_t app_wifi_store_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();       /* 建锁失败也能用，只是不加锁 */
    }
    if (s_ready) {
        return ESP_OK;                          /* 已经读过一次了 */
    }
    if (!store_nvs_open()) {
        return ESP_ERR_INVALID_STATE;
    }

    store_lock();

    s_count = 0;
    s_seq   = 0;

    uint8_t cnt = 0;
    if (nvs_get_u8(s_nvs, STORE_KEY_COUNT, &cnt) != ESP_OK) {
        /* 还没有任何记录：正常情况（第一次开机） */
        store_unlock();
        ESP_LOGI(TAG, "NVS 里还没有 WiFi 记录");
        return ESP_OK;
    }
    if (cnt > APP_WIFI_STORE_MAX) {
        ESP_LOGW(TAG, "NVS 里的条数 %u 超过上限 %d，按上限读",
                 (unsigned)cnt, APP_WIFI_STORE_MAX);
        cnt = APP_WIFI_STORE_MAX;
    }

    for (uint8_t i = 0; i < cnt; i++) {
        app_wifi_cred_t cred;
        if (!store_read_entry(i, &cred)) {
            ESP_LOGW(TAG, "第 %u 条记录读不出来（跳过）", (unsigned)i);
            continue;
        }
        s_list[s_count++] = cred;
    }

    uint32_t csq = 0;
    if (nvs_get_u32(s_nvs, STORE_KEY_SEQ, &csq) == ESP_OK) {
        s_seq = csq;
    }
    /* 序号计数器必须比所有记录都大，否则新记录会一写进去就被当成“最久没用过” */
    for (int i = 0; i < s_count; i++) {
        if (s_list[i].seq > s_seq) {
            s_seq = s_list[i].seq;
        }
    }

    store_unlock();

    ESP_LOGI(TAG, "NVS 里读到 %d/%d 个 WiFi 记录（序号计数器 %u）",
             s_count, APP_WIFI_STORE_MAX, (unsigned)s_seq);
    return ESP_OK;
}

bool app_wifi_store_ready(void)
{
    return s_ready;
}

int app_wifi_store_count(void)
{
    store_lock();
    int n = s_count;
    store_unlock();
    return n;
}

int app_wifi_store_list(app_wifi_cred_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }

    store_lock();
    int n = (s_count < max) ? s_count : max;
    memcpy(out, s_list, (size_t)n * sizeof(app_wifi_cred_t));
    store_unlock();
    return n;
}

bool app_wifi_store_has(const char *ssid)
{
    store_lock();
    int idx = store_find_idx(ssid);
    store_unlock();
    return (idx >= 0);
}

bool app_wifi_store_find(const char *ssid, char *password, size_t len)
{
    if (password == NULL || len == 0) {
        return false;
    }

    store_lock();
    int idx = store_find_idx(ssid);
    if (idx >= 0) {
        store_copy_str(password, len, s_list[idx].password);
    }
    store_unlock();
    return (idx >= 0);
}

esp_err_t app_wifi_store_save(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        ESP_LOGW(TAG, "SSID 为空，不保存");
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) > APP_WIFI_SSID_MAX_LEN) {
        ESP_LOGW(TAG, "SSID 太长（%u 字节），不保存", (unsigned)strlen(ssid));
        return ESP_ERR_INVALID_ARG;
    }
    if (password != NULL && strlen(password) > APP_WIFI_PASS_MAX_LEN) {
        ESP_LOGW(TAG, "密码太长（%u 字节），不保存", (unsigned)strlen(password));
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready && !store_nvs_open()) {
        return ESP_ERR_INVALID_STATE;
    }

    store_lock();

    int idx = store_find_idx(ssid);
    bool replaced = false;
    char evicted[APP_WIFI_SSID_MAX_LEN + 1] = {0};

    if (idx < 0) {
        if (s_count < APP_WIFI_STORE_MAX) {
            idx = s_count++;                        /* 还有空位，追加 */
        } else {
            idx = store_oldest_idx();               /* 满了：覆盖最久没用过的 */
            replaced = true;
            if (idx >= 0) {
                store_copy_str(evicted, sizeof(evicted), s_list[idx].ssid);
            }
        }
    }
    if (idx < 0) {
        store_unlock();
        ESP_LOGE(TAG, "找不到可写的槽位（内部状态异常）");
        return ESP_FAIL;
    }

    memset(&s_list[idx], 0, sizeof(s_list[idx]));
    store_copy_str(s_list[idx].ssid, sizeof(s_list[idx].ssid), ssid);
    store_copy_str(s_list[idx].password, sizeof(s_list[idx].password), password);
    s_list[idx].seq = ++s_seq;

    esp_err_t err = store_flush();
    int count_now = s_count;
    store_unlock();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "写 NVS 失败：%s（本次运行仍记得，重启会丢）", esp_err_to_name(err));
        return err;
    }

    if (replaced) {
        ESP_LOGW(TAG, "WiFi 名单已满（%d 个），覆盖最久没用过的「%s」-> 记下「%s」",
                 APP_WIFI_STORE_MAX, evicted, ssid);
    } else {
        ESP_LOGI(TAG, "已记住 WiFi「%s」（%d/%d，密码 %u 字节）",
                 ssid, count_now, APP_WIFI_STORE_MAX, (unsigned)strlen(password ? password : ""));
    }
    return ESP_OK;
}

void app_wifi_store_touch(const char *ssid)
{
    if (!s_ready || ssid == NULL || ssid[0] == '\0') {
        return;
    }

    store_lock();
    int idx = store_find_idx(ssid);
    if (idx >= 0) {
        s_list[idx].seq = ++s_seq;
        (void)store_flush();
    }
    store_unlock();
}

void app_wifi_store_dump(void)
{
    store_lock();
    ESP_LOGI(TAG, "已保存的 WiFi 名单（%d/%d，按内存顺序）：", s_count, APP_WIFI_STORE_MAX);
    for (int i = 0; i < s_count; i++) {
        ESP_LOGI(TAG, "  [%d] %-32s 密码 %u 字节，序号 %u",
                 i, s_list[i].ssid, (unsigned)strlen(s_list[i].password),
                 (unsigned)s_list[i].seq);
    }
    store_unlock();
}
