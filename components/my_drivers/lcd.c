/**
 * @file    lcd.c
 * @brief   液晶屏(ST7789 320x240 SPI) + 触摸屏(FT6336) + LVGL 初始化
 *
 * 说明（与旧版 esp32_s3_szp BSP 对齐的关键点）：
 *   1. LCD 的 CS 由 PCA9557 的 IO0 控制（ESP32 的 SPI CS 引脚为 NC），
 *      必须在发送初始化命令前把 CS 拉低；
 *   2. 背光 GPIO42 是“低电平点亮”，所以 LEDC 通道要开 output_invert；
 *   3. PCA9557 的 IO3~IO7 保持输入，不要全部配置成输出（会误驱动板上电路）。
 *
 * 命名：本模块属于 my_drivers，对外接口统一用 lcd_ 前缀
 *       （原来的 bsp_display_* / bsp_lvgl_init 已改名，功能不变）。
 *       屏幕自检（白/红/绿/蓝/黑刷色）已移到 app 层的开机动画里（app_splash）。
 */

#include <string.h>
#include "bsp/bsp_iic.h"
#include "bsp/bsp_spi.h"
#include "my_drivers/pca9557/pca9557.h"
#include "lcd.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_ft5x06.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LCD";

/* ============================ LVGL 任务参数 ============================ */

/** LVGL 任务绑定的核心：1 = APP 核（Core 0 留给 WiFi/网络任务，避免界面被 TLS 饿死） */
#define LCD_LVGL_TASK_CORE      1
/** LVGL 任务优先级：低于音频采集任务（实时性），高于按键任务 */
#define LCD_LVGL_TASK_PRIO      5

/* 全局句柄 */
static esp_lcd_panel_handle_t    panel_handle = NULL;
static esp_lcd_panel_io_handle_t io_handle    = NULL;
static esp_lcd_touch_handle_t    tp           = NULL;
static lv_disp_t                *disp         = NULL;
static lv_indev_t               *disp_indev   = NULL;

/* 整屏填充用的行缓冲（只分配一次，避免反复 malloc/free，
 * 也避免“DMA 还在读、内存已经被释放”的问题） */
static uint16_t *s_line_buf = NULL;

/* LVGL 暂停标志 */
static bool s_lvgl_paused = false;

/* ============================================================
 *  背光
 * ============================================================ */
esp_err_t lcd_brightness_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BSP_LCD_BACKLIGHT,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(BSP_LCD_BACKLIGHT, 1);   /* 先置高（灭） */

    const ledc_channel_config_t backlight_channel = {
        .gpio_num   = BSP_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LCD_LEDC_CH,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = 0,
        .duty       = 0,
        .hpoint     = 0,
        /* 用 IDF 的默认档 NO_ALIVE_NO_PD（官方文档 ledc.rst:350：
         * “no signal output and LEDC power domain will not be powered down in sleep”）。
         * 本工程走“软省电”（不进 light sleep、只把背光调到最小），开机路径一次配置后
         * 不再重建 PWM；若改回 LEDC_SLEEP_MODE_KEEP_ALIVE，在这颗芯片上会告警：
         * LEDC 的全局时钟是 APB（LEDC_AUTO_CLK 在本工程解析成 APB），APB 在 light sleep
         * 期间不能保持供电，驱动会打 "the binded timer can't keep alive in sleep"
         * （ledc.c:947-955），而且 KEEP_ALIVE 还要额外保住时钟源/电源域，睡眠功耗反而更高。 */
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags.output_invert = true,                /* 背光低电平点亮 */
    };
    const ledc_timer_config_t backlight_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = 0,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };

    ESP_RETURN_ON_ERROR(ledc_timer_config(&backlight_timer), TAG, "LEDC timer 配置失败");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&backlight_channel), TAG, "LEDC channel 配置失败");

    return ESP_OK;
}

esp_err_t lcd_brightness_set(int brightness_percent)
{
    if (brightness_percent > 100) brightness_percent = 100;
    if (brightness_percent < 0)   brightness_percent = 0;

    ESP_LOGD(TAG, "设置背光亮度: %d%%", brightness_percent);
    uint32_t duty_cycle = (1023 * brightness_percent) / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH, duty_cycle),
                        TAG, "ledc_set_duty 失败");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH),
                        TAG, "ledc_update_duty 失败");
    return ESP_OK;
}

esp_err_t lcd_backlight_off(void) { return lcd_brightness_set(0); }
esp_err_t lcd_backlight_on(void)  { return lcd_brightness_set(100); }

/* ============================================================
 *  液晶屏驱动（SPI 总线由 bsp_spi_init 负责）
 * ============================================================ */

/* 只创建并初始化 io_handle / panel_handle（含 reset + CS 拉低 + init + 反色/交换/镜像）。
 *
 * 不碰背光（那是 lcd_brightness_init 的事），不碰 LVGL，也不发 DISPON：
 * 开机路径 lcd_panel_new() = 背光 + 本函数，只跑一次（软省电不重建面板）。
 */
static esp_err_t lcd_create_panel_io(void)
{
    esp_err_t ret = ESP_OK;

    /* ---- Panel IO ---- */
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num       = BSP_LCD_DC,
        .cs_gpio_num       = BSP_LCD_SPI_CS,     /* GPIO_NUM_NC：CS 由 PCA9557 控制 */
        .pclk_hz           = BSP_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits      = LCD_CMD_BITS,
        .lcd_param_bits    = LCD_PARAM_BITS,
        .spi_mode          = 2,                  /* 与旧工程一致 */
        .trans_queue_depth = 10,
    };
    ESP_GOTO_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &io_handle),
        err, TAG, "创建 panel IO 失败（SPI 总线是否已初始化？）");

    /* ---- ST7789 驱动 ---- */
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,           /* NC：用软件复位命令 */
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
    };
    ESP_GOTO_ON_ERROR(
        esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle),
        err, TAG, "创建 ST7789 面板失败");

    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(panel_handle), err, TAG, "面板复位失败");

    /* ★关键：CS 由 PCA9557 控制，必须在发初始化命令之前拉低，否则屏收不到任何命令 */
    ESP_GOTO_ON_ERROR(pca9557_lcd_cs(0), err, TAG, "拉低 LCD_CS 失败");

    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(panel_handle), err, TAG, "面板初始化失败");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_invert_color(panel_handle, true), err, TAG, "反色设置失败");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_swap_xy(panel_handle, true), err, TAG, "交换 XY 失败");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_mirror(panel_handle, true, false), err, TAG, "镜像设置失败");

    ESP_LOGI(TAG, "ST7789 初始化完成（%dx%d）", BSP_LCD_H_RES, BSP_LCD_V_RES);
    return ESP_OK;

err:
    if (panel_handle) { esp_lcd_panel_del(panel_handle);    panel_handle = NULL; }
    if (io_handle)    { esp_lcd_panel_io_del(io_handle);    io_handle    = NULL; }
    return ret;
}

/* 开机路径：初始化背光 PWM + 创建并初始化面板（对外行为与改动前一致） */
esp_err_t lcd_panel_new(void)
{
    ESP_RETURN_ON_ERROR(lcd_brightness_init(), TAG, "背光初始化失败");
    return lcd_create_panel_io();
}

/* ============================================================
 *  直接写屏原语
 * ============================================================ */
static uint16_t *lcd_get_line_buffer(void)
{
    if (s_line_buf) {
        return s_line_buf;
    }

    /* 优先放 PSRAM，失败则退到内部 RAM —— 只有 PSRAM 配置不对时也不会“静默失败” */
    s_line_buf = (uint16_t *)heap_caps_malloc(BSP_LCD_H_RES * sizeof(uint16_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_line_buf == NULL) {
        ESP_LOGW(TAG, "PSRAM 行缓冲分配失败，改用内部 RAM（说明 PSRAM 可能没配置对）");
        s_line_buf = (uint16_t *)heap_caps_malloc(BSP_LCD_H_RES * sizeof(uint16_t),
                                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_line_buf == NULL) {
        ESP_LOGE(TAG, "行缓冲分配失败，无法整屏填色");
    }
    return s_line_buf;
}

void lcd_set_color(uint16_t color)
{
    if (panel_handle == NULL) {
        ESP_LOGE(TAG, "面板尚未初始化");
        return;
    }

    uint16_t *buffer = lcd_get_line_buffer();
    if (buffer == NULL) {
        return;
    }

    for (size_t i = 0; i < BSP_LCD_H_RES; i++) {
        buffer[i] = color;
    }
    for (int y = 0; y < BSP_LCD_V_RES; y++) {
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, BSP_LCD_H_RES, y + 1, buffer);
    }

    /* draw_bitmap 是异步的（内部 SPI 队列），等它把缓冲发完再返回 */
    vTaskDelay(pdMS_TO_TICKS(30));
}

void lcd_draw_picture(int x_start, int y_start, int x_end, int y_end, const unsigned char *gImage)
{
    if (panel_handle == NULL || gImage == NULL) {
        return;
    }
    size_t pixels_byte_size = (size_t)(x_end - x_start) * (y_end - y_start) * 2;
    uint16_t *pixels = (uint16_t *)heap_caps_malloc(pixels_byte_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        pixels = (uint16_t *)heap_caps_malloc(pixels_byte_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (pixels == NULL) {
        ESP_LOGE(TAG, "图片缓冲分配失败（%u 字节）", (unsigned)pixels_byte_size);
        return;
    }
    memcpy(pixels, gImage, pixels_byte_size);
    esp_lcd_panel_draw_bitmap(panel_handle, x_start, y_start, x_end, y_end, pixels);
    vTaskDelay(pdMS_TO_TICKS(20));
    free(pixels);
}

esp_err_t lcd_draw_bitmap(int x, int y, int w, int h, const void *pixels)
{
    if (panel_handle == NULL || pixels == NULL || w <= 0 || h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x < 0 || y < 0 || (x + w) > BSP_LCD_H_RES || (y + h) > BSP_LCD_V_RES) {
        ESP_LOGE(TAG, "绘制区域越界: (%d,%d) %dx%d", x, y, w, h);
        return ESP_ERR_INVALID_ARG;
    }
    /* 直接交给 esp_lcd，不做任何拷贝 */
    return esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + w, y + h, pixels);
}

/* ============================================================
 *  LVGL 暂停/恢复（开机动画直接写屏时用）
 * ============================================================ */
esp_err_t lcd_lvgl_pause(bool pause)
{
    if (disp == NULL) {
        return ESP_OK;          /* LVGL 还没起来，不需要暂停 */
    }

    if (pause) {
        if (s_lvgl_paused) {
            return ESP_OK;
        }
        if (!lvgl_port_lock(0)) {       /* 一直持有锁 -> lvgl 任务就不会刷新了 */
            return ESP_ERR_TIMEOUT;
        }
        s_lvgl_paused = true;
    } else {
        if (!s_lvgl_paused) {
            return ESP_OK;
        }
        s_lvgl_paused = false;
        lvgl_port_unlock();
    }
    return ESP_OK;
}

/* ============================================================
 *  LVGL 显示接口
 * ============================================================ */
static lv_disp_t *lcd_display_lvgl_init(void)
{
    esp_err_t ret = lcd_panel_new();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "液晶屏初始化失败: %s", esp_err_to_name(ret));
        return NULL;
    }

    /* 打开“显示”和背光（后面的开机动画才看得见） */
    esp_lcd_panel_disp_on_off(panel_handle, true);
    lcd_backlight_on();

    lcd_set_color(0xffff);                  /* 先刷白，避免上电时的乱码 */

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io_handle,
        .panel_handle  = panel_handle,
        .buffer_size   = BSP_LCD_H_RES * BSP_LCD_DRAW_BUF_HEIGHT,   /* 单位：像素 */
        .double_buffer = true,
        .hres          = BSP_LCD_H_RES,
        .vres          = BSP_LCD_V_RES,
        .monochrome    = false,
        .color_format  = LV_COLOR_FORMAT_RGB565,   /* LVGL9 / lvgl_port 2.x 必填 */
        .rotation = {
            /* 必须和 lcd_panel_new() 里的 swap_xy / mirror 保持一致 */
            .swap_xy  = true,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma    = true,
            .buff_spiram = false,
            /* ★ LVGL9 的 RGB565 在内存里是小端，ST7789 走 SPI 需要大端，
             *   不开这个开关颜色会红蓝互换（旧工程用 LVGL8 时由 LV_COLOR_16_SWAP 处理） */
            .swap_bytes  = true,
        }
    };

    lv_disp_t *d = lvgl_port_add_disp(&disp_cfg);
    if (d == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp 失败（内存不足或配置非法）");
    }
    return d;
}

/* ============================================================
 *  触摸屏
 * ============================================================ */
esp_err_t lcd_touch_new(esp_lcd_touch_handle_t *ret_touch)
{
    i2c_master_bus_handle_t bus_handle = bsp_i2c_get_bus_handle();
    if (bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C 总线未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    /* 先探测一下触摸芯片在不在，避免后面报一堆难懂的错 */
    esp_err_t err = i2c_master_probe(bus_handle, BSP_TOUCH_I2C_ADDR, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "触摸芯片(0x%02X)无应答: %s", BSP_TOUCH_I2C_ADDR, esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;

    esp_lcd_panel_io_i2c_config_t tp_io_config = {
        .dev_addr            = BSP_TOUCH_I2C_ADDR,
        .scl_speed_hz        = I2C_MASTER_FREQ_HZ,
        .control_phase_bytes = 1,
        .dc_bit_offset       = 0,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
        .flags = {
            .dc_low_on_data = 0,
            .disable_control_phase = 1,
        },
    };

    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(bus_handle, &tp_io_config, &tp_io_handle),
        TAG, "创建触摸 IO 失败");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_V_RES,
        .y_max = BSP_LCD_H_RES,
        .rst_gpio_num = BSP_TOUCH_RST_GPIO,
        .int_gpio_num = BSP_TOUCH_INT_GPIO,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags  = { .swap_xy = 1, .mirror_x = 1, .mirror_y = 0 },
    };

    return esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, ret_touch);
}

bool lcd_touch_pressed(void)
{
    if (tp == NULL) {
        return false;
    }

    /* 读一次触摸芯片（I2C），失败就当“没有触摸”，别影响低功耗主循环 */
    if (esp_lcd_touch_read_data(tp) != ESP_OK) {
        return false;
    }

    uint16_t x     = 0;
    uint16_t y     = 0;
    uint8_t  count = 0;
    if (!esp_lcd_touch_get_coordinates(tp, &x, &y, NULL, &count, 1)) {
        return false;
    }
    return (count > 0);
}

static lv_indev_t *lcd_display_indev_init(lv_disp_t *disp)
{
    /* 触摸失败不能 abort（否则会不断重启），只警告并继续跑显示 */
    esp_err_t err = lcd_touch_new(&tp);
    if (err != ESP_OK || tp == NULL) {
        ESP_LOGW(TAG, "触摸屏初始化失败(%s)，本次不注册触摸输入", esp_err_to_name(err));
        return NULL;
    }

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp   = disp,
        .handle = tp,
    };
    return lvgl_port_add_touch(&touch_cfg);
}

/* ============================================================
 *  LVGL 初始化入口
 * ============================================================ */
void lcd_init(void)
{
    ESP_LOGI(TAG, "开始 LVGL 初始化...");

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();

    /* ------------------------------------------------------------------
     * LVGL 任务的核与优先级（默认值在这里不合适，必须改）：

     * 默认 task_affinity = -1（不绑核），而 lcd_init() 是在 **main 任务**里跑的，
     * main 任务被 sdkconfig 钉在 CPU0（CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0），
     * 于是 LVGL 任务也落在 CPU0，和 app_wifi(5)、app_net(5) 抢同一个核，
     * 而它自己的优先级是 4 —— **只要网络任务在跑 TLS/HTTP，LVGL 就被饿死**。
     * 现象：识别结果和 AI 回复的聊天气泡一起冒出来、录音中的状态字也不刷新、
     * 触摸发涩，整机“反应变慢”。

     * 改成：绑到 APP 核（CPU1），优先级 5。
     *   CPU1：app_audio(6) > taskLVGL(5) > app_key(4)  → 录音实时性仍然最高
     *   CPU0：app_wifi(5) / app_net(5) 单独跑，不再和界面抢 CPU
     * ------------------------------------------------------------------ */
    lvgl_cfg.task_affinity = LCD_LVGL_TASK_CORE;
    lvgl_cfg.task_priority = LCD_LVGL_TASK_PRIO;

    esp_err_t ret = lvgl_port_init(&lvgl_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init 失败: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "lvgl_port_init 成功");

    ESP_LOGI(TAG, "初始化液晶屏...");
    disp = lcd_display_lvgl_init();
    if (disp == NULL) {
        ESP_LOGE(TAG, "液晶屏初始化失败");
        return;
    }
    ESP_LOGI(TAG, "液晶屏初始化成功");

    ESP_LOGI(TAG, "初始化触摸屏...");
    disp_indev = lcd_display_indev_init(disp);
    if (disp_indev == NULL) {
        ESP_LOGW(TAG, "触摸屏未启用");
    } else {
        ESP_LOGI(TAG, "触摸屏初始化成功");
    }

    ESP_LOGI(TAG, "打开背光...");
    lcd_backlight_on();

    ESP_LOGI(TAG, "LVGL 初始化完成");
}

lv_display_t *lcd_get_display(void)
{
    return disp;
}

esp_lcd_panel_handle_t lcd_get_panel(void)
{
    return panel_handle;
}
