/**
 * @file    app.c
 * @brief   应用入口实现：初始化 + 创建双核任务
 */

#include <string.h>

#include "app.h"
#include "app_config.h"
#include "app_bus.h"
#include "app_key.h"
#include "app_audio.h"
#include "app_wifi.h"
#include "app_net.h"
#include "app_chat_ui.h"
#include "app_mem.h"
#include "app_power.h"
#include "app_settings.h"
#include "app_time.h"

#include "my_drivers/lcd.h"
#include "my_drivers/audio/audio.h"
#include "my_drivers/font_hzk16/hzk16.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "APP";

/* ============================ 内部函数 ============================ */

/**
 * @brief  创建任务并绑定到指定核心
 *
 * @param  fn    任务函数
 * @param  name  任务名（调试用）
 * @param  stack 栈大小（字节）
 * @param  prio  优先级
 * @param  core  绑定的核心（0 = 网络核，1 = 应用核）
 */
static esp_err_t create_task(TaskFunction_t fn, const char *name,
                             uint32_t stack, UBaseType_t prio, BaseType_t core)
{
    if (xTaskCreatePinnedToCore(fn, name, stack, NULL, prio, NULL, core) != pdPASS) {
        ESP_LOGE(TAG, "任务 %s 创建失败（内存不足？）", name);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "任务 %-9s 已创建 -> Core %d，优先级 %d，栈 %u 字节",
             name, (int)core, (int)prio, (unsigned)stack);
    return ESP_OK;
}

/* ============================ 应用入口 ============================ */

esp_err_t app_start(void)
{
    ESP_LOGI(TAG, "================ 对话掌机启动 ================");

    /* 液晶屏必须已经就绪（board_init() 里完成） */
    if (lcd_get_display() == NULL) {
        ESP_LOGE(TAG, "液晶屏未就绪，请先调用 board_init()");
        return ESP_ERR_INVALID_STATE;
    }

    /* ---------- 0. 用户设置（背光等）----------
     * 尽量早：lcd_init() 里背光是 100%，这里读回用户上次保存的亮度并立刻应用，
     * 免得开机先刺眼地亮一下再变暗。TF 卡没插就用默认 50%，不影响后面流程。 */
    if (app_settings_init() != ESP_OK) {
        ESP_LOGW(TAG, "用户设置初始化异常，继续启动");
    }

    /* ---------- 0.5 内存监控：先注册钩子，第一行日志就是“还没分配大缓冲”的基线 ---------- */
    app_mem_monitor_init();

    /* ---------- 1. 跨任务队列 ---------- */
    ESP_RETURN_ON_ERROR(app_bus_init(), TAG, "队列初始化失败");

    /* ---------- 1.5 WiFi 提前初始化 ----------
     * 顶栏那个 WiFi 文字按钮随时可能被点开，它要靠 app_wifi 里的
     * “请求队列 + 互斥锁 + NVS 名单”才能发扫描/连接请求，所以这些必须在**界面建好之前**
     * 就准备好（界面一加载，用户就能点）；射频（net_init）也一并在这里初始化，
     * app_wifi_task 里再调一次是幂等的。顺便把 NVS 里记着的网络读进内存，
     * 开机日志里就能看到“记着几个 WiFi”。 */
    esp_err_t err = app_wifi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 初始化失败: %s（界面上的 WiFi 按钮仍可打开，但扫不到东西）",
                 esp_err_to_name(err));
    }
    /* ---------- 2. 中文字库（HZK16，GB2312 点阵，从 fontbin 分区按需读取） ---------- */
    if (hzk16_init() != ESP_OK) {
        ESP_LOGE(TAG, "中文字库初始化失败，中文会显示成占位框");
    }
    hzk16_set_fallback_font(&lv_font_montserrat_14);    /* 英文/数字用西文字体回退 */
    const lv_font_t *cn_font = hzk16_get_lv_font();

    /* ---------- 3. 聊天界面（新建 screen 并加载） ---------- */
    err = app_chat_ui_init(cn_font);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "聊天界面创建失败: %s", esp_err_to_name(err));
        return err;
    }
    app_bus_post_chat(CHAT_MSG_STATUS, app_bus_state_text(APP_STATE_IDLE));

    /* ---------- 4. 音频：ES8311/ES7210 + 录音器 ---------- */
    err = audio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "音频初始化失败: %s（麦克风将不可用）", esp_err_to_name(err));
        app_bus_post_chat(CHAT_MSG_ERROR, "音频初始化失败");
    } else if (app_audio_init() != ESP_OK) {
        app_bus_post_chat(CHAT_MSG_ERROR, "录音器初始化失败");
    }

    /* ---------- 5. 按键（GPIO0 + esp_timer 20ms 消抖） ---------- */
    err = app_key_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(err));
        app_bus_post_chat(CHAT_MSG_ERROR, "按键初始化失败");
    }

    /* ---------- 6. 网络模块（ASR token 缓存 + 对话历史，只初始化不联网） ---------- */
    err = app_net_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "网络模块初始化异常: %s", esp_err_to_name(err));
    }

    /* ---------- 6.5 系统时间（SNTP 对时）----------
     * 必须在网络任务跑起来之前/同时启动：它对时的结果会进 system 提示词
     * （见 app_llm.c），也决定界面待机提示里那串 HH:MM 什么时候出现。
     * 这里只是创建对时任务，它自己会等 WiFi 连上再发请求，不会阻塞启动。 */
    if (app_time_start() != ESP_OK) {
        ESP_LOGW(TAG, "时间模块启动失败，界面不显示时间、AI 也不知道当前日期");
    }

    /* ---------- 6.6 低功耗（IMU + 触摸静止检测）----------
     * 必须在 app_settings_init() 之后（要读用户选的休眠时间），
     * 也必须在界面建好之后：省电时只降背光 + WiFi 省电 + 屏蔽录音键，
     * 不碰面板/SPI/LVGL（早先“关屏 + light sleep”的方案会让 ST7789 醒不过来）。
     * 这里只是初始化状态，真正的轮询在 app_power_task 里。 */
    if (app_power_init() != ESP_OK) {
        ESP_LOGW(TAG, "低功耗模块初始化异常，自动休眠可能不可用");
    }

    /* ---------- 7. 创建四个任务（双核绑定） ---------- */
    esp_err_t task_err = ESP_OK;

#define APP_CREATE_TASK(fn, name, stack, prio, core)                          \
    do {                                                                      \
        if (create_task((fn), (name), (stack), (prio), (core)) != ESP_OK) {   \
            task_err = ESP_FAIL;                                              \
        }                                                                     \
    } while (0)

    /* Core 0（网络核）：WiFi 事件循环 + 网络请求（ASR/LLM） */
    APP_CREATE_TASK(app_wifi_task,  "app_wifi",
                    APP_WIFI_TASK_STACK,  APP_WIFI_TASK_PRIO,  APP_CPU_NET_CORE);
    APP_CREATE_TASK(app_net_task,   "app_net",
                    APP_NET_TASK_STACK,   APP_NET_TASK_PRIO,   APP_CPU_NET_CORE);

    /* Core 1（应用/显示核）：按键业务状态机 + 音频采集 +（LVGL 自己的任务） */
    APP_CREATE_TASK(app_key_task,   "app_key",
                    APP_KEY_TASK_STACK,   APP_KEY_TASK_PRIO,   APP_CPU_APP_CORE);
    APP_CREATE_TASK(app_audio_task, "app_audio",
                    APP_AUDIO_TASK_STACK, APP_AUDIO_TASK_PRIO, APP_CPU_APP_CORE);

    /* 低功耗轮询（同应用核，最低优先级：只在没人用时才采样/休眠） */
    APP_CREATE_TASK(app_power_task, "app_power",
                    APP_POWER_TASK_STACK, APP_POWER_TASK_PRIO, APP_POWER_TASK_CORE);

#undef APP_CREATE_TASK

    if (task_err != ESP_OK) {
        ESP_LOGE(TAG, "有任务创建失败，功能可能不完整");
    }

    /* ---------- 8. 内存快照 ---------- */
    ESP_LOGI(TAG, "内存：内部剩余 %u 字节，PSRAM 剩余 %u 字节",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    app_mem_log("启动完成");        /* 详细的那份（含最大可分配块与 LVGL 池） */
    ESP_LOGI(TAG, "================ 启动完成，按住按键开始说话 ================");

    return task_err;
}
