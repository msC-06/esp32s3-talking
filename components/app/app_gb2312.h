/**
 * @file    app_gb2312.h
 * @brief   UTF-8 / GB2312 转换与文本宽度工具（app 层）
 *
 * 为什么需要它：
 *   - HZK16 点阵字库是 GB2312 编码（区/位寻址），
 *     而 ASR / LLM 返回的都是 UTF-8 文本，手工取模绘制时必须先转成 GB2312；
 *   - 用 LVGL 的 label + hzk16 字体显示时，字体内部已经做了
 *     Unicode -> GB2312 的转换，所以**显示**不需要本模块；
 *     本模块主要用于按 16x16 点阵估算文本显示宽度、以及按宽度安全截断。
 */

#ifndef __APP_GB2312_H
#define __APP_GB2312_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 估算显示宽度时的单字符像素宽度 */
#define APP_FONT_HALF_W     8U      /*!< 半角（ASCII）字符宽度 */
#define APP_FONT_FULL_W     16U     /*!< 全角（汉字）字符宽度 */

/**
 * @brief  把 UTF-8 字符串转换为 GB2312 编码（字库编码）
 *
 * @param  utf8     输入 UTF-8 字符串
 * @param  gb       输出缓冲区（会补 '\0'）
 * @param  gb_size  输出缓冲区大小
 * @return 写入的字节数（不含 '\0'）；参数非法或缓冲太小返回 0
 *
 * @note   ASCII -> 1 字节；汉字 -> 2 字节（高字节在前）；字库里没有的字符 -> '?'。
 */
size_t utf8_to_gb2312(const char *utf8, char *gb, size_t gb_size);

/**
 * @brief  按首字节判断一个 UTF-8 字符占几个字节
 *
 * @param  first_byte 字符首字节
 * @return 1~4；非法首字节返回 1（当单字节往前走，避免死循环）
 *
 * @note   打字机逐字追加、按字节安全截断时都要用它，
 *         保证永远不会把多字节字符切成两半。
 */
int app_utf8_char_bytes(unsigned char first_byte);

/**
 * @brief  估算一段 UTF-8 文本用 hzk16 渲染需要多少像素宽
 *
 * @param  utf8 UTF-8 文本（可为 NULL，返回 0）
 * @return 估算宽度（像素）
 */
uint32_t app_text_display_width(const char *utf8);

/**
 * @brief  按“显示宽度”安全截断 UTF-8 文本（不会切断多字节字符）
 *
 * @param  utf8      输入文本
 * @param  out       输出缓冲区
 * @param  out_size  输出缓冲区大小
 * @param  max_width 允许的最大显示宽度（像素）
 * @return 实际写入的字节数（不含 '\0'）
 */
size_t app_text_truncate_utf8(const char *utf8, char *out, size_t out_size, uint32_t max_width);

#ifdef __cplusplus
}
#endif

#endif /* __APP_GB2312_H */
