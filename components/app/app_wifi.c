/**
 * @file    app_wifi.c
 * @brief   WiFi（STA）连接管理实现：状态刷新 + 扫描/连接请求 + 已知网络自动连接
 *
 * 这个文件现在管三件事（都跑在 app_wifi_task 这一个任务里，Core 0）：
 *
 *   1. **状态刷新**：每 APP_WIFI_STATUS_REFRESH_MS 把“连没连 + 信号档位”投给顶栏；
 *   2. **界面请求**：界面上点“扫描/连接”时，请求从队列进来，在这里（唯一的安全位置）
 *      执行 net_scan()/net_connect()，因为这两个调用都是**阻塞**的
 *      （全信道扫描 1~3 秒、连接最长 15 秒），放 LVGL 任务里界面就冻住了；
 *   3. **已知网络自动连接**：没连上时每隔 APP_WIFI_KNOWN_SCAN_INTERVAL_MS
 *      扫一遍附近，发现“记过的网络”（NVS 里存过的 + 编译期配置的那个）就自己连上去。
 *      现场实测：底层的自动重连只认最后一次连接的那个 AP，换个地方就没用了，
 *      所以这条路径是“开机自动上网”的主要保障。
 *
 * 连上以后会把 SSID + 密码写进 NVS（见 app_wifi_store.c），
 * 下次开机即使用户没动过界面，也能自己连回去。
 */

#include <stdio.h>
#include <string.h>

#include "app_wifi.h"
#include "app_wifi_store.h"
#include "app_bus.h"
#include "app_config.h"

#include "my_drivers/net/net.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "APP_WIFI";

/* ============================ 内部类型 ============================ */

/** 界面 -> 本任务的请求 */
typedef enum {
    WIFI_REQ_SCAN = 1,          /*!< 扫一次 */
    WIFI_REQ_CONNECT,           /*!< 连指定的 AP */
} wifi_req_type_t;

typedef struct {
    wifi_req_type_t type;
    char            ssid[APP_WIFI_SSID_MAX_LEN + 1];
    char            password[APP_WIFI_PASS_MAX_LEN + 1];
} wifi_req_t;

/** “已知网络”候选（用于自动连接） */
typedef struct {
    char   ssid[APP_WIFI_SSID_MAX_LEN + 1];
    char   password[APP_WIFI_PASS_MAX_LEN + 1];
    int8_t rssi;                /*!< 扫描里看到的信号；RSSI_UNSEEN = 没扫到 */
    bool   from_config;         /*!< true = 来自 app_config_secret.h */
} wifi_cand_t;

/** “没在扫描结果里看到”的标记（真实 RSSI 一定比它大，排序时自然排最后） */
#define WIFI_RSSI_UNSEEN    (-128)

/* ============================ 内部状态（全部 static） ============================ */

static char s_ip[20]           = "0.0.0.0";
/** 上一次投递到界面的 WiFi 状态文字（避免重复刷屏） */
static char s_last_status[64]  = "";

/* ---- 界面请求队列 + 保护共享数据的互斥锁 ---- */
static QueueHandle_t     s_req_q = NULL;
static SemaphoreHandle_t s_lock  = NULL;

/* ---- 扫描结果缓存（界面读它画列表） ---- */
static app_wifi_ap_t         s_scan_list[APP_WIFI_SCAN_LIST_MAX];
static app_wifi_ap_t         s_scan_build[APP_WIFI_SCAN_LIST_MAX];   /*!< 组装用，发布时整体拷过去 */
static int                   s_scan_count   = 0;
static app_wifi_scan_state_t s_scan_state   = APP_WIFI_SCAN_IDLE;
static uint32_t              s_scan_version = 0;

/* ---- 连接请求状态（界面读它显示进度） ---- */
static app_wifi_connect_state_t s_conn_state   = APP_WIFI_CONNECT_IDLE;
static uint32_t                 s_conn_version = 0;
static char                     s_conn_msg[96] = "";
static char                     s_conn_ssid[APP_WIFI_SSID_MAX_LEN + 1] = "";

/* ---- 大一点的缓冲一律放静态区：本文件的函数都在 app_wifi_task（栈 5KB）里跑 ---- */
static net_ap_info_t s_ap_raw[APP_WIFI_SCAN_LIST_MAX];      /*!< net_scan 的原始结果 */
static wifi_cand_t   s_cands[APP_WIFI_STORE_MAX + 1];       /*!< 自动连接的候选清单 */
static app_wifi_cred_t s_creds[APP_WIFI_STORE_MAX];         /*!< NVS 名单快照 */

/* ============================ 锁 / 小工具 ============================ */

static void wifi_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void wifi_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

/** 编译期配置的 SSID 是否可用（模板占位符 = 没配） */
static bool wifi_config_ssid_valid(void)
{
    return (APP_WIFI_SSID[0] != '\0') && (strcmp(APP_WIFI_SSID, "YOUR_WIFI_SSID") != 0);
}

/**
 * @brief  把 WiFi 状态投递到界面（内容没变就不投）
 *
 * @note   左上角只显示两项：**连没连** + **信号档位**（强/中/弱），
 *         不显示 IP、不显示 dBm —— 顶栏只有 320 像素宽，
 *         而 IP 和 RSSI 数值对用户没意义（要看详细值看串口日志，那里会打全）。
 *         内容不变时不投递，所以 2.5 秒轮询也不会刷屏。
 */
static void wifi_post_status(const char *text)
{
    if (text == NULL || strcmp(text, s_last_status) == 0) {
        return;
    }
    strncpy(s_last_status, text, sizeof(s_last_status) - 1);
    s_last_status[sizeof(s_last_status) - 1] = '\0';

    ESP_LOGI(TAG, "界面 WiFi 状态 -> 「%s」", text);
    app_bus_post_chat(CHAT_MSG_WIFI, text);
}

/**
 * @brief  刷新本机 IP 缓存
 */
static void wifi_refresh_ip(void)
{
    char ip[20] = {0};
    if (net_get_ip(ip, sizeof(ip)) == ESP_OK) {
        strncpy(s_ip, ip, sizeof(s_ip) - 1);
        s_ip[sizeof(s_ip) - 1] = '\0';
    } else {
        strncpy(s_ip, "0.0.0.0", sizeof(s_ip) - 1);
    }
}

/**
 * @brief  信号强度分档：强 / 中 / 弱（阈值见 app_config.h）
 */
static const char *wifi_level_text(int8_t rssi)
{
    if (rssi >= APP_WIFI_RSSI_STRONG) {
        return "强";
    }
    if (rssi >= APP_WIFI_RSSI_MEDIUM) {
        return "中";
    }
    return "弱";
}

/**
 * @brief  按“是否联网 + 信号档位”刷新一次界面状态（详细数值只进串口日志）
 *
 * @param  connected 当前是否已连接（已拿到 IP）
 */
static void wifi_refresh_status(bool connected)
{
    char text[32];

    if (connected) {
        int8_t rssi = net_get_rssi();
        wifi_refresh_ip();
        snprintf(text, sizeof(text), "WiFi %s", wifi_level_text(rssi));
        ESP_LOGI(TAG, "已连接 %s：IP %s，信号 %d dBm（%s）",
                 net_get_ssid(), s_ip, (int)rssi, wifi_level_text(rssi));
    } else {
        snprintf(text, sizeof(text), "WiFi未连接");
        strncpy(s_ip, "0.0.0.0", sizeof(s_ip) - 1);
        s_ip[sizeof(s_ip) - 1] = '\0';
    }

    wifi_post_status(text);
}

/* ============================ 扫描 ============================ */

/** 设置扫描状态（bump = 是否让版本号 +1；只有“有新结果/失败”才需要） */
static void wifi_set_scan_state(app_wifi_scan_state_t st, bool bump)
{
    wifi_lock();
    s_scan_state = st;
    if (bump) {
        s_scan_version++;
    }
    wifi_unlock();
}

/**
 * @brief  真的扫一次，并把结果整理进界面缓存
 *
 * @param  for_ui true = 界面点的扫描（要更新状态/版本号，让界面刷新）
 * @return >=0 扫到的 AP 数量（原始条数）；<0 失败
 *
 * @note   只有 app_wifi_task 会调用它（net_scan 是阻塞的）。
 */
static int wifi_do_scan(bool for_ui)
{
    if (for_ui) {
        wifi_set_scan_state(APP_WIFI_SCAN_RUNNING, false);
    }

    int n = net_scan(s_ap_raw, APP_WIFI_SCAN_LIST_MAX);
    if (n < 0) {
        ESP_LOGW(TAG, "扫描失败（%d）", n);
        if (for_ui) {
            wifi_set_scan_state(APP_WIFI_SCAN_FAILED, true);
        }
        return n;
    }

    /* 去重（同名 AP 只留信号最强的）+ 插入排序（按 RSSI 从强到弱） */
    int out = 0;
    for (int i = 0; i < n; i++) {
        if (s_ap_raw[i].ssid[0] == '\0') {
            continue;                       /* 隐藏 SSID：界面没法显示，也不给点 */
        }

        int found = -1;
        for (int k = 0; k < out; k++) {
            if (strcmp(s_scan_build[k].ssid, s_ap_raw[i].ssid) == 0) {
                found = k;
                break;
            }
        }
        if (found >= 0) {
            if (s_ap_raw[i].rssi > s_scan_build[found].rssi) {
                s_scan_build[found].rssi = s_ap_raw[i].rssi;
            }
            continue;
        }
        if (out >= APP_WIFI_SCAN_LIST_MAX) {
            break;                          /* 列表满了，后面的直接丢（已经按信号排序） */
        }

        int pos = out;
        while (pos > 0 && s_scan_build[pos - 1].rssi < s_ap_raw[i].rssi) {
            s_scan_build[pos] = s_scan_build[pos - 1];
            pos--;
        }
        memset(&s_scan_build[pos], 0, sizeof(s_scan_build[0]));
        strncpy(s_scan_build[pos].ssid, s_ap_raw[i].ssid, sizeof(s_scan_build[pos].ssid) - 1);
        s_scan_build[pos].rssi   = s_ap_raw[i].rssi;
        s_scan_build[pos].secure = (s_ap_raw[i].authmode != WIFI_AUTH_OPEN);
        out++;
    }

    /* 一次性发布（界面可能正在读，必须整体替换） */
    wifi_lock();
    memcpy(s_scan_list, s_scan_build, sizeof(s_scan_build));
    s_scan_count = out;
    if (for_ui) {
        s_scan_state = APP_WIFI_SCAN_DONE;
        s_scan_version++;
    }
    wifi_unlock();

    ESP_LOGI(TAG, "扫描完成：%d 个 AP（去重后 %d 个）%s",
             n, out, for_ui ? "，已通知界面" : "（后台自动连接用）");
    for (int i = 0; i < out; i++) {
        ESP_LOGI(TAG, "  [%2d] %-32s RSSI=%4d dBm %s",
                 i + 1, s_scan_list[i].ssid, s_scan_list[i].rssi,
                 s_scan_list[i].secure ? "加密" : "开放");
    }
    return n;
}

void app_wifi_scan_request(void)
{
    if (s_req_q == NULL) {
        ESP_LOGW(TAG, "请求队列还没建好，忽略这次扫描请求");
        return;
    }

    wifi_req_t req;
    memset(&req, 0, sizeof(req));
    req.type = WIFI_REQ_SCAN;

    if (xQueueSend(s_req_q, &req, 0) != pdTRUE) {
        /* 队列满：丢掉最老的一条请求再塞（用户连点两下不该被拒） */
        wifi_req_t drop;
        (void)xQueueReceive(s_req_q, &drop, 0);
        if (xQueueSend(s_req_q, &req, 0) != pdTRUE) {
            ESP_LOGW(TAG, "扫描请求入队失败");
            return;
        }
    }

    /* 立刻置为“扫描中”，界面马上就能显示出来（不用等任务被调度到） */
    wifi_set_scan_state(APP_WIFI_SCAN_RUNNING, false);
}

app_wifi_scan_state_t app_wifi_scan_state(void)
{
    wifi_lock();
    app_wifi_scan_state_t st = s_scan_state;
    wifi_unlock();
    return st;
}

uint32_t app_wifi_scan_version(void)
{
    wifi_lock();
    uint32_t v = s_scan_version;
    wifi_unlock();
    return v;
}

int app_wifi_scan_snapshot(app_wifi_ap_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }

    wifi_lock();
    int n = (s_scan_count < max) ? s_scan_count : max;
    memcpy(out, s_scan_list, (size_t)n * sizeof(app_wifi_ap_t));
    wifi_unlock();
    return n;
}

/* ============================ 连接状态 ============================ */

static void wifi_set_conn_state(app_wifi_connect_state_t st, const char *msg,
                                const char *ssid, bool bump)
{
    wifi_lock();
    s_conn_state = st;
    if (msg != NULL) {
        strncpy(s_conn_msg, msg, sizeof(s_conn_msg) - 1);
        s_conn_msg[sizeof(s_conn_msg) - 1] = '\0';
    } else {
        s_conn_msg[0] = '\0';
    }
    if (ssid != NULL) {
        strncpy(s_conn_ssid, ssid, sizeof(s_conn_ssid) - 1);
        s_conn_ssid[sizeof(s_conn_ssid) - 1] = '\0';
    }
    if (bump) {
        s_conn_version++;
    }
    wifi_unlock();
}

app_wifi_connect_state_t app_wifi_connect_state(void)
{
    wifi_lock();
    app_wifi_connect_state_t st = s_conn_state;
    wifi_unlock();
    return st;
}

uint32_t app_wifi_connect_version(void)
{
    wifi_lock();
    uint32_t v = s_conn_version;
    wifi_unlock();
    return v;
}

const char *app_wifi_connect_message(void)
{
    return s_conn_msg;
}

const char *app_wifi_connect_ssid(void)
{
    return s_conn_ssid;
}

esp_err_t app_wifi_connect_request(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0' ||
        strlen(ssid) > APP_WIFI_SSID_MAX_LEN) {
        ESP_LOGW(TAG, "连接请求的 SSID 非法");
        return ESP_ERR_INVALID_ARG;
    }
    if (password != NULL && strlen(password) > APP_WIFI_PASS_MAX_LEN) {
        ESP_LOGW(TAG, "连接请求的密码太长");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_req_q == NULL) {
        ESP_LOGW(TAG, "请求队列还没建好，忽略这次连接请求");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_req_t req;
    memset(&req, 0, sizeof(req));
    req.type = WIFI_REQ_CONNECT;
    strncpy(req.ssid, ssid, sizeof(req.ssid) - 1);
    if (password != NULL) {
        strncpy(req.password, password, sizeof(req.password) - 1);
    }

    if (xQueueSend(s_req_q, &req, 0) != pdTRUE) {
        wifi_req_t drop;
        (void)xQueueReceive(s_req_q, &drop, 0);
        if (xQueueSend(s_req_q, &req, 0) != pdTRUE) {
            ESP_LOGW(TAG, "连接请求入队失败");
            return ESP_ERR_TIMEOUT;
        }
    }

    wifi_set_conn_state(APP_WIFI_CONNECT_RUNNING, "正在连接...", ssid, true);
    ESP_LOGI(TAG, "界面请求连接「%s」（密码 %u 字节）", ssid,
             (unsigned)strlen(password ? password : ""));
    return ESP_OK;
}

/**
 * @brief  执行一条连接请求（只有 app_wifi_task 会调用）
 */
static void wifi_do_connect_request(const wifi_req_t *req)
{
    wifi_post_status("WiFi连接中");

    esp_err_t err = net_connect(req->ssid,
                               (req->password[0] != '\0') ? req->password : NULL);
    if (err == ESP_OK) {
        /* 需求：连接成功的 WiFi 自动记到 NVS（同时刷新“最近使用”序号） */
        app_wifi_store_save(req->ssid, req->password);
        wifi_set_conn_state(APP_WIFI_CONNECT_OK, "连接成功", req->ssid, true);
        wifi_refresh_status(true);
        return;
    }

    char msg[96];
    if (err == ESP_FAIL) {
        snprintf(msg, sizeof(msg), "连接失败（密码错误或热点不可达）");
    } else if (err == ESP_ERR_TIMEOUT) {
        snprintf(msg, sizeof(msg), "连接超时（热点太远/信号太弱？）");
    } else {
        snprintf(msg, sizeof(msg), "连接失败（%s）", esp_err_to_name(err));
    }

    ESP_LOGW(TAG, "连接「%s」失败：%s", req->ssid, msg);
    wifi_set_conn_state(APP_WIFI_CONNECT_FAIL, msg, req->ssid, true);
    wifi_refresh_status(false);
}

/* ============================ 已知网络自动连接 ============================ */

/**
 * @brief  界面上有没有排队等着处理的请求
 *
 * @note   自动连接（扫一轮 + 最多试 3 个 AP）最坏能占住本任务几十秒。
 *         如果这段时间用户点了界面上的“扫描/连接”，等待体验会非常糟
 *         （界面显示“扫描中...”半天不动）。所以自动连接每走一步都先看一眼队列，
 *         有用户请求就让路 —— 用户的操作永远优先于后台自动连接。
 */
static bool wifi_ui_request_pending(void)
{
    return (s_req_q != NULL) && (uxQueueMessagesWaiting(s_req_q) > 0);
}

/**
 * @brief  试连一个候选网络，成功返回 true
 *
 * @note   成功后同样会写 NVS —— 包括“编译期配置的那个 SSID”，
 *         这样以后就算把配置删了，设备照样记得这个网络。
 */
static bool wifi_try_candidate(const wifi_cand_t *c)
{
    ESP_LOGI(TAG, "尝试连接「%s」（信号 %d dBm%s）", c->ssid, (int)c->rssi,
             c->from_config ? "，来自编译期配置" : "，来自 NVS 记录");
    wifi_post_status("WiFi连接中");

    esp_err_t err = net_connect(c->ssid, (c->password[0] != '\0') ? c->password : NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "连接「%s」失败：%s", c->ssid, esp_err_to_name(err));
        return false;
    }

    app_wifi_store_save(c->ssid, c->password);
    wifi_refresh_status(true);
    return true;
}

/**
 * @brief  扫一遍附近，把“记过的网络”挨个试一遍
 *
 * @return true 连上了；false 都没连上（或压根没有候选）
 *
 * @note   候选来源：编译期配置的 SSID + NVS 里存的所有记录。
 *         先扫一次的好处：能判断哪个候选就在附近（并按信号强弱排顺序），
 *         不用对着一个不存在的 AP 干等 15 秒超时。
 */
static bool wifi_try_known_aps(void)
{
    if (net_is_connected()) {
        return true;
    }

    /* ---------- 1. 收集候选 ---------- */
    int n = 0;

    if (wifi_config_ssid_valid()) {
        memset(&s_cands[n], 0, sizeof(s_cands[n]));
        strncpy(s_cands[n].ssid, APP_WIFI_SSID, sizeof(s_cands[n].ssid) - 1);
        strncpy(s_cands[n].password, APP_WIFI_PASSWORD, sizeof(s_cands[n].password) - 1);
        s_cands[n].rssi        = WIFI_RSSI_UNSEEN;
        s_cands[n].from_config = true;
        n++;
    }

    int cn = app_wifi_store_list(s_creds, APP_WIFI_STORE_MAX);
    for (int i = 0; i < cn && n < (APP_WIFI_STORE_MAX + 1); i++) {
        if (s_creds[i].ssid[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (int k = 0; k < n; k++) {
            if (strcmp(s_cands[k].ssid, s_creds[i].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        memset(&s_cands[n], 0, sizeof(s_cands[n]));
        strncpy(s_cands[n].ssid, s_creds[i].ssid, sizeof(s_cands[n].ssid) - 1);
        strncpy(s_cands[n].password, s_creds[i].password, sizeof(s_cands[n].password) - 1);
        s_cands[n].rssi        = WIFI_RSSI_UNSEEN;
        s_cands[n].from_config = false;
        n++;
    }

    if (n == 0) {
        ESP_LOGW(TAG, "还没有任何 WiFi 记录（也没配置 SSID），请在界面上点 WiFi 按钮选一个");
        return false;
    }

    /* ---------- 2. 扫一次，标出哪些候选就在附近 ---------- */
    if (wifi_ui_request_pending()) {
        ESP_LOGI(TAG, "界面有请求在排队，先不做自动扫描");
        return false;
    }
    int sn = wifi_do_scan(false);
    if (sn > 0) {
        for (int i = 0; i < n; i++) {
            for (int k = 0; k < sn; k++) {
                if (s_ap_raw[k].ssid[0] == '\0') {
                    continue;
                }
                if (strcmp(s_ap_raw[k].ssid, s_cands[i].ssid) == 0 &&
                    s_ap_raw[k].rssi > s_cands[i].rssi) {
                    s_cands[i].rssi = s_ap_raw[k].rssi;
                }
            }
        }
    }

    /* ---------- 3. 排序：扫到的按信号从强到弱排前面，没扫到的垫底 ---------- */
    for (int i = 1; i < n; i++) {
        wifi_cand_t key = s_cands[i];
        int         j   = i - 1;
        while (j >= 0 && s_cands[j].rssi < key.rssi) {
            s_cands[j + 1] = s_cands[j];
            j--;
        }
        s_cands[j + 1] = key;
    }

    int seen = 0;
    for (int i = 0; i < n; i++) {
        if (s_cands[i].rssi != WIFI_RSSI_UNSEEN) {
            seen++;
        }
    }
    ESP_LOGI(TAG, "已知网络 %d 个，其中 %d 个就在附近，开始尝试（最多试 %d 个）",
             n, seen, APP_WIFI_TRY_MAX);

    /* ---------- 4. 挨个试 ---------- */
    int tried = 0;
    for (int i = 0; i < n && tried < APP_WIFI_TRY_MAX; i++) {
        if (net_is_connected()) {
            return true;
        }
        if (wifi_ui_request_pending()) {
            ESP_LOGI(TAG, "界面有请求在排队，自动连接让路（已试 %d 个）", tried);
            break;
        }
        tried++;
        if (wifi_try_candidate(&s_cands[i])) {
            return true;
        }
    }

    return net_is_connected();
}

/* ============================ 生命周期 ============================ */

esp_err_t app_wifi_init(void)
{
    /* 请求队列/锁必须在界面能用之前就建好（app_start 里先调一次本函数） */
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            ESP_LOGW(TAG, "创建互斥锁失败（状态读写不加锁）");
        }
    }
    if (s_req_q == NULL) {
        s_req_q = xQueueCreate(APP_WIFI_REQ_QUEUE_LEN, sizeof(wifi_req_t));
        if (s_req_q == NULL) {
            ESP_LOGE(TAG, "创建 WiFi 请求队列失败（界面无法触发扫描/连接）");
        }
    }

    /* NVS 里的 WiFi 名单：只读 flash，不碰射频，先加载好 */
    app_wifi_store_init();

    return net_init();
}

bool app_wifi_is_connected(void)
{
    return net_is_connected();
}

const char *app_wifi_ip(void)
{
    return s_ip;
}

bool app_wifi_wait_connected(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if (net_is_connected()) {
            wifi_refresh_ip();
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
    return net_is_connected();
}

void app_wifi_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "WiFi 任务启动（Core %d），编译期 SSID = %s",
             xPortGetCoreID(), wifi_config_ssid_valid() ? APP_WIFI_SSID : "（没配置）");

    /* 界面文字按“精简版”来：未连接 = WiFi未连接；已连接 = WiFi 强/中/弱 */
    wifi_post_status("WiFi未连接");

    /* 1. 初始化 STA（+ 请求队列/NVS 名单；幂等，app_start 里可能已经调过一次） */
    esp_err_t err = app_wifi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 初始化失败: %s", esp_err_to_name(err));
        wifi_post_status("WiFi初始化失败");
        vTaskDelete(NULL);
        return;
    }
    net_set_auto_reconnect(true);

    ESP_LOGI(TAG, "NVS 里记着 %d 个 WiFi（上限 %d 个，存满覆盖最久没用过的）",
             app_wifi_store_count(), APP_WIFI_STORE_MAX);
    app_wifi_store_dump();

    /*
     * 2. 开机首次连接。
     *
     * 踩过的坑：这里原来是个“没连上就 net_connect() → 失败就等 5 秒再来”的循环，
     *   而 bsp_wifi 内部也有一个自动重连任务（bsp_wifi_reconnect_task，
     *   每 3 秒试一次、试 3 次放弃），两边同时调 esp_wifi_connect() 就会互相打架，
     *   日志里出现 “wifi:sta is connected, disconnect before connecting to new ap”，
     *   结果是把已经连上的链路又断开重来，永远连不稳。
     *
     * 现在：先连编译期配置的那个 SSID（如果有）；连不上就等底层重连任务折腾一会儿，
     *   还是不行才走“扫一遍已知网络”的兜底路径（这条路也会被主循环定期触发）。
     */
    if (wifi_config_ssid_valid()) {
        err = net_connect(APP_WIFI_SSID, APP_WIFI_PASSWORD);
        if (err == ESP_OK) {
            /* 编译期配置的网络也记进 NVS：以后就算改了配置，设备仍认这个网络 */
            app_wifi_store_save(APP_WIFI_SSID, APP_WIFI_PASSWORD);
            wifi_refresh_status(true);
        } else {
            ESP_LOGW(TAG, "首次连接 %s 失败(%s)，先交给底层自动重连", APP_WIFI_SSID,
                     esp_err_to_name(err));
            wifi_refresh_status(false);
        }
    } else {
        ESP_LOGW(TAG, "编译期没有配置 SSID（app_config_secret.h），改用 NVS 里记过的网络");
    }

    if (!net_is_connected()) {
        app_wifi_wait_connected(APP_WIFI_RECONNECT_INTERVAL_MS);
    }
    if (!net_is_connected()) {
        ESP_LOGW(TAG, "开机还没连上，扫一遍附近的已知网络试试");
        wifi_try_known_aps();
    }

    /*
     * 3. 主循环：既“看状态 + 刷信号档位”，也处理界面发来的扫描/连接请求。
     *
     *    这个任务本身就是独立的（Core 0，优先级 5），和 LVGL 任务（Core 1）分开，
     *    所以这里的 net_is_connected()/net_scan()/net_connect() 绝不会卡住界面渲染；
     *    而状态文字没变化时 app_bus 里不会产生新消息，界面也不会白重绘。
     *
     *    用 xQueueReceive(…, 2.5 秒超时) 代替 vTaskDelay：平时就是“2.5 秒刷一次状态”，
     *    用户点扫描/连接时又能立刻响应（不用等这次延时走完）。
     */
    uint32_t offline_ms = 0;
    for (;;) {
        wifi_req_t req;
        bool got_req = (s_req_q != NULL) &&
                       (xQueueReceive(s_req_q, &req,
                                      pdMS_TO_TICKS(APP_WIFI_STATUS_REFRESH_MS)) == pdTRUE);

        if (got_req) {
            if (req.type == WIFI_REQ_SCAN) {
                wifi_do_scan(true);
            } else if (req.type == WIFI_REQ_CONNECT) {
                wifi_do_connect_request(&req);
                offline_ms = 0;             /* 用户刚操作过，自动连接的倒计时重新开始 */
            }
        }

        bool connected = net_is_connected();
        wifi_refresh_status(connected);

        if (connected) {
            offline_ms = 0;
        } else {
            if (!got_req) {
                /* 处理请求时没有等满一个周期，就不计时（否则会越攒越快） */
                offline_ms += APP_WIFI_STATUS_REFRESH_MS;
            }

            /* 掉线很久（底层自动重连可能已经用尽次数）才去扫一遍已知网络，且限频 */
            if (offline_ms >= APP_WIFI_KNOWN_SCAN_INTERVAL_MS) {
                offline_ms = 0;
                ESP_LOGW(TAG, "掉线超过 %d 秒，扫一遍附近的已知网络并尝试连接",
                         APP_WIFI_KNOWN_SCAN_INTERVAL_MS / 1000);
                wifi_try_known_aps();
            }
        }
    }
}
