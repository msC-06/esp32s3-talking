/**
 * @file    app_gb2312.c
 * @brief   UTF-8 / GB2312 转换与文本宽度工具实现
 *
 * 分层：app 层，调用 my_drivers 的字库公开接口 hzk16_utf8_to_gb2312()，
 *       不直接碰底层字库表（表在 my_drivers 内部）。
 */

#include <string.h>

#include "app_gb2312.h"

#include "esp_attr.h"               /* EXT_RAM_BSS_ATTR：临时缓冲放 PSRAM */
#include "my_drivers/font_hzk16/hzk16.h"

/** 宽度估算时使用的临时 GB2312 缓冲（放静态区：别在网络/LVGL 任务栈上占 4KB） */
#define GB_TMP_MAX      4096U

/**
 * 转换用的静态缓冲
 * @note  只有界面线程（LVGL 定时器 / 建界面）会调用宽度估算，所以静态缓冲安全；
 *        缓冲够大，正常长度的气泡文本不会被截断，算出来的宽度才准；
 *        用 EXT_RAM_BSS_ATTR 放 PSRAM，不占内部 RAM。
 */
EXT_RAM_BSS_ATTR static char s_gb_tmp[GB_TMP_MAX];

/* ============================ 内部函数 ============================ */

/**
 * @brief  判断 UTF-8 字符占几个字节（只按首字节判断，用于安全截断/逐字追加）
 */
int app_utf8_char_bytes(unsigned char c)
{
    if (c < 0x80U) {
        return 1;
    }
    if ((c & 0xE0U) == 0xC0U) {
        return 2;
    }
    if ((c & 0xF0U) == 0xE0U) {
        return 3;
    }
    if ((c & 0xF8U) == 0xF0U) {
        return 4;
    }
    return 1;       /* 非法首字节：当成单字节往前走，避免死循环 */
}

/* ============================ 对外接口 ============================ */

size_t utf8_to_gb2312(const char *utf8, char *gb, size_t gb_size)
{
    return hzk16_utf8_to_gb2312(utf8, gb, gb_size);
}

uint32_t app_text_display_width(const char *utf8)
{
    if (utf8 == NULL) {
        return 0;
    }

    size_t n = utf8_to_gb2312(utf8, s_gb_tmp, sizeof(s_gb_tmp));
    if (n == 0) {
        return 0;
    }

    uint32_t width = 0;
    for (size_t i = 0; i < n; ) {
        unsigned char c = (unsigned char)s_gb_tmp[i];
        if (c < 0x80U) {
            width += APP_FONT_HALF_W;
            i     += 1;
        } else {
            width += APP_FONT_FULL_W;
            i     += 2;     /* GB2312 双字节 */
        }
    }
    return width;
}

size_t app_text_truncate_utf8(const char *utf8, char *out, size_t out_size, uint32_t max_width)
{
    if (utf8 == NULL || out == NULL || out_size == 0) {
        return 0;
    }

    size_t          src   = 0;
    size_t          dst   = 0;
    uint32_t        width = 0;
    const uint8_t  *p     = (const uint8_t *)utf8;

    while (p[src] != '\0') {
        int    bytes = app_utf8_char_bytes(p[src]);
        size_t room  = out_size - 1U - dst;         /* 留一个字节放 '\0' */
        if ((size_t)bytes > room) {
            break;                                  /* 输出缓冲放不下 */
        }

        /* 尾巴不完整（多字节字符被上游切开了）：立刻停。
         * 不检查的话下面会把 '\0' 一起拷走，然后 src 越过结尾继续读越界内存。 */
        bool complete = true;
        for (int i = 1; i < bytes; i++) {
            if (p[src + i] == '\0') {
                complete = false;
                break;
            }
        }
        if (!complete) {
            break;
        }

        uint32_t char_w = (p[src] < 0x80U) ? APP_FONT_HALF_W : APP_FONT_FULL_W;
        if (width + char_w > max_width) {
            break;                                  /* 宽度超了 */
        }

        for (int i = 0; i < bytes; i++) {
            out[dst++] = (char)p[src + i];
        }
        src   += (size_t)bytes;
        width += char_w;
    }

    out[dst] = '\0';
    return dst;
}
