/**
 * @file    app_settings.h
 * @brief   用户设置持久化：目前只有“屏幕背光亮度”，存在 TF 卡的 FATFS 上
 *
 * 分层：app 层。TF 卡文件系统由 my_drivers/sdcard 挂载（挂载点 /sd），
 *       背光由 my_drivers/lcd 的 lcd_brightness_set() 控制，本模块只做“存取 + 应用”。
 *
 * 存储格式（`/sd/chat_settings.txt`，纯文本，方便手改/排查）：
 *
 *     ESP32TALK_CFG 1
 *     # 背光亮度，0~100
 *     backlight=60
 *
 * 行为约定：
 *   - 读不到文件 / TF 卡没插 / 格式不对 -> 一律用 APP_LCD_BRIGHTNESS_DEFAULT（默认 50%）；
 *   - 写的时候先写临时文件再 rename，避免写一半掉电把设置写坏；
 *   - 写失败（没插卡）不会报错弹窗，只在日志里提示“本次运行有效”——
 *     用户可以正常调亮度，只是重启后回到默认值。
 */

#ifndef __APP_SETTINGS_H
#define __APP_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 生命周期 ============================ */

/**
 * @brief  初始化：必要时重试挂载 TF 卡 → 读取设置 → **立即应用背光**
 *
 * @return ESP_OK 成功（不管有没有读到文件，能应用默认值也算成功）；
 *         ESP_ERR_INVALID_STATE 背光驱动还没初始化
 *
 * @note   在 app_start() 里尽早调用（界面创建之前），
 *         这样开机就是用户上次的亮度，不会先亮一下 100% 再变暗。
 * @note   TF 卡由 board_init() 里已经挂载过一次；这里再试一次是为了
 *         “开机时没插卡、后来插上了”这种情况也能读到设置。
 */
esp_err_t app_settings_init(void);

/* ============================ 背光 ============================ */

/**
 * @brief  当前背光亮度（0~100，已经过下限保护）
 * @note   未初始化时返回 APP_LCD_BRIGHTNESS_DEFAULT
 */
int app_settings_get_backlight(void);

/**
 * @brief  设置背光亮度：立刻生效，并按需持久化
 *
 * @param  percent 0~100（低于 APP_LCD_BRIGHTNESS_MIN 会被抬到下限）
 * @param  persist true = 写入 TF 卡（写失败只记日志）
 * @return ESP_OK 亮度已生效；写盘失败时返回写盘错误码（亮度**仍然已生效**）
 *
 * @note   滑条拖动过程中建议 persist=false（只在【确认】时存一次），
 *         不然会一直写卡，既慢又费卡。
 */
esp_err_t app_settings_set_backlight(int percent, bool persist);

/**
 * @brief  把当前亮度写回 TF 卡（等价于 app_settings_set_backlight(当前值, true)）
 */
esp_err_t app_settings_save(void);

/* ============================ 自动休眠 ============================ */

/**
 * @brief  当前“长时间静止后进入省电模式”的超时（毫秒）
 *
 * @return APP_POWER_TIMEOUT_30S / APP_POWER_TIMEOUT_60S / APP_POWER_TIMEOUT_NEVER(0)
 *
 * @note   这个值只存在内部 NVS（键 sleep_ms），不进 TF 卡上的亮度设置文件：
 *         亮度的文件格式已经稳定，不再动它，避免“手改过设置文件的人”升级后
 *         被写成两种格式。
 */
uint32_t app_settings_get_sleep_timeout_ms(void);

/**
 * @brief  设置自动休眠超时并持久化到 NVS
 *
 * @param  timeout_ms APP_POWER_TIMEOUT_* 之一（其它值会被归一到三个档位）
 * @return ESP_OK 已保存；NVS 打不开/写失败时返回错误码（内存里的值仍然已更新）
 */
esp_err_t app_settings_set_sleep_timeout_ms(uint32_t timeout_ms);

/* ============================ IMU 唤醒灵敏度 ============================ */

/**
 * @brief  当前 IMU 唤醒灵敏度档位
 *
 * @return APP_POWER_IMU_SENS_WEAK(0) / MED(1) / STRONG(2)；默认中档
 *
 * @note   和休眠超时一样只存内部 NVS（键 imu_sens），不进 TF 卡设置文件。
 */
uint8_t app_settings_get_imu_sensitivity(void);

/**
 * @brief  设置 IMU 唤醒灵敏度并持久化到 NVS
 *
 * @param  level 0 = 弱，1 = 中，2 = 强；越界值收敛到中档（1）
 * @return ESP_OK 已保存；NVS 打不开/写失败时返回错误码（内存里的值仍然已更新）
 */
esp_err_t app_settings_set_imu_sensitivity(uint8_t level);

/* ============================ 其它 ============================ */

/** TF 卡当前是否可用（挂载成功） */
bool app_settings_sd_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_SETTINGS_H */
