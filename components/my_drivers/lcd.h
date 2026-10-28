/**
 * @file    lcd.h
 * @brief   液晶屏(ST7789 320x240 SPI) + 触摸屏(FT6336) + LVGL 接口
 *
 * 分层：本文件属于 my_drivers（屏幕/触摸是板上外部器件），
 *       底层用 bsp 的 SPI/I2C 接口与 PCA9557 的 LCD_CS。
 *
 * 命名说明：原来这里叫 bsp_display_*，按分层规则统一改成 lcd_* 前缀。
 *
 * 关键点（与旧版 esp32_s3_szp BSP 对齐）：
 *   1. LCD 的 CS 由 PCA9557 的 IO0 控制（ESP32 的 SPI CS 引脚为 NC），
 *      必须在发送初始化命令前把 CS 拉低；
 *   2. 背光 GPIO42 是“低电平点亮”，所以 LEDC 通道要开 output_invert。
 *
 * 省电说明：本工程最终采用的省电方式是“软省电”（见 app_power.c）——
 *   只降背光 + WiFi 省电 + 屏蔽录音键，**不进 light sleep、不拆 SPI 总线、
 *   不暂停 LVGL**，所以这里不再提供“唤醒后重建面板/背光”那一套诊断接口。
 */

#ifndef LCD_H
#define LCD_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== LCD 参数 ========== */
#define LCD_CMD_BITS               (8)
#define LCD_PARAM_BITS             (8)
#define BSP_LCD_BITS_PER_PIXEL     (16)
#define BSP_LCD_H_RES              (320)
#define BSP_LCD_V_RES              (240)

/* ========== LCD 引脚 ========== */
#define BSP_LCD_DC                 (GPIO_NUM_39)
#define BSP_LCD_RST                (GPIO_NUM_NC)
#define BSP_LCD_BACKLIGHT          (GPIO_NUM_42)

#define BSP_LCD_DRAW_BUF_HEIGHT    (20)
#define LCD_LEDC_CH                LEDC_CHANNEL_0

/* ========== 触摸参数 ========== */
#define BSP_TOUCH_I2C_ADDR         (0x38)
#define BSP_TOUCH_INT_GPIO         (GPIO_NUM_NC)
#define BSP_TOUCH_RST_GPIO         (GPIO_NUM_NC)

/* ========== 背光接口 ========== */
/** 初始化背光 LEDC 通道（占空比 0 = 灭） */
esp_err_t lcd_brightness_init(void);

/** 设置背光亮度（0~100%，低于下限的取值由调用者自己保证） */
esp_err_t lcd_brightness_set(int brightness_percent);

/** 关背光（= set(0)） */
esp_err_t lcd_backlight_off(void);

/** 开到最亮（= set(100)） */
esp_err_t lcd_backlight_on(void);

/* ========== 液晶屏接口 ========== */

/** 创建 panel IO + ST7789 并完成寄存器初始化（不含 LVGL） */
esp_err_t lcd_panel_new(void);

/* ========== 触摸屏接口 ========== */

/** 创建触摸设备；屏无触摸或不应答时返回错误码（不会 abort） */
esp_err_t lcd_touch_new(esp_lcd_touch_handle_t *ret_touch);

/**
 * @brief  轮询一次触摸：屏上现在有没有手指按着
 *
 * @return true = 有手指按下；false = 没有触摸（或触摸芯片没初始化/读失败）
 *
 * @note   这是给“低功耗检测”用的**非 LVGL** 路径：直接对触摸芯片做一次 I2C 读。
 *         正常运行时 LVGL 的输入设备也会读同一个触摸句柄，且总是在持 LVGL 锁时读；
 *         调用方也应持 LVGL 锁（如 app_power.c 的 power_touch_pressed()），
 *         否则两边的读序列会交叉，可能出现偶发误判。
 */
bool lcd_touch_pressed(void);

/* ========== LVGL 初始化入口（屏幕 + 触摸 + 背光） ========== */
void lcd_init(void);

/* ========== 获取句柄 ========== */
lv_display_t *lcd_get_display(void);

/** 获取 esp_lcd 面板句柄（开机动画/摄像头预览等直接写屏时用） */
esp_lcd_panel_handle_t lcd_get_panel(void);

/* ========== 直接写屏的原语（不经过 LVGL） ========== */

/** 整屏填充一个颜色（RGB565） */
void lcd_set_color(uint16_t color);

/** 显示一张图片（RGB565 原始数据，内部会拷贝一份） */
void lcd_draw_picture(int x_start, int y_start, int x_end, int y_end, const unsigned char *gImage);

/**
 * @brief  把一块 RGB565 像素直接刷到面板（开机动画/视频帧用，不做拷贝）
 *
 * @param  x, y    起点（左上角）
 * @param  w, h    宽高（像素）
 * @param  pixels  RGB565 像素数据，长度 w*h*2 字节
 * @note   异步发送（内部 SPI 队列），需要等画面稳定时调用者自己加小延时
 */
esp_err_t lcd_draw_bitmap(int x, int y, int w, int h, const void *pixels);

/* ========== 直接写屏时暂停 LVGL 刷新 ========== */

/**
 * @brief  暂停/恢复 LVGL 刷新
 *
 * @param  pause true = 暂停（内部会持有 LVGL 锁，期间不要再调用 LVGL API）
 *                false = 恢复
 * @note   用于“开机动画直接写屏”，避免画面被 LVGL 的刷新覆盖。
 */
esp_err_t lcd_lvgl_pause(bool pause);

#ifdef __cplusplus
}
#endif

#endif // LCD_H
