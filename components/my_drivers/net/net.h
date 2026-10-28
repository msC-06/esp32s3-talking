/**
 * @file    net.h
 * @brief   网络服务：给 app/main 用的高层 WiFi 接口
 *
 * 分层：`bsp/bsp_wifi` 是“片上外设”层（ESP32-S3 的 WiFi 射频，只操作 ESP-IDF 原生 API）；
 *       app/main 不允许直接调用 bsp，所以这里做一层薄封装，
 *       上层只认 net_* 接口（扫描/连接/状态），不关心底层用的是哪套 wifi API。
 *
 * 典型用法：
 *      net_init();
 *      net_ap_info_t aps[NET_SCAN_MAX_AP];
 *      int n = net_scan(aps, NET_SCAN_MAX_AP);
 *      net_connect_with_retry("MyAP", "12345678", 3);
 */

#ifndef __NET_H
#define __NET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 单次扫描最多返回多少个 AP（也是推荐给调用者的缓冲区大小） */
#define NET_SCAN_MAX_AP         20

/** 连接超时（毫秒） */
#define NET_CONNECT_TIMEOUT_MS  15000

/** 扫描到的 AP 信息（对上层屏蔽底层结构体） */
typedef struct {
    char             ssid[33];      /*!< AP 名称 */
    uint8_t          bssid[6];      /*!< AP MAC */
    int8_t           rssi;          /*!< 信号强度 dBm */
    uint8_t          channel;       /*!< 信道 */
    wifi_auth_mode_t authmode;      /*!< 加密方式 */
} net_ap_info_t;

/* ============================ 初始化 ============================ */

/** 初始化网络（WiFi STA），可重复调用 */
esp_err_t net_init(void);

/** 反初始化 */
esp_err_t net_deinit(void);

/** 是否已初始化 */
bool net_is_initialized(void);

/* ============================ 连接/断开 ============================ */

/** 连接指定 AP（ssid + password；开放网络 password 传 NULL） */
esp_err_t net_connect(const char *ssid, const char *password);

/** 连接指定 AP，失败自动重试 max_retry 次 */
esp_err_t net_connect_with_retry(const char *ssid, const char *password, int max_retry);

/** 断开当前连接 */
esp_err_t net_disconnect(void);

/** 是否已连接并拿到 IP */
bool net_is_connected(void);

/** 是否开启断线自动重连（默认开） */
void net_set_auto_reconnect(bool enable);

/**
 * @brief  切换 WiFi 省电等级（自动休眠省电时用）
 * @param  enable true = 最大省电（保持连接，延迟变大）；false = 默认
 * @return ESP_OK 已生效；ESP_ERR_INVALID_STATE 网络还没初始化
 */
esp_err_t net_set_power_save(bool enable);

/* ============================ 扫描 ============================ */

/**
 * @brief  扫描周围 WiFi
 * @param  buffer    由调用者提供的缓冲区
 * @param  max_count 缓冲区容量（<=0 时按 NET_SCAN_MAX_AP 处理）
 * @return >=0 实际写入的 AP 数量；<0 失败
 */
int net_scan(net_ap_info_t *buffer, uint16_t max_count);

/* ============================ 状态 ============================ */

/** 获取当前 IP 字符串，例如 "192.168.1.100" */
esp_err_t net_get_ip(char *ip_str, size_t len);

/** 当前信号强度 dBm（0 = 未连接） */
int8_t net_get_rssi(void);

/** 当前连接的 SSID（未连接返回空串） */
const char *net_get_ssid(void);

#ifdef __cplusplus
}
#endif

#endif /* __NET_H */
