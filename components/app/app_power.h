/**
 * @file    app_power.h
 * @brief   低功耗 / 自动休眠：IMU + 触摸静止检测
 *
 * 分工：
 *   - 这个模块只负责“判断 + 开关省电”，不碰界面内容、不改业务流程；
 *   - 界面（设置二级页）通过 app_power_set_timeout_ms() 改超时；
 *   - 其它模块（比如打开设置页、开始录音）可以调 app_power_notify_activity()
 *     表示“用户还在操作”，让计时重新开始。
 *
 * 软省电模式里做了什么（见 app_power.c 的 power_enter_soft_sleep()）：
 *   背光降到最低（APP_LCD_BRIGHTNESS_MIN）+ WiFi 切最大省电档（保持连接）
 *   + 屏蔽录音键（省电期间按 KEY 只当唤醒，不录音）。
 *   **不进 light sleep、不关面板、不拆 SPI 总线、不暂停 LVGL** —— 画面完全不受影响。
 *   触摸 / IMU 超阈值 / 按键 任一命中就恢复：背光回用户亮度、解禁录音键、
 *   WiFi 回默认功耗档。
 *
 * 为什么不做 light sleep：ESP32-S3 没有 SPI retention，这块板的 ST7789 在
 * light sleep 之后不再响应任何命令（连 SLPIN/SLPOUT/DISPON 都没反应），
 * 实机已判定该路线走不通，故改为“软省电”。
 */

#ifndef __APP_POWER_H
#define __APP_POWER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化低功耗模块（读持久化的休眠超时、重置计时）
 *
 * @note   要在 app_settings_init() 之后调用（否则读不到用户设置）。
 * @return ESP_OK
 */
esp_err_t app_power_init(void);

/**
 * @brief  低功耗任务：正常采样 -> 静止超时后进入省电 -> 等唤醒 -> 恢复
 *
 * @param  arg 未使用
 */
void app_power_task(void *arg);

/**
 * @brief  告诉低功耗模块“现在有用户活动”，静止计时从现在重新开始
 *
 * @note   线程安全，可在界面任务里调用。
 */
void app_power_notify_activity(void);

/**
 * @brief  当前是否处于软省电（背光最小 + 录音键屏蔽）
 *
 * @note   给按键任务用：软省电期间按录音键只唤醒、不开始录音。
 *         只有低功耗任务会改变它，读一次即可（bool 读写是原子的）。
 */
bool app_power_is_sleeping(void);

/**
 * @brief  当前生效的自动休眠超时（毫秒，0 = 永不）
 */
uint32_t app_power_get_timeout_ms(void);

/**
 * @brief  修改自动休眠超时并持久化
 *
 * @param  timeout_ms APP_POWER_TIMEOUT_30S / APP_POWER_TIMEOUT_60S / APP_POWER_TIMEOUT_NEVER
 * @return ESP_OK 已保存；NVS 写失败时返回错误码（内存里的新值仍然生效）
 */
esp_err_t app_power_set_timeout_ms(uint32_t timeout_ms);

/**
 * @brief  当前 IMU 唤醒灵敏度档位
 *
 * @return APP_POWER_IMU_SENS_WEAK(0) / MED(1) / STRONG(2)
 */
uint8_t app_power_get_imu_sensitivity(void);

/**
 * @brief  修改 IMU 唤醒灵敏度（立即生效 + 持久化到 NVS）
 *
 * @param  level 0 = 弱，1 = 中，2 = 强；越界值收敛到中档
 * @return ESP_OK 已保存；NVS 写失败时返回错误码（内存里的新档位仍然立即生效）
 *
 * @note   只影响 IMU 判据的加速度/角速度阈值；触摸唤醒不受档位影响
 *         （任何触摸都直接唤醒）。改档位相当于一次用户操作，会重置静止计时。
 */
esp_err_t app_power_set_imu_sensitivity(uint8_t level);

#ifdef __cplusplus
}
#endif

#endif /* __APP_POWER_H */
