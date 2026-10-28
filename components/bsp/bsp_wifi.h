/**
 * @file    bsp_wifi.h
 * @brief   ESP32-S3 板级 WiFi（STA 模式）驱动
 *
 * 逻辑参考旧版 BSP（esp32_s3_szp / IDF v5.1.4），并按 IDF v5.5.5 的
 * esp_wifi / esp_event / esp_netif API 重写。
 *
 * 典型用法：
 *      bsp_wifi_init();                       // 1. 初始化（只需一次）
 *      bsp_wifi_ap_info_t aps[20];
 *      int n = bsp_wifi_scan(aps, 20);        // 2. 扫描
 *      bsp_wifi_connect("MyAP", "12345678");  // 3. 连接
 */

#ifndef __BSP_WIFI_H
#define __BSP_WIFI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 配置项 ============================ */

/** 扫描结果结构体（对上层屏蔽 wifi_ap_record_t 的细节） */
typedef struct {
    char             ssid[33];      /*!< AP 名称（最大 32 字节 + '\0'） */
    uint8_t          bssid[6];      /*!< AP MAC 地址 */
    int8_t           rssi;          /*!< 信号强度 dBm（越接近 0 越强） */
    uint8_t          channel;       /*!< 信道 */
    wifi_auth_mode_t authmode;      /*!< 加密方式 */
} bsp_wifi_ap_info_t;

/** 单次扫描能返回的最大 AP 数量（用户缓冲区不够大时按这个数分配临时内存） */
#define BSP_WIFI_SCAN_MAX_AP        20

/** 连接 AP 的默认超时（毫秒），可用 bsp_wifi_set_connect_timeout() 修改 */
#define BSP_WIFI_CONNECT_TIMEOUT_MS 15000

/* ============================ 初始化/反初始化 ============================ */

/**
 * @brief  初始化 WiFi（STA 模式）
 *
 * 内部完成：esp_netif_init -> esp_event_loop_create_default ->
 *          创建默认 STA netif -> 注册事件回调 -> esp_wifi_init ->
 *          设置 STA 模式/国家码/省电模式 -> esp_wifi_start
 *
 * @note   可重复调用，重复调用直接返回 ESP_OK。
 * @return ESP_OK 成功；其他为 esp_err_t 错误码
 */
esp_err_t bsp_wifi_init(void);

/**
 * @brief  反初始化 WiFi（断开、停止、释放 netif 与事件回调）
 */
esp_err_t bsp_wifi_deinit(void);

/**
 * @brief  WiFi 是否已经初始化
 */
bool bsp_wifi_is_initialized(void);

/* ============================ 连接 / 断开 ============================ */

/**
 * @brief  连接指定 WiFi
 *
 * @param  ssid      AP 名称，不能为 NULL 或空串
 * @param  password  密码；开放网络传 NULL 或 ""
 * @return ESP_OK 已连接并拿到 IP；
 *         ESP_ERR_INVALID_ARG 参数错误；
 *         ESP_ERR_INVALID_STATE 未初始化；
 *         ESP_ERR_TIMEOUT 超时未连上（密码错误时会较快返回 ESP_FAIL）
 */
esp_err_t bsp_wifi_connect(const char *ssid, const char *password);

/**
 * @brief  连接指定 WiFi，失败自动重试
 *
 * @param  ssid         AP 名称
 * @param  password     密码
 * @param  max_retry    最大尝试次数（>=1）
 */
esp_err_t bsp_wifi_connect_with_retry(const char *ssid, const char *password, int max_retry);

/**
 * @brief  断开当前连接
 */
esp_err_t bsp_wifi_disconnect(void);

/**
 * @brief  是否已连接上 AP 且拿到 IP
 */
bool bsp_wifi_is_connected(void);

/**
 * @brief  设置断线自动重连（默认开启）
 */
void bsp_wifi_set_auto_reconnect(bool enable);

/**
 * @brief  切换 WiFi 省电等级（自动休眠省电时用）
 *
 * @param  enable true  = 最大省电（WIFI_PS_MAX_MODEM，只在 DTIM 醒来听信标，
 *                        保持连接但延迟变大）
 *                false = 默认省电（WIFI_PS_MIN_MODEM，和开机时一致）
 * @return ESP_OK 已生效；ESP_ERR_INVALID_STATE WiFi 还没初始化
 *
 * @note   不会断开连接：STA 仍然保持关联，只是收发变“慢一点”。
 *         唤醒后要记得调回 false，否则语音上传会明显变慢。
 */
esp_err_t bsp_wifi_set_power_save(bool enable);

/**
 * @brief  设置连接超时时间
 */
void bsp_wifi_set_connect_timeout(uint32_t timeout_ms);

/* ============================ 扫描 ============================ */

/**
 * @brief  扫描周围的 WiFi
 *
 * @param  buffer     结果缓冲区，由调用者提供
 * @param  max_count  缓冲区能容纳的最大条目数（<=0 时按 BSP_WIFI_SCAN_MAX_AP 处理）
 * @return >=0 实际写入的 AP 数量；<0 表示失败
 *         -1 通用失败 / -2 参数错误 / -3 未初始化
 */
int bsp_wifi_scan(bsp_wifi_ap_info_t *buffer, uint16_t max_count);

/* ============================ 状态查询 ============================ */

/**
 * @brief  获取当前连接的 AP 信息
 * @param  info 输出参数，可为 NULL（仅做连接判断）
 * @return ESP_OK 已连接；ESP_FAIL 未连接
 */
esp_err_t bsp_wifi_get_ap_info(bsp_wifi_ap_info_t *info);

/**
 * @brief  获取当前信号强度
 * @return dBm（0 表示未连接）
 */
int8_t bsp_wifi_get_rssi(void);

/**
 * @brief  获取当前 IP 字符串，例如 "192.168.1.100"
 * @param  ip_str 输出缓冲
 * @param  len    缓冲长度（建议 >= 16）
 * @return ESP_OK 成功；ESP_FAIL 未获取到 IP
 */
esp_err_t bsp_wifi_get_ip(char *ip_str, size_t len);

/**
 * @brief  获取当前连接的 SSID（未连接返回空串）
 */
const char *bsp_wifi_get_ssid(void);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_WIFI_H */
