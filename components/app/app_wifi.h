/**
 * @file    app_wifi.h
 * @brief   WiFi（STA）连接管理任务 + 扫描/连接请求接口
 *
 * 分层：app 层。底层 WiFi 射频属于 bsp（bsp_wifi），
 *       app 层统一通过 my_drivers 的 net_* 接口使用，不直接调 esp_wifi。
 *
 * 行为：
 *   1. app_wifi_init() 建请求队列/锁、初始化 STA、加载 NVS 里的 WiFi 名单；
 *   2. 开机先连编译期配置的 SSID（app_config_secret.h，没配就跳过），
 *      连不上就“扫一遍附近的已知网络”挨个试；
 *   3. 主循环每 APP_WIFI_STATUS_REFRESH_MS 刷新顶栏状态（连没连 + 信号档位）；
 *   4. 掉线超过 APP_WIFI_KNOWN_SCAN_INTERVAL_MS 就再扫一遍已知网络；
 *   5. 界面（app_wifi_ui）通过 app_wifi_scan_request() / app_wifi_connect_request()
 *      触发扫描与连接 —— 所有射频操作都发生在本任务里，界面只读结果。
 *
 * 线程模型：
 *   - **扫描/连接只能在 app_wifi_task 里做**（net_scan / net_connect 都是阻塞的，
 *     放在 LVGL 任务里会把界面卡住好几秒）；
 *   - 界面把请求丢进队列，然后轮询 app_wifi_scan_state()/app_wifi_connect_state()
 *     以及对应的 version 计数器（版本号变了 = 有新结果）。
 */

#ifndef __APP_WIFI_H
#define __APP_WIFI_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 扫描结果 ============================ */

/** 扫描到的一个 AP（给界面用的精简版） */
typedef struct {
    char   ssid[33];        /*!< AP 名称（隐藏 SSID 的不会出现在列表里） */
    int8_t rssi;            /*!< 信号强度 dBm（越接近 0 越强） */
    bool   secure;          /*!< true = 需要密码 */
} app_wifi_ap_t;

/** 扫描状态（界面据此显示“扫描中.../没找到 WiFi/扫描失败”） */
typedef enum {
    APP_WIFI_SCAN_IDLE = 0,     /*!< 没有在扫、也没有新结果 */
    APP_WIFI_SCAN_RUNNING,      /*!< 正在扫描 */
    APP_WIFI_SCAN_DONE,         /*!< 扫完了，结果在缓存里 */
    APP_WIFI_SCAN_FAILED,       /*!< 扫描失败 */
} app_wifi_scan_state_t;

/** 连接状态（界面弹窗据此显示进度/结果） */
typedef enum {
    APP_WIFI_CONNECT_IDLE = 0,  /*!< 没有连接请求 */
    APP_WIFI_CONNECT_RUNNING,   /*!< 正在连接 */
    APP_WIFI_CONNECT_OK,        /*!< 连上了（界面可以关掉弹窗） */
    APP_WIFI_CONNECT_FAIL,      /*!< 失败（说明文字见 app_wifi_connect_message） */
} app_wifi_connect_state_t;

/* ============================ 生命周期 ============================ */

/**
 * @brief  初始化：建请求队列与互斥锁、加载 NVS 里的 WiFi 名单、初始化 STA
 * @return ESP_OK 成功；其它为 net_init（bsp_wifi）的错误码
 * @note   幂等，可以重复调用。app_start() 里会先调一次（这样界面一建好就能用
 *         “扫描/连接”接口），app_wifi_task 里再调一次也无副作用。
 */
esp_err_t app_wifi_init(void);

/**
 * @brief  WiFi 管理任务（Core 0，优先级 5）
 * @param  arg 未使用
 * @note   由 app_start() 用 xTaskCreatePinnedToCore() 创建
 */
void app_wifi_task(void *arg);

/** 当前是否已连接并拿到 IP */
bool app_wifi_is_connected(void);

/**
 * @brief  等待联网（在其它任务里调用，用于发 HTTP 请求前等 WiFi）
 *
 * @param  timeout_ms 最长等待时间
 * @return true 已连接；false 超时仍未连接
 */
bool app_wifi_wait_connected(uint32_t timeout_ms);

/** 当前 IP 字符串（未连接返回 "0.0.0.0"） */
const char *app_wifi_ip(void);

/* ============================ 扫描（界面用） ============================ */

/**
 * @brief  请求扫一次附近的 WiFi（异步：丢进队列，由 WiFi 任务执行）
 * @note   任意任务可调用；重复请求只会排队，不会同时开多次扫描。
 */
void app_wifi_scan_request(void);

/** 当前扫描状态 */
app_wifi_scan_state_t app_wifi_scan_state(void);

/**
 * @brief  扫描结果版本号：每完成一次扫描 +1
 * @note   界面靠它判断“有没有新结果”（比轮询状态更不容易漏）。
 */
uint32_t app_wifi_scan_version(void);

/**
 * @brief  拷贝一份扫描结果（去重 + 按信号从强到弱排序）
 *
 * @param  out 输出数组（调用者提供）
 * @param  max out 能放几个
 * @return 实际拷贝的条数（0 = 还没有结果）
 */
int app_wifi_scan_snapshot(app_wifi_ap_t *out, int max);

/* ============================ 连接（界面用） ============================ */

/**
 * @brief  请求连接指定 AP（异步，由 WiFi 任务执行）
 *
 * @param  ssid     AP 名称
 * @param  password 密码；开放网络传 NULL 或 ""
 * @return ESP_OK 请求已排队；ESP_ERR_INVALID_ARG 参数非法；ESP_ERR_INVALID_STATE 队列没建好
 *
 * @note   连接成功后会**自动把账号密码存进 NVS**（见 app_wifi_store）。
 */
esp_err_t app_wifi_connect_request(const char *ssid, const char *password);

/** 当前连接请求的状态 */
app_wifi_connect_state_t app_wifi_connect_state(void);

/** 连接状态的版本号：每次状态变化 +1 */
uint32_t app_wifi_connect_version(void);

/** 连接状态的中文说明（例如「连接成功」「连接失败（密码错误或热点不可达）」） */
const char *app_wifi_connect_message(void);

/** 最近一次连接请求的 SSID（没有则空串） */
const char *app_wifi_connect_ssid(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_WIFI_H */
