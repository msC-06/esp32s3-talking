/**
 * @file    app_power.c
 * @brief   低功耗 / 自动省电：“软省电”（IMU + 触摸静止检测）
 *
 * 整体思路（对应需求：长时间静止且无触摸 -> 省电；触摸或 IMU 超阈值 -> 恢复）：
 *
 *   正常模式（每 APP_POWER_SAMPLE_MS 采一次）
 *     ├─ 触摸屏有手指 / IMU 加速度角速度超阈值 / 按键按下 -> 记一次“活动”
 *     └─ 距上次活动 >= 用户选的超时 且 当前处于空闲(IDLE、无弹窗、不在打字)
 *          -> power_enter_soft_sleep()
 *
 *   软省电模式（每 APP_POWER_SLEEP_POLL_MS 醒一次检查唤醒源）
 *     ├─ 背光关到 POWER_SLEEP_BRIGHTNESS（当前 0，只影响运行时、不写 flash）、
 *     │  WiFi 切最大省电档、录音键被屏蔽
 *     └─ 触摸 / IMU 超阈值 / 按键 任一命中 -> power_leave_soft_sleep()
 *
 * 为什么是“软省电”而不是 light sleep：
 *   实机验证过 light sleep 路线走不通 —— ESP32-S3 没有 SPI retention，
 *   而这块板的 ST7789 在 light sleep 之后连命令都不再响应；更进一步，
 *   只要走“关屏 + 拆 SPI3 总线 + 重建面板”那一套，面板就会死在无响应状态
 *   （build-6 的对照实验：全程不睡、只重建，同样画不出颜色）。
 *   因此这里只做三件对画面零风险的事：
 *     1. 背光关到 0（面板/SPI 完全不碰；值只在运行时生效，不写 flash）；
 *     2. 屏蔽录音键（省电期间按 KEY 只当唤醒）；
 *     3. WiFi 切最大省电档（保持连接，靠协议栈自己睡醒）。
 */

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "app_power.h"
#include "app_bus.h"
#include "app_config.h"
#include "app_settings.h"

#include "bsp/bsp_gpio.h"
#include "my_drivers/lcd.h"
#include "my_drivers/net/net.h"
#include "my_drivers/qmi8658.h"

#include "esp_err.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "APP_PWR";

/* ============================ 内部状态 ============================ */

/** 当前超时（毫秒，0 = 永不自动省电） */
static int32_t s_timeout_ms = (int32_t)APP_POWER_TIMEOUT_DEFAULT_MS;

/** 上一次“有活动”的时刻（毫秒，esp_timer 时基，溢出靠无符号相减处理） */
static volatile uint32_t s_last_active_ms = 0;

/** 当前是否处于软省电（背光最小 + 录音键屏蔽） */
static bool s_sleeping = false;

/** 唤醒请求闩锁：任何“用户操作”（按键/触摸/设置提交）都会置位，
 *  任务循环下一拍必定据此退出软省电。
 *
 *  为什么要闩锁：软省电时任务每 APP_POWER_SLEEP_POLL_MS 才采一次样，
 *  用户 ~100 ms 的快按很可能整个落在两次采样之间 —— 只靠“采样时恰好按着”
 *  会把这按键丢掉（屏一直暗，日志却以为已唤醒）。闩锁把事件记下来，绝不丢。 */
static volatile bool s_wake_req = false;

/** 上一帧加速度（用来判断“这一下有没有在动”） */
static struct {
    bool  valid;
    float ax, ay, az;
} s_prev_acc;

/** IMU 连续读失败计数；连续失败太多次就先停用 IMU（只靠触摸/按键） */
static int  s_imu_fail_count = 0;
static bool s_imu_ok         = true;

/** IMU 停用后下一次重试的时刻（毫秒）：I2C 偶发失败不该让 IMU 本次上电永久失效 */
static uint32_t s_imu_retry_at_ms = 0;

/** IMU 停用后的重试间隔（毫秒） */
#define POWER_IMU_RETRY_MS 5000u

/** 软省电时的背光值：0 = 完全关背光（省电最大化）。
 *
 *  **只影响运行时，不写入任何 flash**：既不进 NVS 也不进 TF 卡里的设置文件，
 *  唤醒时仍然是 app_settings_get_backlight() 里用户保存的那个亮度。
 *  想让屏幕“暗但还能看见”，把这里改成 APP_LCD_BRIGHTNESS_MIN（5）即可。 */
#define POWER_SLEEP_BRIGHTNESS 0

/** 当前 IMU 唤醒灵敏度档位（0/1/2，决定 power_imu_moved() 用的阈值） */
static uint8_t s_imu_sens = APP_POWER_IMU_SENS_DEFAULT;

static uint32_t power_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ============================ IMU 运动检测 ============================ */

/** 档位名字（只用于日志） */
static const char *power_imu_sens_name(uint8_t level)
{
    switch (level) {
    case APP_POWER_IMU_SENS_WEAK:
        return "弱";
    case APP_POWER_IMU_SENS_STRONG:
        return "强";
    default:
        return "中";
    }
}

/** 当前档位的加速度阈值（g） */
static float power_imu_acc_threshold(void)
{
    switch (s_imu_sens) {
    case APP_POWER_IMU_SENS_WEAK:
        return APP_POWER_IMU_ACC_G_WEAK;
    case APP_POWER_IMU_SENS_STRONG:
        return APP_POWER_IMU_ACC_G_STRONG;
    default:
        return APP_POWER_IMU_ACC_G_MED;
    }
}

/** 当前档位的角速度阈值（°/s，三轴绝对值之和） */
static float power_imu_gyro_threshold(void)
{
    switch (s_imu_sens) {
    case APP_POWER_IMU_SENS_WEAK:
        return APP_POWER_IMU_GYRO_DPS_WEAK;
    case APP_POWER_IMU_SENS_STRONG:
        return APP_POWER_IMU_GYRO_DPS_STRONG;
    default:
        return APP_POWER_IMU_GYRO_DPS_MED;
    }
}

/**
 * @brief  读一次 IMU，判断“是不是在动”
 *
 * 判据（任一满足即算在动）：
 *   1. 相邻两次采样的加速度差（L1）超过当前档位的加速度阈值；
 *   2. 三轴角速度绝对值之和超过当前档位的角速度阈值（转/倾斜）。
 *
 * 阈值随设置页的“IMU 唤醒阈值”档位变化：弱 / 中 / 强（见 app_config.h
 * 的 APP_POWER_IMU_ACC_G_* 与 APP_POWER_IMU_GYRO_DPS_*）。触摸唤醒不在这里，
 * 任何触摸都直接唤醒，不受档位影响。
 *
 * 为什么用“和上一帧比”而不是“和长期基线比”：
 *   基线法在设备换个静止姿态后会一直判定“在动”（新姿态和旧基线差得远），
 *   必须等基线慢慢追上去；差值法对“拿起来/翻动/敲一下”这种动作最灵敏，
 *   静止时读数抖动又很小，不会误唤醒。
 */
static bool power_imu_moved(void)
{
    uint32_t now = power_now_ms();

    /* IMU 之前连续读失败：先停用；每 POWER_IMU_RETRY_MS 低频重试一次，
     * 成功就自动恢复（不用重启机器），避免一次 I2C 抖动导致永久降级 */
    if (!s_imu_ok && (int32_t)(now - s_imu_retry_at_ms) < 0) {
        return false;
    }

    t_sQMI8658 data;
    memset(&data, 0, sizeof(data));
    qmi8658_Read_AccAndGry(&data);

    /* qmi8658_Read_AccAndGry() 失败时直接 return，结构体保持全 0 —— 据此判断 */
    if (data.acc_x == 0.0f && data.acc_y == 0.0f && data.acc_z == 0.0f &&
        data.gyro_x == 0.0f && data.gyro_y == 0.0f && data.gyro_z == 0.0f) {
        if (++s_imu_fail_count >= 10) {
            s_imu_ok          = false;
            s_imu_retry_at_ms = now + POWER_IMU_RETRY_MS;
            ESP_LOGW(TAG, "IMU 连续 %d 次读不到，暂停 IMU 检测（%u ms 后重试，期间只靠触摸和按键）",
                     s_imu_fail_count, (unsigned)POWER_IMU_RETRY_MS);
        }
        return false;
    }
    s_imu_fail_count = 0;

    if (!s_imu_ok) {
        s_imu_ok         = true;
        s_prev_acc.valid = false;   /* 重新取基线，避免拿旧姿态当参照 */
        ESP_LOGI(TAG, "IMU 恢复响应，重新启用 IMU 静止检测");
    }

    float gyro_sum = fabsf(data.gyro_x) + fabsf(data.gyro_y) + fabsf(data.gyro_z);

    if (!s_prev_acc.valid) {
        s_prev_acc.valid = true;
        s_prev_acc.ax    = data.acc_x;
        s_prev_acc.ay    = data.acc_y;
        s_prev_acc.az    = data.acc_z;
        return false;
    }

    float diff = fabsf(data.acc_x - s_prev_acc.ax) +
                 fabsf(data.acc_y - s_prev_acc.ay) +
                 fabsf(data.acc_z - s_prev_acc.az);

    s_prev_acc.ax = data.acc_x;
    s_prev_acc.ay = data.acc_y;
    s_prev_acc.az = data.acc_z;

    return (diff > power_imu_acc_threshold()) || (gyro_sum > power_imu_gyro_threshold());
}

/* ============================ 进入/退出软省电 ============================ */

/** 现在能不能睡：业务空闲、没有弹窗、没有在打字
 *  （设置二级页不算：用户停在设置页走开，屏幕一样该暗下来） */
static bool power_can_sleep(void)
{
    if (s_timeout_ms == (int32_t)APP_POWER_TIMEOUT_NEVER) {
        return false;
    }
    if (app_bus_get_state() != APP_STATE_IDLE) {
        return false;
    }
    if (app_bus_is_ui_modal() || app_bus_is_ui_typing()) {
        return false;
    }
    if (lcd_get_panel() == NULL) {
        return false;
    }
    return true;
}

/**
 * @brief  读一次触摸：和 LVGL 的 indev 共用同一把锁
 *
 * esp_lvgl_port 的触摸输入设备也会读同一个 FT5x06 句柄，而且总是在持 LVGL 锁时读；
 * 这里同样用 lvgl_port_lock 串行化，避免两边并发改 tp->data 造成偶发误判。
 *
 * @return true = 有手指按着 / 这一拍拿不到锁（按活动处理）；false = 没有触摸
 */
static bool power_touch_pressed(void)
{
    if (!lvgl_port_lock(20)) {
        /* 拿不到锁（LVGL 正在刷屏）：这一拍读不了，按“有活动”处理 ——
         * 宁可晚一点省电，也不要在用户可能正操作屏幕时误判 */
        return true;
    }
    bool pressed = lcd_touch_pressed();
    lvgl_port_unlock();
    return pressed;
}

/**
 * @brief  进入软省电：只降背光 + WiFi 省电 + 屏蔽录音键
 *
 * @note   完全不碰面板/SPI/LVGL：这条路已经实机验证过，画面不会丢。
 *         背光关到 POWER_SLEEP_BRIGHTNESS（当前 0 = 全灭，省电最大化），
 *         这个值只在运行时生效、不写 flash；唤醒后恢复用户保存的亮度。
 */
static void power_enter_soft_sleep(void)
{
    if (s_sleeping) {
        return;
    }
    s_sleeping = true;
    /* 丢弃进入前遗留的唤醒请求：那条请求代表“入睡前的动作”，
     * 不能在刚入睡的下一拍把设备又立刻叫醒 */
    s_wake_req = false;

    ESP_LOGI(TAG, "进入软省电：背光→%d%%（临时值，不写 flash）+ WiFi 最大省电档 + 屏蔽录音键（touch/IMU/按键可唤醒）",
             POWER_SLEEP_BRIGHTNESS);

    esp_err_t bk_err = lcd_brightness_set(POWER_SLEEP_BRIGHTNESS);
    if (bk_err != ESP_OK) {
        /* 背光降不下来只是省电效果打折，不阻塞流程（下次退出时还会恢复） */
        ESP_LOGW(TAG, "背光降到 %d%% 失败：%s", POWER_SLEEP_BRIGHTNESS, esp_err_to_name(bk_err));
    }

    if (net_is_initialized()) {
        esp_err_t err = net_set_power_save(true);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "WiFi 已切到最大省电档（保持连接）");
        } else if (err != ESP_ERR_INVALID_STATE) {
            /* INVALID_STATE = 射频还没起来，属预期，不当错误刷屏 */
            ESP_LOGW(TAG, "WiFi 省电切换失败：%s（保持原功耗档）", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "WiFi 未初始化：本次只降背光、屏蔽录音键");
    }
}

/**
 * @brief  退出软省电：背光恢复用户亮度 + WiFi 回默认功耗档 + 解禁录音键
 */
static void power_leave_soft_sleep(void)
{
    if (!s_sleeping) {
        return;
    }
    s_sleeping = false;

    if (net_is_initialized()) {
        esp_err_t err = net_set_power_save(false);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "WiFi 已恢复默认功耗档");
        } else if (err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "恢复 WiFi 功耗档失败：%s", esp_err_to_name(err));
        }
    }

    int restore = app_settings_get_backlight();
    esp_err_t bk_err = lcd_brightness_set(restore);
    if (bk_err != ESP_OK) {
        ESP_LOGW(TAG, "恢复背光到 %d%% 失败：%s", restore, esp_err_to_name(bk_err));
    }

    s_last_active_ms = power_now_ms();
    ESP_LOGI(TAG, "退出软省电：背光恢复到 %d%%、WiFi 回默认功耗档、录音键解禁", restore);
}

/* ============================ 对外接口 ============================ */

esp_err_t app_power_init(void)
{
    s_timeout_ms      = (int32_t)app_settings_get_sleep_timeout_ms();
    s_last_active_ms  = power_now_ms();
    s_sleeping        = false;
    s_wake_req        = false;
    s_imu_ok          = true;
    s_imu_fail_count  = 0;
    s_imu_retry_at_ms = 0;
    s_prev_acc.valid  = false;
    s_imu_sens        = app_settings_get_imu_sensitivity();

    /* 版本横幅：用来一眼确认“烧进去的到底是不是这一版” */
    ESP_LOGI(TAG, "===== 固件 2026-10-27 build-8（软省电：降背光 + 屏蔽录音键 + WiFi 省电）=====");

    ESP_LOGI(TAG, "低功耗模块启动：超时 %" PRId32 " ms（0=永不），采样 %d ms，省电轮询 %d ms，"
                  "IMU 灵敏度 %s（acc %.2f g / gyro %.1f °/s）",
             s_timeout_ms, APP_POWER_SAMPLE_MS, APP_POWER_SLEEP_POLL_MS,
             power_imu_sens_name(s_imu_sens), power_imu_acc_threshold(),
             power_imu_gyro_threshold());
    return ESP_OK;
}

void app_power_notify_activity(void)
{
    s_last_active_ms = power_now_ms();
    /* 同时置“唤醒请求”：软省电期间由任务循环下一拍消费，
     * 这样快按/轻触也不会因为刚好落在两次采样之间被丢掉 */
    s_wake_req = true;
}

bool app_power_is_sleeping(void)
{
    return s_sleeping;
}

uint32_t app_power_get_timeout_ms(void)
{
    return (uint32_t)s_timeout_ms;
}

esp_err_t app_power_set_timeout_ms(uint32_t timeout_ms)
{
    esp_err_t err = app_settings_set_sleep_timeout_ms(timeout_ms);
    /* 不管存不存得进，新的超时都立刻生效 */
    s_timeout_ms = (int32_t)app_settings_get_sleep_timeout_ms();
    app_power_notify_activity();

    if (s_timeout_ms == (int32_t)APP_POWER_TIMEOUT_NEVER) {
        ESP_LOGI(TAG, "休眠超时改为“永不”（0 ms）：不会再自动进入软省电");
    } else {
        ESP_LOGI(TAG, "休眠超时改为 %" PRId32 " ms", s_timeout_ms);
    }
    return err;
}

uint8_t app_power_get_imu_sensitivity(void)
{
    return s_imu_sens;
}

esp_err_t app_power_set_imu_sensitivity(uint8_t level)
{
    esp_err_t err = app_settings_set_imu_sensitivity(level);
    /* 不管存不存得进，新档位都立刻生效（下一次采样就用新阈值） */
    s_imu_sens = app_settings_get_imu_sensitivity();
    app_power_notify_activity();

    ESP_LOGI(TAG, "IMU 唤醒灵敏度改为 %s（acc %.2f g / gyro %.1f °/s）",
             power_imu_sens_name(s_imu_sens), power_imu_acc_threshold(),
             power_imu_gyro_threshold());
    return err;
}

void app_power_task(void *arg)
{
    (void)arg;

#if !APP_POWER_ENABLE
    /* 排障开关置 0：整条自动省电链路不启动，行为和旧固件一致 */
    ESP_LOGW(TAG, "APP_POWER_ENABLE=0，自动省电功能已关闭");
    vTaskDelete(NULL);
    return;
#endif

    ESP_LOGI(TAG, "低功耗任务已启动（软省电：只降背光 + WiFi 省电 + 屏蔽录音键）");

    for (;;) {
        uint32_t now   = power_now_ms();
        bool     active = power_touch_pressed() || power_imu_moved() || bsp_gpio_key_is_pressed();

        if (s_sleeping) {
            /* 唤醒判据 = 采样到的活动 || 别处置的唤醒请求（快按/设置提交等）。
             * 闩锁只消费一次，避免旧请求把下一次休眠立刻又打断 */
            bool wake = s_wake_req || active;
            s_wake_req = false;
            if (wake) {
                power_leave_soft_sleep();   /* 内部会把 s_last_active_ms 置为当前时刻 */
                s_last_active_ms = power_now_ms();
            } else {
                vTaskDelay(pdMS_TO_TICKS(APP_POWER_SLEEP_POLL_MS));
                continue;
            }
        } else {
            if (active) {
                s_last_active_ms = now;
            }

            uint32_t idle = now - s_last_active_ms;
            if (s_timeout_ms > 0 && idle >= (uint32_t)s_timeout_ms && power_can_sleep()) {
                ESP_LOGI(TAG, "空闲 %" PRIu32 " ms ≥ 超时 %" PRIu32 " ms，进入软省电",
                         idle, (uint32_t)s_timeout_ms);
                power_enter_soft_sleep();
                vTaskDelay(pdMS_TO_TICKS(APP_POWER_SLEEP_POLL_MS));
                continue;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(APP_POWER_SAMPLE_MS));
    }
}
