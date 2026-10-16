/**
 * @file    app_wifi_store.h
 * @brief   连过的 WiFi 名单（SSID + 密码）保存在内部 NVS
 *
 * 分层：app 层。只碰内部 NVS（flash 上的 nvs 分区），不碰射频、不碰界面。
 *
 * 为什么要有它：
 *   原来的工程只有一个编译期写死的 SSID（app_config_secret.h），
 *   换一个地方就没网可用了。现在界面上可以点着选附近的 WiFi、输密码连接，
 *   连上以后**自动记住**；下次开机（哪怕换了热点）自己就能连回去。
 *
 * 存储策略（需求：最多 10 个，超出时覆盖最久的）：
 *   - 每条记录有 ssid / password / seq 三个字段，seq 是**全局递增的使用序号**；
 *   - 每次“连接成功”都会把该条的 seq 刷成最新（LRU：最久没用过的 seq 最小）；
 *   - 存满 10 条又要存新的时，挑 seq 最小的那条覆盖，并在日志里写明覆盖了谁。
 *
 * NVS 键名（命名空间见 APP_WIFI_STORE_NVS_NAMESPACE，键最长 15 字节）：
 *   cnt       记录条数（u8）
 *   csq       全局序号计数器（u32，只增不减）
 *   ssid<i>   第 i 条的 SSID（字符串）
 *   pw<i>     第 i 条的密码（字符串）
 *   sq<i>     第 i 条的序号（u32）
 *
 * 线程安全：内部有一把互斥锁，任意任务都可以调用（界面会用它判断“已存”）。
 *           返回数据一律**拷贝**给调用者，不暴露内部指针。
 */

#ifndef __APP_WIFI_STORE_H
#define __APP_WIFI_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "app_config.h"         /* APP_WIFI_STORE_MAX */

#ifdef __cplusplus
extern "C" {
#endif

/** SSID / 密码的最大长度（与 IDF 的 wifi_config_t.sta 一致，不含结尾 '\0'） */
#define APP_WIFI_SSID_MAX_LEN   32
#define APP_WIFI_PASS_MAX_LEN   64

/** 一条已保存的 WiFi 记录 */
typedef struct {
    char     ssid[APP_WIFI_SSID_MAX_LEN + 1];       /*!< AP 名称 */
    char     password[APP_WIFI_PASS_MAX_LEN + 1];   /*!< 密码（开放网络是空串） */
    uint32_t seq;                                   /*!< 使用序号，越大越新 */
} app_wifi_cred_t;

/* ============================ 生命周期 ============================ */

/**
 * @brief  初始化：打开 NVS 并把名单读进内存（幂等，可重复调用）
 *
 * @return ESP_OK 成功（包括“NVS 里还没有记录”）；
 *         ESP_ERR_INVALID_STATE NVS 打不开 —— 此时**不保存**，但连接功能照常可用
 *
 * @note   在 app_wifi_init() 里调用（app_start() 中、WiFi 任务创建之前就会先跑一次）。
 */
esp_err_t app_wifi_store_init(void);

/** NVS 后端是否可用（不可用时 save 会失败，但连接仍然能连） */
bool app_wifi_store_ready(void);

/* ============================ 查询 ============================ */

/** 当前记录了几条 */
int app_wifi_store_count(void);

/**
 * @brief  拷贝一份名单快照
 *
 * @param  out 输出数组（调用者提供）
 * @param  max out 能放几条
 * @return 实际拷贝的条数
 */
int app_wifi_store_list(app_wifi_cred_t *out, int max);

/** 这个 SSID 记过没有（界面列表里给已存的网络打标记用） */
bool app_wifi_store_has(const char *ssid);

/**
 * @brief  取某个 SSID 记着的密码
 *
 * @param  ssid     要查的 AP 名称
 * @param  password 输出缓冲
 * @param  len      缓冲大小（建议 APP_WIFI_PASS_MAX_LEN + 1）
 * @return true 找到了（开放网络的密码是空串，也算找到）
 */
bool app_wifi_store_find(const char *ssid, char *password, size_t len);

/* ============================ 写入 ============================ */

/**
 * @brief  保存/更新一条记录（连接成功后调用）
 *
 * @param  ssid     AP 名称（空串会被拒绝）
 * @param  password 密码，开放网络传 NULL 或 ""
 * @return ESP_OK 已写入 NVS；其它表示没存住（日志里会说明原因）
 *
 * @note   已经存在同名记录时就地更新密码并刷新序号；
 *         不存在时：没满就追加，满了就覆盖**序号最小（最久没用过）**的那条。
 */
esp_err_t app_wifi_store_save(const char *ssid, const char *password);

/**
 * @brief  只刷新“最近使用”序号（把这条挪到最新，避免被淘汰）
 * @note   自动连接成功时用它；找不到该 SSID 时什么都不做。
 */
void app_wifi_store_touch(const char *ssid);

/** 把当前名单打到日志里（开机、以及保存后打一条，方便排查） */
void app_wifi_store_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_WIFI_STORE_H */
