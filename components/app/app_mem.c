/**
 * @file    app_mem.c
 * @brief   空闲内存监控实现（空闲任务触发 + 低优先级任务上报）
 *
 * 为什么不是“直接在空闲任务里打印”：
 *   FreeRTOS 空闲任务的栈只有 CONFIG_FREERTOS_IDLE_TASK_STACKSIZE（本工程 1536 字节），
 *   而一次带一堆参数的 ESP_LOGI 光 vsnprintf + UART 写就要几百字节，
 *   再加上读堆统计、走 LVGL 内存池链表，很容易把空闲任务的栈冲爆
 *   （现象是 “A stack overflow in task IDLE0 has been detected”）。
 *   而且空闲任务还负责回收被删除任务的 TCB/栈，绝对不能在里面阻塞。
 *
 *   所以这里的职责划分是：
 *     空闲任务（mem_idle_hook）  —— 只做一次时间判断，到点给监控任务发个通知（几十字节栈）
 *     监控任务（mem_monitor_task）—— 真正读堆/LVGL 统计并打印（自带 4KB 栈）
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/**
 * 注意包含顺序：freeertos/FreeRTOS.h 必须在 esp_freertos_hooks.h 之前。
 * esp_freertos_hooks.h 内部只包含 freertos/portmacro.h，此时 configNUMBER_OF_CORES
 * 还没定义，portmacro.h 里的 portYIELD_CORE 就不会被定义；等后面再包含 FreeRTOS.h 时，
 * portmacro.h 已被 include guard 挡掉，就会报
 * “portYIELD_CORE() must be defined if configNUMBER_OF_CORES > 1”。
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_mem.h"
#include "app_config.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_freertos_hooks.h"

#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "APP_MEM";

/* ============================ 内部状态（全部 static） ============================ */

static bool         s_registered = false;   /*!< 钩子是否已注册 */
static TaskHandle_t s_task       = NULL;    /*!< 监控任务句柄 */
static int64_t      s_last_us    = 0;       /*!< 上次上报时间（esp_timer 基准，微秒） */

/**
 * 要盯栈的任务名
 * @note  用 xTaskGetHandle() 按名字查，任务自删了会返回 NULL，不会踩野指针；
 *        以后新增任务，把名字加进这个表就能一起被监控。
 */
static const char *const s_watch_names[] = {
    "app_wifi", "app_net", "app_key", "app_audio", "app_mem", "taskLVGL",
};
#define MEM_WATCH_NUM   (sizeof(s_watch_names) / sizeof(s_watch_names[0]))

/* ============================ 内部函数 ============================ */

/**
 * @brief  打印各任务栈的剩余量（历史最小值），低于阈值就告警
 *
 * @note   uxTaskGetStackHighWaterMark() 的单位是 StackType_t（字），乘 4 得字节。
 *         这个值是“开机以来剩得最少的时候”，能提前发现快爆栈的任务
 *         （之前就被“4KB 的消息缓冲放在 4KB 栈的任务里”坑过一次）。
 */
static void mem_log_task_stacks(void)
{
    char line[192];
    int  off     = 0;
    bool danger  = false;
    char danger_line[96];
    int  d_off   = 0;

    line[0] = '\0';
    danger_line[0] = '\0';

    for (size_t i = 0; i < MEM_WATCH_NUM; i++) {
        TaskHandle_t h = xTaskGetHandle(s_watch_names[i]);
        if (h == NULL) {
            continue;               /* 任务不存在（还没建 / 已经自删） */
        }
        uint32_t free_bytes = (uint32_t)uxTaskGetStackHighWaterMark(h) * sizeof(StackType_t);

        if (off < (int)sizeof(line) - 24) {
            off += snprintf(line + off, sizeof(line) - (size_t)off,
                            "%s%s=%u", (off > 0) ? " " : "", s_watch_names[i], (unsigned)free_bytes);
        }
        if (free_bytes < APP_MEM_WARN_STACK) {
            danger = true;
            if (d_off < (int)sizeof(danger_line) - 16) {
                d_off += snprintf(danger_line + d_off, sizeof(danger_line) - (size_t)d_off,
                                  "%s%s=%u", (d_off > 0) ? " " : "",
                                  s_watch_names[i], (unsigned)free_bytes);
            }
        }
    }

    if (off > 0) {
        ESP_LOGI(TAG, "任务栈剩余(字节，历史最小值): %s", line);
    }
    if (danger) {
        ESP_LOGW(TAG, "有任务栈剩余不足 %u 字节，随时可能爆栈（查局部大数组/大结构体）: %s",
                 (unsigned)APP_MEM_WARN_STACK, danger_line);
    }
}

/**
 * @brief  把字节数转成 KB（日志里好读）
 */
static inline uint32_t mem_kb(size_t bytes)
{
    return (uint32_t)((bytes + 512U) / 1024U);
}

/**
 * @brief  读一次 LVGL 内存池的占用情况
 *
 * @param  used_pct 输出：占用百分比；读不到时输出 0xFF
 * @param  frag_pct 输出：碎片率百分比；读不到时输出 0xFF
 *
 * @note   LVGL 的池没有内部锁，直接读可能在别的任务分配时读到不一致的链表，
 *         所以这里先抢 LVGL 锁（拿不到就跳过这次统计，不影响内存上报）。
 */
static void mem_read_lvgl_pool(uint8_t *used_pct, uint8_t *frag_pct)
{
    *used_pct = 0xFF;
    *frag_pct = 0xFF;

    if (lvgl_port_lock(50)) {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        *used_pct = mon.used_pct;
        *frag_pct = mon.frag_pct;
        lvgl_port_unlock();
    }
}

/* ============================ 对外接口 ============================ */

void app_mem_log(const char *where)
{
    const size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t int_blk  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t ps_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t ps_blk   = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    /* 注意：历史最低要按“内部堆”取。esp_get_minimum_free_heap_size() 是所有堆里
     * 的最小值，PSRAM 有 6MB+，那个数会一直是 6000 多 KB，看不出内部 RAM 的真实水位。 */
    const size_t int_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const char  *who      = (where != NULL) ? where : "-";

    uint8_t lv_used = 0xFF;
    uint8_t lv_frag = 0xFF;
    mem_read_lvgl_pool(&lv_used, &lv_frag);

    /* 拆成两行：单行参数太多会多吃栈，也不方便肉眼看 */
    ESP_LOGI(TAG, "[%s] 内部空闲 %u KB（最大块 %u KB）| 内部历史最低 %u KB",
             who, mem_kb(int_free), mem_kb(int_blk), mem_kb(int_min));
    if (lv_used == 0xFF) {
        ESP_LOGI(TAG, "[%s] PSRAM 空闲 %u KB（最大块 %u KB）| LVGL池 占用未知（没抢到锁）",
                 who, mem_kb(ps_free), mem_kb(ps_blk));
    } else {
        ESP_LOGI(TAG, "[%s] PSRAM 空闲 %u KB（最大块 %u KB）| LVGL池 %u%%（碎片 %u%%）",
                 who, mem_kb(ps_free), mem_kb(ps_blk), (unsigned)lv_used, (unsigned)lv_frag);
    }

    /* 低于阈值就告警，方便及时发现“缓冲区调太大” */
    if (int_free < APP_MEM_WARN_INTERNAL) {
        ESP_LOGW(TAG, "内部 RAM 只剩 %u KB（阈值 %u KB）：建议调小 APP_TEXT_MAX_LEN / "
                      "APP_CHAT_QUEUE_LEN / CONFIG_LV_MEM_SIZE",
                 mem_kb(int_free), mem_kb(APP_MEM_WARN_INTERNAL));
    }
    if (ps_free < APP_MEM_WARN_PSRAM) {
        ESP_LOGW(TAG, "PSRAM 只剩 %u KB（阈值 %u KB）：建议调小 APP_LLM_RESP_BUF_SIZE / "
                      "APP_WEB_SEARCH_RESP_BUF_SIZE",
                 mem_kb(ps_free), mem_kb(APP_MEM_WARN_PSRAM));
    }

    /* 顺带看一眼各任务栈，爆栈比内存不足更致命 */
    mem_log_task_stacks();
}

void app_mem_log_brief(const char *where)
{
    /* 带上内部堆的历史最低：打点是“前/后”两笔，中间掉下去的量只有水位能看出来。
     * 水位是单调的，所以相邻两笔的差值就是这两笔之间那一段吃掉的量 —— 这正是
     * 定位“到底哪一段把内部 RAM 挤干”最省事的办法。 */
    ESP_LOGI(TAG, "[%s] 内部空闲 %u KB（最大块 %u KB，历史最低 %u KB）| PSRAM 空闲 %u KB",
             (where != NULL) ? where : "-",
             mem_kb(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             mem_kb(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             mem_kb(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
             mem_kb(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

/* ============================ 单阶段峰值（可重算的低水位） ============================ */

/**
 * @brief  峰值监测是否正在跑
 *
 * @note   heap_caps_get_minimum_free_size() 给的是“**自开机以来**”的单调水位，只看它永远分不清
 *         “这轮搜索最凶”和“三个小时前某次更凶”。IDF 5.x 提供的
 *         heap_caps_monitor_local_minimum_free_size_start/stop 可以把这个水位**重置**到
 *         调用那一刻，于是 begin/end 一对就量出“这一段自己最低掉到多少”。
 */
static bool s_peak_active = false;

void app_mem_peak_begin(void)
{
    if (s_peak_active) {
        return;             /* 已经在量了，不嵌套（嵌套会把起点冲掉，得到的是内层而不是外层） */
    }
    s_peak_active = (heap_caps_monitor_local_minimum_free_size_start() == ESP_OK);
    if (!s_peak_active) {
        ESP_LOGW(TAG, "局部水位监测启动失败，本段只能看全局水位");
    }
}

void app_mem_peak_end(const char *where)
{
    /* ⚠️ 顺序：必须在 stop **之前**读最小值 —— stop 之后这个接口又退回“自开机以来”的值，
     *    先停再读就白测了。 */
    const size_t int_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t int_blk  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    if (s_peak_active) {
        heap_caps_monitor_local_minimum_free_size_stop();
        s_peak_active = false;
    }

    /* 一行说完这一段：最低掉到多少（峰值）、现在回来了多少、当时的最大连续块够不够建 TLS */
    ESP_LOGI(TAG, "[%s] 内部峰值 %u KB（现空闲 %u KB，最大块 %u KB）",
             (where != NULL) ? where : "-",
             mem_kb(int_min), mem_kb(int_free), mem_kb(int_blk));
}

/**
 * @brief  空闲任务钩子：只做时间判断 + 给监控任务发通知
 *
 * @return false 表示不需要让出 CPU
 * @note   ⚠️ 这里绝对不能打印日志或做重活：空闲任务栈只有 1536 字节
 *         （CONFIG_FREERTOS_IDLE_TASK_STACKSIZE），而且它还要回收被删任务的资源。
 */
static bool mem_idle_hook(void)
{
#if APP_MEM_LOG_ENABLE
    if (s_task == NULL) {
        return false;       /* 监控任务还没起来 */
    }

    const int64_t now = esp_timer_get_time();
    if (s_last_us != 0 &&
        (now - s_last_us) < (int64_t)APP_MEM_LOG_INTERVAL_MS * 1000) {
        return false;       /* 没到点，立刻返回 */
    }
    s_last_us = now;
    xTaskNotifyGive(s_task);    /* 重活交给监控任务 */
#endif
    return false;
}

/**
 * @brief  监控任务：被空闲任务按间隔唤醒，读内存并打印
 */
static void mem_monitor_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "内存监控任务启动（Core %d，优先级 %d，栈 %d 字节）",
             xPortGetCoreID(), APP_MEM_TASK_PRIO, APP_MEM_TASK_STACK);

    for (;;) {
        /* 一直睡到空闲任务通知“到点了” */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        app_mem_log("空闲任务");
    }
}

esp_err_t app_mem_monitor_init(void)
{
    if (s_registered) {
        return ESP_OK;
    }

    /* 1. 先建监控任务（打印需要足够的栈） */
    if (xTaskCreatePinnedToCore(mem_monitor_task, "app_mem",
                                APP_MEM_TASK_STACK, NULL, APP_MEM_TASK_PRIO,
                                &s_task, APP_CPU_NET_CORE) != pdPASS) {
        ESP_LOGE(TAG, "内存监控任务创建失败");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* 2. 再注册空闲任务钩子（由它按间隔触发上报） */
    esp_err_t err = esp_register_freertos_idle_hook(mem_idle_hook);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册空闲任务钩子失败: %s", esp_err_to_name(err));
        vTaskDelete(s_task);
        s_task = NULL;
        return err;
    }
    s_registered = true;

    ESP_LOGI(TAG, "内存监控已启动：空闲任务每 %d 秒触发一次上报", APP_MEM_LOG_INTERVAL_MS / 1000);
    app_mem_log("启动基线");        /* 这一句在调用者（main 任务）上下文里打印 */
    return ESP_OK;
}
