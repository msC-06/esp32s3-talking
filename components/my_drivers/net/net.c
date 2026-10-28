/**
 * @file    net.c
 * @brief   网络服务实现：对 bsp_wifi 的一层薄封装（含结构体转换）
 *
 * 这里只做转发与结构体转换，不加业务逻辑，方便以后替换底层实现
 * （例如换成 W5500 以太网、4G 模组）而不用改 app 代码。
 */

#include <string.h>
#include <stdlib.h>

#include "net.h"

#include "bsp/bsp_wifi.h"

#include "esp_log.h"

static const char *TAG = "net";

/* ============================ 初始化 ============================ */

esp_err_t net_init(void)
{
    return bsp_wifi_init();
}

esp_err_t net_deinit(void)
{
    return bsp_wifi_deinit();
}

bool net_is_initialized(void)
{
    return bsp_wifi_is_initialized();
}

/* ============================ 连接/断开 ============================ */

esp_err_t net_connect(const char *ssid, const char *password)
{
    return bsp_wifi_connect(ssid, password);
}

esp_err_t net_connect_with_retry(const char *ssid, const char *password, int max_retry)
{
    return bsp_wifi_connect_with_retry(ssid, password, max_retry);
}

esp_err_t net_disconnect(void)
{
    return bsp_wifi_disconnect();
}

bool net_is_connected(void)
{
    return bsp_wifi_is_connected();
}

void net_set_auto_reconnect(bool enable)
{
    bsp_wifi_set_auto_reconnect(enable);
}

esp_err_t net_set_power_save(bool enable)
{
    return bsp_wifi_set_power_save(enable);
}

/* ============================ 扫描 ============================ */

int net_scan(net_ap_info_t *buffer, uint16_t max_count)
{
    if (buffer == NULL || max_count == 0) {
        ESP_LOGE(TAG, "扫描缓冲区非法");
        return -2;
    }
    if (max_count > NET_SCAN_MAX_AP) {
        max_count = NET_SCAN_MAX_AP;
    }

    /* 底层用的是自己的结构体，这里先用临时缓冲接住再转换 */
    bsp_wifi_ap_info_t *tmp = calloc(max_count, sizeof(bsp_wifi_ap_info_t));
    if (tmp == NULL) {
        ESP_LOGE(TAG, "扫描临时缓冲分配失败");
        return -1;
    }

    int count = bsp_wifi_scan(tmp, max_count);
    if (count > 0) {
        for (int i = 0; i < count; i++) {
            memset(&buffer[i], 0, sizeof(net_ap_info_t));
            memcpy(buffer[i].ssid, tmp[i].ssid, sizeof(buffer[i].ssid) - 1);
            memcpy(buffer[i].bssid, tmp[i].bssid, sizeof(buffer[i].bssid));
            buffer[i].rssi     = tmp[i].rssi;
            buffer[i].channel  = tmp[i].channel;
            buffer[i].authmode = tmp[i].authmode;
        }
    }

    free(tmp);
    return count;
}

/* ============================ 状态 ============================ */

esp_err_t net_get_ip(char *ip_str, size_t len)
{
    return bsp_wifi_get_ip(ip_str, len);
}

int8_t net_get_rssi(void)
{
    return bsp_wifi_get_rssi();
}

const char *net_get_ssid(void)
{
    return bsp_wifi_get_ssid();
}
