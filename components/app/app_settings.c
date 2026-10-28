/**
 * @file    app_settings.c
 * @brief   用户设置持久化实现（TF 卡 FATFS 上的小文本文件）
 *
 * ---------------------------------------------------------------------------
 * 这次踩的坑（完整版，现场日志）：
 *   W APP_SET: /sd/chat_settings.txt（157 字节）里没有 backlight= 这一项，用默认值
 *   W APP_SET: 第一行原始内容：「」
 *   W APP_SET: 写完自检不一致：文件里读到 -1，本次写入的是 68
 *   把卡插到电脑上：文件长度 157 字节（正好是我们写的 5 行），**内容是空的**。
 *
 * 真凶不是"文件系统调用方式"，而是**缓冲放错了内存**（详见 sdkconfig.defaults 的长注释）：
 *   FatFS 在 CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y（IDF 默认值）时
 *   把扇区窗口 / 每文件缓冲分配到 **PSRAM**；
 *   而 ESP32-S3 的 SDMMC 驱动在写卡前**不会**把 CPU cache 刷回内存
 *   （sdmmc_transaction.c 里的 esp_cache_msync 被
 *    `#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE` 包着，而这个宏只有 ESP32-P4 定义），
 *   于是 DMA 直接去读 PSRAM，拿到的是还躺在 cache 里的旧数据（表现为整片 0）。
 *   结果：目录项的长度写对了，数据区却是空的。
 *   ⇒ 真正修复：sdkconfig 关掉 CONFIG_FATFS_ALLOC_PREFER_EXTRAM（缓冲回内部 RAM，
 *      S3 的内部 RAM 不经 L1 cache，DMA 天然一致）。这一条对游戏存档同样重要。
 *
 * 这一层仍然保持"最直白"的写法（open/read/write/fsync），好处：
 *   1. 没有 stdio 缓冲、没有 fseek，读到的字节数一目了然；
 *   2. write/read 返回真实读写长度，能立刻发现"说写了 157 字节其实没进去"；
 *   3. 写完用同一套接口回读自检，当场就能知道到底存没存住。
 *
 * 代价：写一半掉电可能留下半个文件。对一个"亮度值"来说可以接受——
 * 读的时候解析失败就用默认值，不会卡住开机。
 * ---------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>                  /* open / O_CREAT ... */
#include <unistd.h>                 /* read / write / fsync / close */
#include <sys/stat.h>               /* stat / struct stat */
#include <errno.h>
#include <inttypes.h>               /* PRIu32 / PRId32（日志里打 seq、超时） */
#include <dirent.h>                 /* 排查用：列 TF 卡根目录 */

#include "app_settings.h"
#include "app_config.h"
#include "app_bus.h"                /* 只在“TF 卡写不进但已存 NVS”时提醒一下界面 */

#include "my_drivers/lcd.h"
#include "my_drivers/sdcard/sdcard.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "APP_SET";

/* ============================ 内部状态 ============================ */

/** 设置文件（就一个目标文件，不再要 .tmp） */
#define SETTINGS_PATH       APP_SETTINGS_PATH

/** 生成出来的设置文本最长长度（5 行 + 中文注释，实测 ~160 字节） */
#define SETTINGS_TEXT_MAX   384

/** 当前亮度（-1 = 还没初始化） */
static int  s_backlight = -1;
/** 自动休眠超时（毫秒；-1 = 还没初始化，0 = 永不） */
static int32_t s_sleep_ms = -1;
/** IMU 唤醒灵敏度档位（-1 = 还没初始化，0/1/2 见 APP_POWER_IMU_SENS_*） */
static int8_t  s_imu_sens = -1;
/** 最近一次写盘是否成功（只用于日志去重，避免反复刷屏） */
static bool s_last_save_ok = true;

/**
 * 保存序号：每存一次 +1，TF 卡文件和 NVS 各存一份。
 * 读的时候取“序号更大的那一份” —— 这样即使拔卡期间改过设置、
 * 之后又插回旧卡，也不会被卡上的旧值覆盖。
 */
static uint32_t s_seq = 0;

/** NVS 句柄（0 = 没打开）与是否可用 */
static nvs_handle_t s_nvs    = 0;
static bool         s_nvs_ok = false;
/** “已存到 NVS”的界面提示只弹一次，别每次调亮度都提醒 */
static bool         s_nvs_hint_shown = false;

/* ---------------------------------------------------------------------------
 * 大一点的缓冲全部放静态区，不放栈上
 *
 * 为什么：保存动作是在【确认】按钮的回调里做的，而回调跑在 **LVGL 任务**里
 * （栈 7KB，且上面还压着 lv_timer_handler 的一串调用）。
 * 如果 [确认] -> app_settings_set_backlight(432B) -> settings_load(896B)
 * -> settings_dump_dir(464B) 各自带一堆几百字节的局部数组，
 * 这一条调用链就要吃掉 1.8KB 栈，很容易把 LVGL 任务顶爆。
 * 放进静态区后，整条链的栈帧都不到 120 字节。
 *
 * 并发性：本模块只有两个调用点（开机的 app_settings_init、界面里点【确认】），
 * 开机那次在界面创建之前就结束了，两者不会并发；而且下面每个缓冲
 * 在一次调用里只被用一次、用完即弃，所以静态复用是安全的。
 * --------------------------------------------------------------------------- */
static char s_write_text[SETTINGS_TEXT_MAX];    /* 要写进卡里的文本 */
static char s_read_buf[SETTINGS_TEXT_MAX];      /* 从卡里读出来的原始内容 */
static char s_first_line[192];                  /* 第一行（日志用） */
static char s_escape_buf[256];                  /* 转义后的第一行（日志用） */
static char s_hex_buf[3 * 24 + 8];              /* 前 24 字节 HEX（日志用） */
static char s_dir_path[320];                    /* 目录项完整路径（排查用） */
static char s_selftest_report[640];             /* 底层自检的结果文本（失败路径才用） */
static char s_probe_report[320];                /* 写入速度探测结果 */

/* ============================ NVS 后端（TF 卡不可用时的兜底） ============================ */

/**
 * @brief  打开 NVS（失败不影响 TF 卡那条路）
 *
 * @note   nvs_flash_init() 是幂等的：bsp_wifi 里后面也会调一次，重复调用返回 ESP_OK。
 *         那两种情况（页满 / 版本不符）按 IDF 惯例擦掉重来一次。
 */
static bool settings_nvs_open(void)
{
#if !APP_SETTINGS_NVS_ENABLE
    return false;
#else
    if (s_nvs != 0) {
        return true;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建（%s），擦除后重来", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化失败：%s（设置就只能靠 TF 卡了）", esp_err_to_name(err));
        return false;
    }

    err = nvs_open(APP_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 打开命名空间 %s 失败：%s", APP_SETTINGS_NVS_NAMESPACE,
                 esp_err_to_name(err));
        s_nvs = 0;
        return false;
    }
    s_nvs_ok = true;
    return true;
#endif
}

/**
 * @brief  从 NVS 读设置
 * @return ESP_OK 读到了；其它（含 ESP_ERR_NVS_NOT_FOUND）表示没有
 */
static esp_err_t settings_nvs_load(int *value, uint32_t *seq)
{
    if (!settings_nvs_open()) {
        return ESP_ERR_INVALID_STATE;
    }

    int32_t   v = 0;
    uint32_t  q = 0;
    esp_err_t err = nvs_get_i32(s_nvs, APP_SETTINGS_NVS_KEY_VALUE, &v);
    if (err != ESP_OK) {
        return err;
    }
    if (nvs_get_u32(s_nvs, APP_SETTINGS_NVS_KEY_SEQ, &q) != ESP_OK) {
        q = 0;
    }

    if (value != NULL) {
        *value = (int)v;
    }
    if (seq != NULL) {
        *seq = q;
    }
    return ESP_OK;
}

/**
 * @brief  把设置写进 NVS（set + commit）
 */
static esp_err_t settings_nvs_store(int value, uint32_t seq)
{
    if (!settings_nvs_open()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_set_i32(s_nvs, APP_SETTINGS_NVS_KEY_VALUE, (int32_t)value);
    if (err == ESP_OK) {
        err = nvs_set_u32(s_nvs, APP_SETTINGS_NVS_KEY_SEQ, seq);
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    return err;
}

/* ============================ 小工具 ============================ */

/** 前置声明：定义在文件末尾，app_settings_init 里要用到 */
static bool settings_file_exists(void);
static void settings_probe_user_file(void);

/**
 * @brief  把亮度夹到 [APP_LCD_BRIGHTNESS_MIN, 100]
 *
 * @note   为什么要下限：滑条可以拖到 0，但 0% 等于屏幕全黑。
 *         如果把这个值存进卡里，下次开机就是黑屏，
 *         而用户在黑屏上既看不到太阳图标也点不到【确认】——等于把机器锁死了。
 *         所以默认下限是 5%（很暗但看得见）。真要“关屏”，
 *         把 app_config.h 里的 APP_LCD_BRIGHTNESS_MIN 改成 0 即可。
 */
static int settings_clamp(int percent)
{
    if (percent < APP_LCD_BRIGHTNESS_MIN) {
        percent = APP_LCD_BRIGHTNESS_MIN;
    }
    if (percent > 100) {
        percent = 100;
    }
    return percent;
}

/**
 * @brief  确保 TF 卡已挂载（没挂就再试一次）
 */
static bool settings_ensure_mounted(void)
{
    if (sdcard_is_mounted()) {
        return true;
    }
    esp_err_t err = sdcard_mount();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TF 卡不可用(%s)，设置无法持久化（挂载点 %s）",
                 esp_err_to_name(err), sdcard_get_mount_point());
        return false;
    }
    ESP_LOGI(TAG, "TF 卡挂载成功：%s", sdcard_get_mount_point());
    return true;
}

/**
 * @brief  把休眠超时归一成三个受支持的档位
 *
 * @note   0 = 永不；45 秒以下算 30 秒；其余算 1 分钟。
 *         这样手改 NVS / 旧固件留下的怪值不会让界面“三个按钮都不高亮”。
 */
static uint32_t settings_sanitize_sleep(uint32_t timeout_ms)
{
    if (timeout_ms == APP_POWER_TIMEOUT_NEVER) {
        return APP_POWER_TIMEOUT_NEVER;
    }
    if (timeout_ms <= 45000u) {
        return APP_POWER_TIMEOUT_30S;
    }
    return APP_POWER_TIMEOUT_60S;
}

/**
 * @brief  把 IMU 灵敏度归一成三档（越界值一律当中档）
 *
 * @note   0 = 弱，1 = 中，2 = 强。手改 NVS 写进来的怪值不会让
 *         设置页“三个按钮都不高亮”。
 */
static uint8_t settings_sanitize_imu_sens(uint8_t level)
{
    if (level >= APP_POWER_IMU_SENS_COUNT) {
        return APP_POWER_IMU_SENS_DEFAULT;
    }
    return level;
}

/**
 * @brief  把一段字节按“可打印原样、其它 \xNN”转义后拼接（用于日志）
 */
static void settings_escape(const char *raw, size_t len, char *out, size_t out_size)
{
    size_t o = 0;

    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';

    for (size_t i = 0; i < len && o + 5 < out_size; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c == '\n' || c == '\r') {
            break;                                  /* 只打第一行的内容 */
        }
        if (c < 0x20 || c > 0x7E) {
            o += (size_t)snprintf(out + o, out_size - o, "\\x%02X", c);
        } else {
            out[o++] = (char)c;
            out[o]   = '\0';
        }
    }
}

/**
 * @brief  把一段字节打成十六进制（最多前 24 字节），看“到底是空的还是全 0”
 */
static void settings_hex(const char *raw, size_t len, char *out, size_t out_size)
{
    size_t o    = 0;
    size_t show = (len > 24) ? 24 : len;

    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';

    for (size_t i = 0; i < show && o + 4 < out_size; i++) {
        o += (size_t)snprintf(out + o, out_size - o, "%02X ", (unsigned char)raw[i]);
    }
}

/**
 * @brief  打印 TF 卡容量（写不进去的时候，第一件要确认的事就是“卡是不是满了”）
 */
static void settings_log_card_space(void)
{
    sdcard_info_t info;

    if (sdcard_get_info(&info) == ESP_OK) {
        ESP_LOGW(TAG, "TF 卡容量：总 %llu KB，剩余 %llu KB",
                 (unsigned long long)(info.total_bytes / 1024),
                 (unsigned long long)(info.free_bytes / 1024));
    } else {
        ESP_LOGW(TAG, "读不到 TF 卡容量信息");
    }
}

/**
 * @brief  列出 TF 卡根目录（只在出问题时调用，用来定位“文件到底在哪/多大”）
 */
static void settings_dump_dir(void)
{
    DIR *dir = opendir(SDCARD_MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGW(TAG, "列不出 %s 目录（errno=%d）", SDCARD_MOUNT_POINT, errno);
        settings_log_card_space();
        return;
    }

    ESP_LOGW(TAG, "TF 卡根目录内容（排查用）：");
    struct dirent *ent;
    int            shown = 0;

    while ((ent = readdir(dir)) != NULL && shown < 24) {
        if (ent->d_name[0] == '.') {
            continue;                               /* . 和 .. 跳过 */
        }
        struct stat st;
        snprintf(s_dir_path, sizeof(s_dir_path), "%s/%s", SDCARD_MOUNT_POINT, ent->d_name);
        if (stat(s_dir_path, &st) == 0) {
            ESP_LOGW(TAG, "  %-30s %8ld 字节%s", ent->d_name, (long)st.st_size,
                     S_ISDIR(st.st_mode) ? "（目录）" : "");
        } else {
            ESP_LOGW(TAG, "  %-30s (stat 失败, errno=%d)", ent->d_name, errno);
        }
        shown++;
    }
    closedir(dir);
    settings_log_card_space();
}

/* ============================ 文本组装 / 解析 ============================ */

/**
 * @brief  组装要写进卡里的文本
 * @return 写入长度；<=0 或 >=size 表示失败
 *
 * @note   seq 是保存序号：和 NVS 里那份比大小，决定开机时听谁的。
 */
static int settings_build_text(char *buf, size_t size, int value, uint32_t seq)
{
    return snprintf(buf, size,
                    "%s 1\n"
                    "# 对话掌机的用户设置，由固件自动生成\n"
                    "# 删掉本文件即恢复默认亮度 %d%%\n"
                    "# backlight 取值范围 %d~100\n"
                    "backlight=%d\n"
                    "seq=%" PRIu32 "\n",
                    APP_SETTINGS_MAGIC, APP_LCD_BRIGHTNESS_DEFAULT,
                    APP_LCD_BRIGHTNESS_MIN, value, seq);
}

/**
 * @brief  解析设置文本
 *
 * @param  buf        文本（**会被就地改写**：换行符换成 '\0'）
 * @param  len        文本长度
 * @param  magic_ok   输出：第一行是不是我们的文件头
 * @param  first_line 输出：第一行的内容（给日志看）
 * @param  first_size first_line 缓冲大小
 * @param  seq_out    输出：文件里的保存序号（老文件没有这一项时给 0）
 * @return 解析出的亮度；没有 backlight= 这一项返回 -1
 *
 * @note   解析故意写得很宽松：只要有 `backlight=N` 就用它，
 *         文件头只作为提示。因为之前吃过亏——用记事本改这个文件会加 UTF-8 BOM，
 *         旧代码一看到文件头不对就把整个文件丢掉，表现成“写能写、开机读不出来”。
 */
static int settings_parse(char *buf, int len, bool *magic_ok,
                          char *first_line, size_t first_size, uint32_t *seq_out)
{
    int value = -1;
    bool got_first = false;

    if (magic_ok != NULL) {
        *magic_ok = (len >= (int)strlen(APP_SETTINGS_MAGIC)) &&
                    (strncmp(buf, APP_SETTINGS_MAGIC, strlen(APP_SETTINGS_MAGIC)) == 0);
    }
    if (first_line != NULL && first_size > 0) {
        first_line[0] = '\0';
    }
    if (seq_out != NULL) {
        *seq_out = 0;
    }

    /* 就地把换行符变成 '\0'，这样每行都是一个 C 字符串 */
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n' || buf[i] == '\r') {
            buf[i] = '\0';
        }
    }

    int pos = 0;
    while (pos < len) {
        const char *line = buf + pos;
        size_t      seg  = strlen(line);

        if (seg > 0) {
            if (!got_first && first_line != NULL && first_size > 0) {
                snprintf(first_line, first_size, "%s", line);
                got_first = true;
            }
            const char *key = strstr(line, "backlight");
            if (key != NULL) {
                const char *eq = strchr(key, '=');
                if (eq != NULL) {
                    int v = -1;
                    if (sscanf(eq + 1, "%d", &v) == 1) {
                        value = v;
                    }
                }
            }

            const char *seq_key = strstr(line, "seq");
            if (seq_key != NULL && seq_out != NULL) {
                const char *eq = strchr(seq_key, '=');
                if (eq != NULL) {
                    unsigned long q = 0;
                    if (sscanf(eq + 1, "%lu", &q) == 1) {
                        *seq_out = (uint32_t)q;
                    }
                }
            }
        }
        pos += (int)seg + 1;
    }
    return value;
}

/* ============================ 读写文件 ============================ */

/**
 * @brief  读整个设置文件
 *
 * @param  buf  输出缓冲（会补 '\0'）
 * @param  size 缓冲大小
 * @return >=0 读到的字节数；<0 失败
 *
 * @note   这里会对比“目录项里的长度”和“实际读到的字节数”，
 *         两者不一致（尤其是读到 0 字节）就是“长度对、数据没落盘”的现场，
 *         必须打出来——否则会被误判成“文件格式不对”。
 */
static int settings_read_file(const char *path, char *buf, size_t size)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        ESP_LOGI(TAG, "还没有文件 %s（errno=%d）", path, errno);
        return -1;
    }
    if (st.st_size <= 0) {
        ESP_LOGW(TAG, "%s 是空文件（目录项 0 字节）", path);
        settings_dump_dir();
        return -1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        ESP_LOGW(TAG, "open(%s, 只读) 失败：errno=%d", path, errno);
        return -1;
    }

    int n   = read(fd, buf, size - 1);
    int err = errno;
    close(fd);

    if (n < 0) {
        ESP_LOGW(TAG, "read(%s) 失败：errno=%d", path, err);
        return -1;
    }
    buf[n] = '\0';

    if (n != (int)st.st_size) {
        settings_hex(buf, (size_t)n, s_hex_buf, sizeof(s_hex_buf));
        ESP_LOGW(TAG, "%s 目录项说 %ld 字节，实际只读到 %d 字节（数据区没落盘/簇链断了）",
                 path, (long)st.st_size, n);
        ESP_LOGW(TAG, "读到的内容前 %d 字节（HEX）：%s", (n > 24) ? 24 : n, s_hex_buf);
    }
    return n;
}

/**
 * @brief  覆盖写整个设置文件（open -> write -> fsync -> close）
 */
static esp_err_t settings_write_file(const char *text, int len)
{
    /* 直接覆盖写目标文件：不用 .tmp+rename（原因见文件头注释） */
    int fd = open(SETTINGS_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        ESP_LOGW(TAG, "open(%s, 覆盖写) 失败：errno=%d（写保护？卡满？）", SETTINGS_PATH, errno);
        return ESP_FAIL;
    }

    int written = write(fd, text, (size_t)len);
    if (written != len) {
        ESP_LOGW(TAG, "write 只写了 %d/%d 字节：errno=%d", written, len, errno);
        close(fd);
        return ESP_FAIL;
    }

    /* fsync：让数据真的落到卡上，而不是停在驱动/卡内部缓存里
     * （只 close 的话，个别卡复位后会变成 0 字节文件） */
    if (fsync(fd) != 0) {
        ESP_LOGW(TAG, "fsync 失败：errno=%d（数据可能只在缓存里）", errno);
    }
    if (close(fd) != 0) {
        ESP_LOGW(TAG, "close 失败：errno=%d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief  从 TF 卡的设置文件里读亮度（+ 保存序号）
 *
 * @param  value_out 输出亮度（可为 NULL）
 * @param  seq_out   输出序号（可为 NULL）
 * @return 0~100；失败/文件里没有这一项返回 -1
 */
static int settings_load_tf(int *value_out, uint32_t *seq_out)
{
    int n = settings_read_file(SETTINGS_PATH, s_read_buf, sizeof(s_read_buf));
    if (n <= 0) {
        return -1;
    }

    bool     magic_ok = false;
    uint32_t seq      = 0;
    int      value    = settings_parse(s_read_buf, n, &magic_ok, s_first_line,
                                       sizeof(s_first_line), &seq);

    if (value < 0) {
        settings_escape(s_first_line, strlen(s_first_line), s_escape_buf, sizeof(s_escape_buf));
        ESP_LOGW(TAG, "%s（读到 %d 字节）里没有 backlight= 这一项", SETTINGS_PATH, n);
        ESP_LOGW(TAG, "第一行原始内容：「%s」", s_escape_buf);
        settings_dump_dir();
        return -1;
    }

    if (value_out != NULL) {
        *value_out = value;
    }
    if (seq_out != NULL) {
        *seq_out = seq;
    }

    if (!magic_ok) {
        settings_escape(s_first_line, strlen(s_first_line), s_escape_buf, sizeof(s_escape_buf));
        ESP_LOGW(TAG, "%s 文件头不是 %s（但 backlight= 能解析），按 %d 使用",
                 SETTINGS_PATH, APP_SETTINGS_MAGIC, value);
        ESP_LOGW(TAG, "第一行原始内容：「%s」", s_escape_buf);
    } else {
        ESP_LOGI(TAG, "读取 %s：%d 字节，backlight=%d", SETTINGS_PATH, n, value);
    }
    return value;
}

/* ============================ 对外接口 ============================ */

esp_err_t app_settings_init(void)
{
    /* 先把“这份固件到底是怎么编的”打出来：排查磁盘问题时，
     * 第一件事就是确认跑的是不是带修复的固件（省得白折腾一轮）。 */
#ifdef CONFIG_FATFS_ALLOC_PREFER_EXTRAM
    ESP_LOGW(TAG, "编译配置：FatFS 缓冲在 PSRAM（FATFS_ALLOC_PREFER_EXTRAM=y）——"
                  "ESP32-S3 上写卡会丢数据，必须把它改成 n 并重新编译烧写！");
#else
    ESP_LOGI(TAG, "编译配置：FatFS 缓冲在内部 RAM（FATFS_ALLOC_PREFER_EXTRAM=n）——正确");
#endif

    /* ---------- 1. 两个后端都读一遍，取“序号更大的那一份” ----------
     * TF 卡：人类可读、能直接改；NVS：卡坏/没插卡时的兜底。
     * 两份都带 seq（每存一次 +1），所以拔卡期间改过设置、之后又插回旧卡，
     * 也不会被卡上的旧值顶掉。 */
    int      v_tf = -1, v_nv = -1;
    uint32_t q_tf = 0,  q_nv = 0;

    if (settings_ensure_mounted()) {
        v_tf = settings_load_tf(NULL, &q_tf);
        if (v_tf >= 0) {
            ESP_LOGI(TAG, "TF 卡里有设置：backlight=%d（序号 %" PRIu32 "）", v_tf, q_tf);
        } else if (settings_file_exists()) {
            /* 文件在、却读不出内容 -> 跑一次底层自检，定位到“卡写不进去”还是“文件系统层” */
            ESP_LOGW(TAG, "设置文件存在但读不出内容，做一次 TF 卡底层自检 ...");
            if (sdcard_selftest(s_selftest_report, sizeof(s_selftest_report)) != ESP_OK) {
                ESP_LOGW(TAG, "底层自检没跑起来：%s", s_selftest_report);
            }
            /* 再做“写入速度阶梯探测”：读正常、写被吞时，降频常常能救回来 */
            if (sdcard_write_probe(s_probe_report, sizeof(s_probe_report)) != ESP_OK) {
                ESP_LOGW(TAG, "写入速度探测没跑起来：%s", s_probe_report);
            }
            /* 再读一张“电脑写进去”的文件做对照：能读出来 = 读没问题，是写不进去 */
            settings_probe_user_file();
        }
    } else {
        ESP_LOGW(TAG, "TF 卡不可用，只看内部 NVS");
    }

    if (settings_nvs_load(&v_nv, &q_nv) == ESP_OK) {
        ESP_LOGI(TAG, "NVS 里有设置：backlight=%d（序号 %" PRIu32 "）", v_nv, q_nv);
    } else {
        v_nv = -1;
        ESP_LOGI(TAG, "NVS 里还没有设置");
    }

    int         value = -1;
    const char *where = "默认值";

    if (v_tf >= 0 && v_nv >= 0) {
        if (q_nv > q_tf) {
            value = v_nv;
            where = "NVS（比 TF 卡新）";
        } else {
            value = v_tf;
            where = "TF 卡";
        }
    } else if (v_tf >= 0) {
        value = v_tf;
        where = "TF 卡";
    } else if (v_nv >= 0) {
        value = v_nv;
        where = "NVS";
    }

    s_seq       = (q_tf > q_nv) ? q_tf : q_nv;
    s_backlight = settings_clamp((value >= 0) ? value : APP_LCD_BRIGHTNESS_DEFAULT);

    /* ---------- 1.5 休眠超时（只存 NVS，和亮度互不影响） ---------- */
    s_sleep_ms = (int32_t)APP_POWER_TIMEOUT_DEFAULT_MS;
    if (settings_nvs_open()) {
        uint32_t  ms = 0;
        esp_err_t e  = nvs_get_u32(s_nvs, APP_SETTINGS_NVS_KEY_SLEEP_MS, &ms);
        if (e == ESP_OK) {
            s_sleep_ms = (int32_t)settings_sanitize_sleep(ms);
            ESP_LOGI(TAG, "NVS 里的休眠超时：%" PRIu32 " ms（归一后 %" PRId32 " ms）",
                     ms, s_sleep_ms);
        } else {
            ESP_LOGI(TAG, "NVS 里还没有休眠设置，用默认 %" PRIu32 " ms",
                     (uint32_t)APP_POWER_TIMEOUT_DEFAULT_MS);
        }
    }

    /* ---------- 1.6 IMU 唤醒灵敏度（同样只存 NVS，默认中档） ---------- */
    s_imu_sens = (int8_t)APP_POWER_IMU_SENS_DEFAULT;
    if (settings_nvs_open()) {
        uint32_t  lvl = 0;
        esp_err_t e   = nvs_get_u32(s_nvs, APP_SETTINGS_NVS_KEY_IMU_SENS, &lvl);
        if (e == ESP_OK) {
            s_imu_sens = (int8_t)settings_sanitize_imu_sens((uint8_t)lvl);
            ESP_LOGI(TAG, "NVS 里的 IMU 唤醒灵敏度：%" PRIu32 "（归一后 %d）",
                     lvl, (int)s_imu_sens);
        } else {
            ESP_LOGI(TAG, "NVS 里还没有 IMU 灵敏度设置，用默认中档");
        }
    }

    /* ---------- 2. 立即应用（越早越好：lcd_init 里背光是 100%） ---------- */
    esp_err_t err = lcd_brightness_set(s_backlight);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置背光失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "背光已应用：%d%%（来源：%s，序号 %" PRIu32 "；范围 %d~100，默认 %d）",
             s_backlight, where, s_seq, APP_LCD_BRIGHTNESS_MIN, APP_LCD_BRIGHTNESS_DEFAULT);
    return ESP_OK;
}

int app_settings_get_backlight(void)
{
    if (s_backlight < 0) {
        return settings_clamp(APP_LCD_BRIGHTNESS_DEFAULT);
    }
    return s_backlight;
}

esp_err_t app_settings_set_backlight(int percent, bool persist)
{
    s_backlight = settings_clamp(percent);

    /* 先让屏幕立刻变（用户拖动滑条要跟手），再考虑存盘 */
    esp_err_t err = lcd_brightness_set(s_backlight);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置背光失败: %s", esp_err_to_name(err));
        return err;
    }

    if (!persist) {
        return ESP_OK;
    }

    /* ---------- 保存：TF 卡 + NVS 两边都写 ----------
     * 顺序：先 TF（人类可读、能直接改），失败就靠 NVS 兜底。
     * 两边都带同一个 seq，开机时按 seq 取新的那一份。 */
    uint32_t  seq    = s_seq + 1;
    bool      tf_ok  = false;
    bool      nvs_ok = false;
    esp_err_t tf_err = ESP_ERR_INVALID_STATE;

    int len = settings_build_text(s_write_text, sizeof(s_write_text), s_backlight, seq);
    if (len > 0 && (size_t)len < sizeof(s_write_text)) {
        if (settings_ensure_mounted()) {
            tf_err = settings_write_file(s_write_text, len);
            if (tf_err == ESP_OK) {
                /* 写完自检：立刻回读一遍，把“报成功其实没写进去”当场暴露出来
                 * （实测有卡会静默吞掉数据：目录项长度对、数据区全 0）。 */
                int      check = -1;
                uint32_t q     = 0;
                check = settings_load_tf(&check, &q);
                if (check == s_backlight) {
                    tf_ok = true;
                    ESP_LOGD(TAG, "TF 卡写完自检通过：%d%%（序号 %" PRIu32 "）", check, q);
                } else {
                    ESP_LOGW(TAG, "TF 卡写完自检不一致：文件里读到 %d，本次写入的是 %d",
                             check, s_backlight);
                    settings_dump_dir();
                    /* 卡写不进去时顺便做一次底层自检，把“卡坏了”这件事说清楚 */
                    if (sdcard_selftest(s_selftest_report, sizeof(s_selftest_report)) != ESP_OK) {
                        ESP_LOGW(TAG, "底层自检没跑起来：%s", s_selftest_report);
                    }
                    tf_err = ESP_FAIL;
                }
            }
        } else {
            ESP_LOGD(TAG, "没有可用的 TF 卡，只写 NVS");
        }
    } else {
        ESP_LOGE(TAG, "设置文本组装失败（缓冲 %u 字节）", (unsigned)sizeof(s_write_text));
    }

    /* NVS：始终写一份（正常情况下只多花几毫秒，换来“拔卡也不丢设置”） */
    esp_err_t nvs_err = settings_nvs_store(s_backlight, seq);
    nvs_ok = (nvs_err == ESP_OK);
    if (!nvs_ok) {
        ESP_LOGW(TAG, "写 NVS 失败：%s", esp_err_to_name(nvs_err));
    }

    if (tf_ok || nvs_ok) {
        s_seq = seq;
    }

    esp_err_t werr = (tf_ok || nvs_ok) ? ESP_OK : ESP_FAIL;

    /* ---------- 日志 + 首次在界面上如实说明存在哪儿 ---------- */
    if (tf_ok && nvs_ok) {
        if (!s_last_save_ok) {
            ESP_LOGI(TAG, "TF 卡恢复可用，设置已保存");
        }
        ESP_LOGI(TAG, "背光设置已保存：%d%%（TF 卡 %d 字节 + NVS 镜像，序号 %" PRIu32 "）",
                 s_backlight, len, seq);
    } else if (nvs_ok) {
        ESP_LOGW(TAG, "TF 卡写不进去(%s)，背光设置已存到内部 NVS：%d%%（序号 %" PRIu32 "）",
                 esp_err_to_name(tf_err), s_backlight, seq);
        if (!s_nvs_hint_shown) {
            s_nvs_hint_shown = true;
            /* 只提示一次：亮度确实记住了，只是没存在卡上 */
            app_bus_post_chat(CHAT_MSG_ERROR, "TF卡写不进，已存到内部存储");
        }
    } else if (tf_ok) {
        ESP_LOGW(TAG, "背光设置已存到 TF 卡：%d%%（NVS 写入失败，拔卡会丢）", s_backlight);
    } else {
        if (s_last_save_ok) {
            ESP_LOGW(TAG, "背光设置保存失败（TF 卡和 NVS 都写不进去），"
                          "本次运行仍有效，重启后回到 %d%%", APP_LCD_BRIGHTNESS_DEFAULT);
        }
    }
    s_last_save_ok = (tf_ok || nvs_ok);
    return werr;
}

esp_err_t app_settings_save(void)
{
    if (s_backlight < 0) {
        s_backlight = settings_clamp(APP_LCD_BRIGHTNESS_DEFAULT);
    }
    return app_settings_set_backlight(s_backlight, true);
}

/** TF 卡当前是否可用（挂载成功） */
bool app_settings_sd_ready(void)
{
    return sdcard_is_mounted();
}

/* ============================ 自动休眠超时 ============================ */

uint32_t app_settings_get_sleep_timeout_ms(void)
{
    if (s_sleep_ms < 0) {
        return APP_POWER_TIMEOUT_DEFAULT_MS;
    }
    return (uint32_t)s_sleep_ms;
}

esp_err_t app_settings_set_sleep_timeout_ms(uint32_t timeout_ms)
{
    uint32_t ms = settings_sanitize_sleep(timeout_ms);
    s_sleep_ms  = (int32_t)ms;

    if (!settings_nvs_open()) {
        ESP_LOGW(TAG, "NVS 不可用，休眠设置只在本次运行有效");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_set_u32(s_nvs, APP_SETTINGS_NVS_KEY_SLEEP_MS, ms);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "保存休眠设置失败：%s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "休眠超时已保存：%s（%" PRIu32 " ms）",
             (ms == APP_POWER_TIMEOUT_NEVER) ? "永不自动休眠"
                                             : ((ms >= APP_POWER_TIMEOUT_60S) ? "1 分钟" : "30 秒"),
             ms);
    return ESP_OK;
}

/* ============================ IMU 唤醒灵敏度 ============================ */

uint8_t app_settings_get_imu_sensitivity(void)
{
    if (s_imu_sens < 0) {
        return APP_POWER_IMU_SENS_DEFAULT;
    }
    return settings_sanitize_imu_sens((uint8_t)s_imu_sens);
}

esp_err_t app_settings_set_imu_sensitivity(uint8_t level)
{
    uint8_t lvl = settings_sanitize_imu_sens(level);
    s_imu_sens  = (int8_t)lvl;

    if (!settings_nvs_open()) {
        ESP_LOGW(TAG, "NVS 不可用，IMU 灵敏度只在本次运行有效");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_set_u32(s_nvs, APP_SETTINGS_NVS_KEY_IMU_SENS, (uint32_t)lvl);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "保存 IMU 灵敏度失败：%s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "IMU 唤醒灵敏度已保存：%s（%u）",
             (lvl == APP_POWER_IMU_SENS_WEAK)   ? "弱"
             : (lvl == APP_POWER_IMU_SENS_STRONG) ? "强"
                                                  : "中",
             (unsigned)lvl);
    return ESP_OK;
}

/**
 * @brief  读一张“由电脑写进去”的文件，看看**读路径**是否正常
 *
 * @note   用户如果在卡上放一个 /sd/test.txt，这里会把它的内容打出来。
 *         这是判断“是写坏了还是读坏了”的关键对照：
 *           - test.txt（电脑写的）能正常读出来 -> 读没问题，是写不进去；
 *           - test.txt 也读成空/乱码       -> 读路径有问题，或者卡上数据本来就不对。
 */
static void settings_probe_user_file(void)
{
    static const char *probe = "/sd/test.txt";

    int n = settings_read_file(probe, s_read_buf, sizeof(s_read_buf));
    if (n < 0) {
        ESP_LOGW(TAG, "读 %s 失败（电脑写进去的文件也读不出来？）", probe);
        return;
    }

    /* 把内容按行打出来（最多 4 行），空行也打，方便看清是不是全 0 */
    ESP_LOGW(TAG, "读 %s：%d 字节，内容如下（「」里是原文）：", probe, n);
    int    line = 0;
    size_t pos  = 0;
    while (pos < (size_t)n && line < 4) {
        size_t end = pos;
        while (end < (size_t)n && s_read_buf[end] != '\n' && s_read_buf[end] != '\r') {
            end++;
        }
        char   esc[256] = {0};
        bool   all_zero = true;
        for (size_t i = pos; i < end; i++) {
            if (s_read_buf[i] != 0) {
                all_zero = false;
                break;
            }
        }
        if (all_zero) {
            ESP_LOGW(TAG, "  第 %d 行：全是 0x00（%u 字节）", line + 1, (unsigned)(end - pos));
        } else {
            settings_escape(s_read_buf + pos, end - pos, esc, sizeof(esc));
            ESP_LOGW(TAG, "  第 %d 行：「%s」", line + 1, esc);
        }
        line++;
        pos = (end < (size_t)n) ? end + 1 : (size_t)n;
    }
}

/**
 * @brief  设置文件是否存在（用来区分“从没保存过”和“保存了但内容不对”）
 */
static bool settings_file_exists(void)
{
    struct stat st;

    return (stat(SETTINGS_PATH, &st) == 0) && (st.st_size > 0);
}
