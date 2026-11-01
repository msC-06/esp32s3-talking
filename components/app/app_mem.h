/**
 * @file    app_mem.h
 * @brief   空闲内存监控：空闲任务按间隔触发，低优先级任务读内存并上报
 *
 * 分层：app 层，只用 ESP-IDF 的堆统计接口 + esp_lvgl_port 的锁。
 *
 * 为什么分两个角色：
 *   空闲任务（IDLE0）的栈只有 CONFIG_FREERTOS_IDLE_TASK_STACKSIZE（本工程 1536 字节），
 *   在它里面打印日志（vsnprintf + UART 写）会直接把栈冲爆，
 *   所以：
 *     - 空闲任务钩子：只做一次时间判断，到点给监控任务发个通知（几十字节栈开销）；
 *     - 监控任务（优先级 1，栈 4KB）：真正去读堆/LVGL 池并打印。
 *   这样既满足“空闲任务定期检查”的意图，又不会踩空闲任务栈小、不能阻塞的坑。
 *
 * 上报内容：
 *   内部 RAM / PSRAM 的空闲量与最大可分配块（最大块决定还能不能分配大缓冲）、
 *   开机以来的历史最低空闲量、LVGL 自己那个内存池的占用率与碎片率；
 *   内部 RAM 或 PSRAM 低于阈值时会额外打一条告警。
 */

#ifndef __APP_MEM_H
#define __APP_MEM_H

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  注册空闲任务钩子，并立刻打一次基线
 *
 * @return ESP_OK 成功；其它为注册失败的错误码
 * @note   由 app_start() 调用；重复调用安全（只会注册一次）
 */
esp_err_t app_mem_monitor_init(void);

/**
 * @brief  立刻打印一次内存快照（不重置定期上报的计时）
 *
 * @param  where 触发位置说明，例如 "启动完成"、"搜索前"，只用于日志
 */
void app_mem_log(const char *where);

/**
 * @brief  打印一行精简内存信息（内部堆余量 / 最大块 / 内部历史最低 / PSRAM），用于关键路径打点
 *
 * @param  where 触发位置说明，例如 "识别前"、"上传失败"
 * @note   只打一行，适合在请求前后插桩；**带历史最低**，相邻两笔的差值就是中间那一段
 *         吃掉的内部 RAM —— 排查「哪一步把内部堆挤干」时靠它定位。
 *         详细的（含任务栈、LVGL 池）用 app_mem_log()。
 */
void app_mem_log_brief(const char *where);

/**
 * @brief  开始量一段代码的**真实内存峰值**（把内部堆低水位重置到此刻）
 *
 * @note   和 app_mem_log_brief() 的区别：brief 报的是“自开机以来”的历史最低，
 *         它是单调的，事后永远分不清是哪一段造成的；begin/end 一对则只量这两行之间。
 *         典型用法：
 *             app_mem_peak_begin();
 *             xxx_do_network_thing();
 *             app_mem_peak_end("联网搜索");
 *         重复 begin 不会嵌套（第二次是空操作）；没 begin 就 end 也安全。
 */
void app_mem_peak_begin(void);

/**
 * @brief  结束本段测量并打印峰值
 *
 * @param  where 阶段名，例如 "第1轮请求"、"联网搜索"，只用于日志
 * @note   打印「内部峰值 x KB（现空闲 y KB，最大块 z KB）」。内部实现里**先读再停**，
 *         因为 heap_caps_monitor_local_minimum_free_size_stop() 之后低水位会退回开机以来的值。
 */
void app_mem_peak_end(const char *where);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MEM_H */
