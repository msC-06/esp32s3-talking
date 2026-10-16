/**
 * @file    app_wifi_ui.h
 * @brief   WiFi 界面：附近 WiFi 列表弹窗 + 小键盘输入密码弹窗
 *
 * 入口是聊天界面顶栏左上角的 **WiFi 文字按钮**（原来是纯文本 label，现在套了一层
 * 可点击的按钮），点一下弹出附近 WiFi 列表：
 *
 *   ┌────────────────────────────┐
 *   │ 附近 WiFi          共 8 个 │
 *   │ ┌────────────────────────┐ │
 *   │ │ MyComputer      已存 强│ │  ← 点某一行 -> 输入密码弹窗
 *   │ │ TP-LINK_5G      开放 中│ │
 *   │ │ Xiaomi_A1            弱│ │
 *   │ └────────────────────────┘ │
 *   │   【重新扫描】    【关闭】 │
 *   └────────────────────────────┘
 *
 * 密码弹窗（中文标题 + 明文/掩码输入框 + 小键盘 + 连接/取消）：
 *   输入密码 -> 【连接】 -> WiFi 任务去连 -> 成功后自动存 NVS 并关掉弹窗。
 *   如果这个 WiFi 以前连过（NVS 里有密码），输入框留空直接点【连接】即可。
 *
 * 线程模型（很重要）：
 *   本模块**所有函数都必须跑在 LVGL 任务里**（点击回调、lv_timer 回调都在那里）。
 *   扫描和连接是阻塞操作，全部丢给 app_wifi_task 做，这里只轮询状态 + 版本号
 *   （app_wifi_scan_state()/app_wifi_scan_version()…），所以界面永远不会被射频卡住。
 */

#ifndef __APP_WIFI_UI_H
#define __APP_WIFI_UI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  打开“附近 WiFi”列表弹窗（必须在 LVGL 任务里调用）
 *
 * @note   弹窗是当前 screen 的子对象，半透明遮罩吃掉下层点击；
 *         打开的同时会自动请求一次扫描，并显示上一次的缓存结果。
 * @note   已经打开时什么都不做（不会叠出第二个弹窗）。
 */
void app_wifi_ui_open(void);

/**
 * @brief  关掉 WiFi 弹窗（列表和密码框一起关）
 */
void app_wifi_ui_close(void);

/** 弹窗是不是开着（聊天界面用它避免重复打开） */
bool app_wifi_ui_is_open(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_WIFI_UI_H */
