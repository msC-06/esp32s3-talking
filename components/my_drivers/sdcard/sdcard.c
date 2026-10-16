/**
 * @file    sdcard.c
 * @brief   TF 卡驱动实现：探测卡 -> 挂 VFS FatFS（不自动格式化）
 *
 * 关键点：
 *   1. mount_config.format_if_mount_failed 固定为 false —— 避免把 exFAT 卡/别人的卡误格式化；
 *   2. 通过挂载返回值区分两种失败：
 *        ESP_FAIL            -> 卡在，但文件系统不是 FAT（未格式化/exFAT）
 *        其它错误(超时等)     -> 基本可以认为没插卡
 *   3. 失败时 IDF 内部会调用 host deinit，所以不会残留半初始化状态，
 *      之后重新插卡再调一次 sdcard_mount() 即可；
 *   4. 传输方式由 SDCARD_TRANSPORT_SPI 选择（默认 SDMMC，本板接法），
 *      两种方式对上层是同一套接口（挂载点 /sd）。
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "sdcard.h"

#include "bsp/bsp_sdmmc.h"
#if SDCARD_TRANSPORT_SPI
#include "bsp/bsp_sdspi.h"
#endif

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sdcard";

/* ============================ 内部状态（全部 static） ============================ */

static sdmmc_card_t *s_card    = NULL;
static bool          s_mounted = false;

/* ============================ 挂载/卸载 ============================ */

esp_err_t sdcard_mount(void)
{
    if (s_mounted) {
        return ESP_OK;                      /* 已经挂载，直接返回 */
    }

    /* 1. 挂载参数：★ 绝对不允许自动格式化 */
    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,    /* ★ 挂载失败也不格式化，避免误擦卡上数据 */
        .max_files              = SDCARD_MAX_FILES,
        .allocation_unit_size   = 16 * 1024,
    };

    ESP_LOGI(TAG, "正在检测/挂载 TF 卡 ...");

    esp_err_t err;

#if SDCARD_TRANSPORT_SPI
    /* ---- SPI 方式：需要板子按 CS/MOSI/MISO/CLK 接线 ---- */
    esp_err_t bus_err = bsp_sdspi_bus_init();
    if (bus_err != ESP_OK) {
        return bus_err;
    }

    sdmmc_host_t          host;
    sdspi_device_config_t slot;
    bsp_sdspi_get_host(&host);
    bsp_sdspi_get_slot(&slot);

    err = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
#else
    /* ---- SDMMC(SDIO 1 线) 方式：本开发板的接法 ---- */
    sdmmc_host_t        host;
    sdmmc_slot_config_t slot;
    bsp_sdmmc_get_host(&host);
    bsp_sdmmc_get_slot(&slot);

    /* 时钟上限可配（见 sdcard.h 的 SDCARD_MAX_FREQ_KHZ）：
     * 写入被静默丢弃的卡/板组合，降频常常能救回来。 */
    host.max_freq_khz = SDCARD_MAX_FREQ_KHZ;
    ESP_LOGI(TAG, "SDMMC 时钟上限 %" PRIu32 " kHz", (uint32_t)host.max_freq_khz);

    err = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
#endif

    if (err == ESP_FAIL) {
        /* 卡有应答，但 FAT 挂不上：多半是 exFAT 或者没格式化 */
        ESP_LOGE(TAG, "TF 卡文件系统无法挂载（不是 FAT32 或未格式化）；"
                      "已按配置跳过格式化，卡上数据未做任何改动");
        s_card = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK) {
        /* 没插卡 / 接触不良 / 卡损坏，都会走到这里 */
        ESP_LOGW(TAG, "未检测到 TF 卡或卡初始化失败(%s)，跳过挂载", esp_err_to_name(err));
        s_card = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "TF 卡挂载成功: %s", SDCARD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);      /* 打印卡名字/容量/速率 */

    /* 顺便打印剩余空间，方便确认挂的是哪张卡 */
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(SDCARD_MOUNT_POINT, &total, &free_b) == ESP_OK) {
        ESP_LOGI(TAG, "容量: 总 %llu KB / 剩余 %llu KB",
                 (unsigned long long)(total / 1024), (unsigned long long)(free_b / 1024));
    }

    return ESP_OK;
}

esp_err_t sdcard_unmount(void)
{
    if (!s_mounted || s_card == NULL) {
        return ESP_OK;
    }

    esp_err_t err = esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_card);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "TF 卡已卸载，可以安全拔卡");
    } else {
        ESP_LOGE(TAG, "TF 卡卸载失败: %s", esp_err_to_name(err));
    }

    s_card    = NULL;
    s_mounted = false;
    return err;
}

bool sdcard_is_mounted(void)
{
    return s_mounted;
}

const char *sdcard_get_mount_point(void)
{
    return SDCARD_MOUNT_POINT;
}

esp_err_t sdcard_get_info(sdcard_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(info, 0, sizeof(sdcard_info_t));

    if (s_card != NULL) {
        /* CID 里的 NAME 是卡的名字（例如 SD16G） */
        memcpy(info->name, s_card->cid.name, sizeof(s_card->cid.name));
        info->name[sizeof(info->name) - 1] = '\0';
    }

    esp_err_t err = esp_vfs_fat_info(SDCARD_MOUNT_POINT, &info->total_bytes, &info->free_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取容量失败: %s", esp_err_to_name(err));
    }
    return err;
}

/* ============================ 底层写入自检 ============================ */

/** 自检用的扇区大小（SD 卡固定 512） */
#define SDCARD_TEST_SEC_SIZE    512
/** 每个 LBA 位置最多写多少行结果 */
#define SDCARD_TEST_MAX_POS     4

/** 缓冲是否全 0（用来区分“数据丢了”还是“数据被改写”） */
static bool sdcard_buf_all_zero(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != 0) {
            return false;
        }
    }
    return true;
}

/**
 * @brief  在一个 LBA 上做一次「写→回读→比对」，并把原内容还原
 *
 * @param  lba      扇区号
 * @param  src      要写入的缓冲（512 字节）
 * @param  rdbuf    回读缓冲（512 字节）
 * @param  orig     原内容缓冲（512 字节，函数内部会先读进来、结束时写回）
 * @param  why      输出：失败原因（可为 NULL）
 * @param  why_size why 缓冲大小
 * @return true 数据成功落盘
 */
static bool sdcard_test_one_lba(uint32_t lba, const uint8_t *src, uint8_t *rdbuf,
                                uint8_t *orig, char *why, size_t why_size)
{
    if (why != NULL && why_size > 0) {
        why[0] = '\0';
    }

    /* 1. 备份原内容（测完还原，避免动到用户数据） */
    if (sdmmc_read_sectors(s_card, orig, lba, 1) != ESP_OK) {
        if (why != NULL) {
            snprintf(why, why_size, "读原扇区失败");
        }
        return false;
    }

    /* 2. 写特征串 */
    esp_err_t werr = sdmmc_write_sectors(s_card, src, lba, 1);
    if (werr != ESP_OK) {
        if (why != NULL) {
            snprintf(why, why_size, "写失败(%s)", esp_err_to_name(werr));
        }
        sdmmc_write_sectors(s_card, orig, lba, 1);      /* 还原 */
        return false;
    }

    /* 3. 回读比对 */
    memset(rdbuf, 0xAA, SDCARD_TEST_SEC_SIZE);
    bool ok = false;
    if (sdmmc_read_sectors(s_card, rdbuf, lba, 1) != ESP_OK) {
        if (why != NULL) {
            snprintf(why, why_size, "回读失败");
        }
    } else if (memcmp(rdbuf, src, SDCARD_TEST_SEC_SIZE) == 0) {
        ok = true;
    } else if (why != NULL) {
        snprintf(why, why_size, "数据不符(%s)",
                 sdcard_buf_all_zero(rdbuf, SDCARD_TEST_SEC_SIZE) ? "读回全 0，写入被吞掉" : "内容被改写");
    }

    /* 4. 还原原内容 */
    sdmmc_write_sectors(s_card, orig, lba, 1);
    return ok;
}

/**
 * @brief  从 FAT 引导扇区算出“数据区”起始扇区（自检只在数据区里做，绝不动 FAT/根目录）
 */
static uint32_t sdcard_data_start_sector(const uint8_t *bs)
{
    uint16_t bytes_per_sector = (uint16_t)(bs[11] | (bs[12] << 8));
    uint16_t reserved         = (uint16_t)(bs[14] | (bs[15] << 8));
    uint8_t  num_fats         = bs[16];
    uint16_t root_entries     = (uint16_t)(bs[17] | (bs[18] << 8));
    uint32_t fat_size         = (uint32_t)bs[36] | ((uint32_t)bs[37] << 8) |
                                ((uint32_t)bs[38] << 16) | ((uint32_t)bs[39] << 24);

    if (bytes_per_sector == 0) {
        bytes_per_sector = 512;
    }
    if (fat_size == 0) {                                /* FAT16：FAT 大小在偏移 22 */
        fat_size = (uint32_t)(bs[22] | (bs[23] << 8));
    }
    if (num_fats == 0) {
        num_fats = 2;
    }

    uint32_t root_sectors = ((uint32_t)root_entries * 32 + bytes_per_sector - 1) / bytes_per_sector;

    return (uint32_t)reserved + (uint32_t)num_fats * fat_size + root_sectors;
}

esp_err_t sdcard_write_probe(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    if (!s_mounted || s_card == NULL) {
        snprintf(out, out_size, "卡未挂载，无法探测");
        return ESP_ERR_INVALID_STATE;
    }

    /* 时钟档位：20MHz（默认）-> 10 -> 5 -> 1 -> 400kHz（探测模式的最低速） */
    static const uint32_t freqs[] = { 20000, 10000, 5000, 1000, 400 };

    uint8_t *buf_orig = (uint8_t *)heap_caps_malloc(SDCARD_TEST_SEC_SIZE,
                                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *buf_int  = (uint8_t *)heap_caps_aligned_alloc(4, SDCARD_TEST_SEC_SIZE,
                                                          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *buf_read = (uint8_t *)heap_caps_malloc(SDCARD_TEST_SEC_SIZE,
                                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (buf_orig == NULL || buf_int == NULL || buf_read == NULL) {
        free(buf_orig); free(buf_int); free(buf_read);
        snprintf(out, out_size, "缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }

    memset(buf_int, 0, SDCARD_TEST_SEC_SIZE);
    for (size_t i = 0; i + 16 < SDCARD_TEST_SEC_SIZE; i += 16) {
        memcpy(buf_int + i, "SDWRITEPROBE-OK!", 16);
    }

    /* 找一个安全位置：数据区 +1MB（卡几乎空的，那里是空闲簇） */
    uint32_t lba = 0;
    if (sdmmc_read_sectors(s_card, buf_read, 0, 1) == ESP_OK) {
        uint32_t data_start = sdcard_data_start_sector(buf_read);
        if (data_start != 0) {
            lba = data_start + 2048U;
        }
    }
    if (lba == 0 || lba >= (uint32_t)s_card->csd.capacity) {
        lba = (uint32_t)s_card->csd.capacity / 20;
    }

    size_t   used    = 0;
    uint32_t working = 0;

    for (size_t i = 0; i < sizeof(freqs) / sizeof(freqs[0]) && used + 32 < out_size; i++) {
        /* 改主机时钟（set_card_clk 由 host 提供，SDMMC / SPI 通用） */
        if (s_card->host.set_card_clk != NULL) {
            s_card->host.set_card_clk(s_card->host.slot, freqs[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(20));

        /* 顺便问问卡自己状态（CMD13）：卡回 “not ready” 说明它自己卡在错误态 */
        esp_err_t st = sdmmc_get_status(s_card);

        char why[64] = {0};
        bool ok = sdcard_test_one_lba(lba, buf_int, buf_read, buf_orig, why, sizeof(why));

        int n = snprintf(out + used, out_size - used, "%" PRIu32 "kHz:%s%s ",
                         freqs[i], ok ? "OK" : "FAIL",
                         ok ? "" : why);
        if (n <= 0) {
            break;
        }
        used += (size_t)n;

        ESP_LOGW(TAG, "  写入探测 @%" PRIu32 "kHz：%s（卡状态 CMD13=%s，实际时钟 %d kHz）",
                 freqs[i], ok ? "能写进去" : "写不进去",
                 esp_err_to_name(st), s_card->real_freq_khz);

        if (ok && working == 0) {
            working = freqs[i];
            break;                              /* 第一个能写的档位就够了 */
        }
    }

    /* 收尾：能把哪一档写进去就停在那一档（本次运行生效），否则回到配置的默认值 */
    uint32_t final_khz = (working != 0) ? working : (uint32_t)SDCARD_MAX_FREQ_KHZ;
    if (s_card->host.set_card_clk != NULL) {
        s_card->host.set_card_clk(s_card->host.slot, final_khz);
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    if (working != 0) {
        ESP_LOGW(TAG, "写入探测结果：%s —— 已把 SD 时钟停在 %" PRIu32 " kHz（本次运行有效）。"
                      "想每次开机都用它，把 sdcard.h 的 SDCARD_MAX_FREQ_KHZ 改成 %" PRIu32,
                 out, working, working);
    } else {
        ESP_LOGE(TAG, "写入探测结果：%s —— 从 20MHz 到 400kHz 都写不进去，"
                      "基本可以判定这张卡不能写了（换卡或用官方 SD Formatter 覆盖格式化试试）", out);
    }

    free(buf_orig);
    free(buf_int);
    free(buf_read);
    return ESP_OK;
}

/**
 * @brief  底层写入自检：绕过文件系统，直接在卡上挑几个扇区做「写→回读→比对」
 *
 * @param  out      输出结果文本
 * @param  out_size 缓冲大小（建议 >= 640）
 * @return ESP_OK 自检执行完成（结果在 out 里）；其它表示没跑起来
 *
 * @note   为什么需要它：文件系统层面出现过「目录项长度对、数据区全是 0」的现象
 *         （写卡返回成功但数据没落盘），必须把问题定位到“卡本身 / 驱动程序 /
 *         用的是不是内部 RAM 缓冲”哪一层。本函数对每个扇区：
 *           1. 先读回原内容（测完会原样写回，不动用户数据）；
 *           2. 用 **内部 RAM** 缓冲写一个特征串，回读比对；
 *           3. 用 **PSRAM** 缓冲写另一个特征串，回读比对。
 *         另外还会：读 LBA0 判断“读”是否正常、做容量阶梯（找出从哪个位置开始写不进去）。
 */
esp_err_t sdcard_selftest(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    if (!s_mounted || s_card == NULL) {
        snprintf(out, out_size, "卡未挂载，无法自检");
        return ESP_ERR_INVALID_STATE;
    }

    int cap = s_card->csd.capacity;             /* 总扇区数 */
    if (cap < 64) {
        snprintf(out, out_size, "容量字段异常(capacity=%d 扇区)", cap);
        return ESP_FAIL;
    }

    /* 缓冲：内部 RAM 与 PSRAM 各一套（PSRAM 需要 64 字节对齐才能过驱动的 DMA 检查） */
    uint8_t *buf_orig = (uint8_t *)heap_caps_malloc(SDCARD_TEST_SEC_SIZE,
                                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *buf_int  = (uint8_t *)heap_caps_aligned_alloc(4, SDCARD_TEST_SEC_SIZE,
                                                          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *buf_psr  = (uint8_t *)heap_caps_aligned_alloc(64, SDCARD_TEST_SEC_SIZE,
                                                          MALLOC_CAP_SPIRAM);
    uint8_t *buf_read = (uint8_t *)heap_caps_malloc(SDCARD_TEST_SEC_SIZE,
                                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (buf_orig == NULL || buf_int == NULL || buf_read == NULL) {
        free(buf_orig); free(buf_int); free(buf_psr); free(buf_read);
        snprintf(out, out_size, "自检缓冲分配失败（内部 RAM 不足）");
        return ESP_ERR_NO_MEM;
    }

    /* ---------- 0. 先判断“读”到底行不行 ----------
     * LBA 0 是格式化时写下的引导扇区，**一定不是全 0**（有 OEM 名、"FAT32"、0x55AA 签名）。
     * 连它都读成全 0 的话，问题就在读路径，而不是卡写不进去。 */
    uint32_t data_start = 0;
    if (sdmmc_read_sectors(s_card, buf_read, 0, 1) == ESP_OK) {
        char oem[9]    = {0};
        char fstype[9] = {0};
        memcpy(oem, buf_read + 3, 8);
        memcpy(fstype, buf_read + 82, 8);
        data_start = sdcard_data_start_sector(buf_read);

        ESP_LOGW(TAG, "LBA0 读回：%s（OEM=%.8s FS=%.8s 签名=%02X%02X 数据区起始扇区=%" PRIu32 "）",
                 sdcard_buf_all_zero(buf_read, SDCARD_TEST_SEC_SIZE) ? "全 0 —— 读也有问题！" : "有内容",
                 oem, fstype, buf_read[510], buf_read[511], data_start);
    } else {
        ESP_LOGW(TAG, "LBA0 读取失败");
    }
    if (data_start == 0 || data_start >= (uint32_t)cap) {
        data_start = (uint32_t)cap / 20;        /* 兜底：避开最前面 5% */
    }

    /* 两套特征串：都带固定 magic，方便肉眼在日志里认出来 */
    memset(buf_int, 0, SDCARD_TEST_SEC_SIZE);
    memset(buf_psr, 0, SDCARD_TEST_SEC_SIZE);
    for (size_t i = 0; i + 16 < SDCARD_TEST_SEC_SIZE; i += 16) {
        memcpy(buf_int + i, "SDTEST-INTERNAL!", 16);
        if (buf_psr != NULL) {
            memcpy(buf_psr + i, "SDTEST-PSRAMBUF!", 16);
        }
    }

    /* ---------- 1. 容量阶梯：从数据区头部一路加到卡尾 ----------
     * 每处只写 1 个扇区且**测完把原内容写回**（写不进去的卡连还原也写不进去，
     * 所以这些空闲扇区不会被动坏；能写进去的会被原样还原）。
     * 目的：找出“从第几个扇区开始写不进去”——扩容卡的典型表现就是后半段全丢。 */
    static const uint32_t steps_mb[] = { 1, 4, 16, 32, 64, 128, 256, 512, 1024, 2048 };
    size_t used = 0;

    used += (size_t)snprintf(out + used, out_size - used,
                             "数据区起始 %" PRIu32 " | 阶梯：", data_start);
    for (size_t i = 0; i < sizeof(steps_mb) / sizeof(steps_mb[0]) && used + 24 < out_size; i++) {
        uint32_t lba_step = data_start + steps_mb[i] * 2048U;       /* 1MB = 2048 扇区 */
        if (lba_step >= (uint32_t)cap) {
            break;
        }
        char why[64] = {0};
        bool ok = sdcard_test_one_lba(lba_step, buf_int, buf_read, buf_orig, why, sizeof(why));
        int  n  = snprintf(out + used, out_size - used, "%uMB:%s ", (unsigned)steps_mb[i],
                           ok ? "OK" : "FAIL");
        if (n <= 0) {
            break;
        }
        used += (size_t)n;
    }

    /* ---------- 2. 关键位置再扫一遍，并把内部 RAM / PSRAM 对照打出来 ---------- */
    uint32_t lba[SDCARD_TEST_MAX_POS];
    int      pos_cnt = 0;
    lba[pos_cnt++] = (uint32_t)(cap / 10);
    lba[pos_cnt++] = (uint32_t)(cap / 2);
    lba[pos_cnt++] = (uint32_t)((int64_t)cap * 9 / 10);
    lba[pos_cnt++] = (uint32_t)(cap - 1);

    for (int i = 0; i < pos_cnt && used + 32 < out_size; i++) {
        char why_i[96] = {0};
        bool ok_int = sdcard_test_one_lba(lba[i], buf_int, buf_read, buf_orig, why_i, sizeof(why_i));

        char why_p[96] = {0};
        bool ok_psr   = (buf_psr != NULL) &&
                        sdcard_test_one_lba(lba[i], buf_psr, buf_read, buf_orig, why_p, sizeof(why_p));

        int n = snprintf(out + used, out_size - used,
                         "| LBA %" PRIu32 " 内RAM:%s%s PSRAM:%s%s ",
                         lba[i],
                         ok_int ? "OK" : "FAIL",
                         ok_int ? "" : why_i,
                         ok_psr ? "OK" : "FAIL",
                         ok_psr ? "" : why_p);
        if (n <= 0 || (size_t)n >= out_size - used) {
            break;                                  /* 缓冲写满，能报多少报多少 */
        }
        used += (size_t)n;
    }

    ESP_LOGW(TAG, "底层自检（每处都写入并还原原内容）：%s", out);

    free(buf_orig);
    free(buf_int);
    free(buf_psr);
    free(buf_read);
    return ESP_OK;
}
