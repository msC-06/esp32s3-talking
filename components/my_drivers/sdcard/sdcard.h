/**
 * @file    sdcard.h
 * @brief   TF 卡（microSD）驱动 + VFS FatFS 挂载
 *
 * 分层：本文件属于 my_drivers（TF 卡是板上外部器件），
 *       底层根据传输方式取用 bsp 的 SDMMC 或 SPI 配置，本模块只做“卡 + 文件系统”。
 *
 * 传输方式（SDCARD_TRANSPORT_SPI）：
 *   - 0（默认）= SDMMC / SDIO 1 线：CLK=47 CMD=48 D0=21，本开发板就是这种接法；
 *   - 1        = SPI 方式：需要板子按 CS/MOSI/MISO/CLK 四线接线，
 *                并把 bsp_sdspi.h 里的引脚宏改成实际接线。
 *   两种方式对上层完全一样（都是 VFS FatFS，挂载点 /sd）。
 *
 * 行为约定（重要）：
 *   - **绝不自动格式化**：挂载参数固定 format_if_mount_failed = false；
 *   - 没插卡 / 卡无法识别 -> sdcard_mount() 返回 ESP_ERR_NOT_FOUND，不注册文件系统；
 *   - 插了卡但不是 FAT32（例如 exFAT、未格式化）-> 返回 ESP_ERR_INVALID_STATE，
 *     只打印提示，不会动卡上的数据；
 *   - 挂载成功后可以直接用标准 C 文件接口：fopen("/sd/xxx.txt", "w")。
 */

#ifndef __SDCARD_H
#define __SDCARD_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 传输方式：0 = SDMMC(SDIO)，1 = SPI（默认 0，可在 CMake 里用 -D 覆盖） */
#ifndef SDCARD_TRANSPORT_SPI
#define SDCARD_TRANSPORT_SPI    0
#endif

/** 挂载点（用标准 C 库文件接口时带上这个前缀） */
#define SDCARD_MOUNT_POINT      "/sd"

/**
 * SD 时钟上限（kHz）。默认 20MHz（SDMMC_HOST_DEFAULT 的值）。
 *
 * 为什么要做成宏：实测遇到过“读卡完全正常、写入却被静默丢弃”的卡/板组合。
 * 读是卡驱动数据线、写是主机驱动数据线，写还要求卡在编程瞬间有足够电流，
 * 所以时钟太快 / 信号余量不足时，常见表现就是“写返回成功、数据没进卡”。
 * 把这里改小（例如 5000、1000 甚至 400）往往能救回来 ——
 * 开机时 sdcard_selftest() 会做一次“写入速度阶梯探测”，直接把能写进去的那一档报出来。
 */
#ifndef SDCARD_MAX_FREQ_KHZ
#define SDCARD_MAX_FREQ_KHZ     20000
#endif

/** 打开文件数的上限 */
#define SDCARD_MAX_FILES        5

/** 卡信息 */
typedef struct {
    char     name[16];          /*!< 卡名字（CID NAME），失败时为空串 */
    uint64_t total_bytes;       /*!< 总容量（字节） */
    uint64_t free_bytes;        /*!< 剩余容量（字节） */
} sdcard_info_t;

/**
 * @brief  挂载 TF 卡文件系统（先探测卡，再挂 FAT）
 *
 * @note   可重复调用；已经挂载时直接返回 ESP_OK。
 *         开机在 board_init() 里会自动尝试一次；如果当时没插卡，
 *         后面插上卡再调用本函数即可（IDF 在挂载失败时会反初始化 SDMMC 主机，
 *         不会残留半初始化状态，所以可以随时重试）。
 *         失败时不会格式化卡。
 * @return ESP_OK 挂载成功；
 *         ESP_ERR_NOT_FOUND   没检测到卡 / 卡初始化失败（可以认为是没插卡）；
 *         ESP_ERR_INVALID_STATE 卡在，但文件系统不是 FAT32（未格式化/exFAT），未做格式化
 */
esp_err_t sdcard_mount(void);

/**
 * @brief  卸载 TF 卡文件系统（等价于弹出，之后可以安全拔卡）
 */
esp_err_t sdcard_unmount(void);

/**
 * @brief  当前是否已经挂载
 */
bool sdcard_is_mounted(void);

/**
 * @brief  获取挂载点字符串（给上层拼路径用）
 */
const char *sdcard_get_mount_point(void);

/**
 * @brief  读取卡容量信息（需要已经挂载）
 * @param  info 输出参数
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 未挂载；ESP_ERR_INVALID_ARG 参数为空
 */
esp_err_t sdcard_get_info(sdcard_info_t *info);

/**
 * @brief  底层写入自检：绕过文件系统，直接在卡上挑几个扇区做「写→回读→比对」
 *
 * @param  out      输出一段人类可读的检查结果（失败原因也写在这里）
 * @param  out_size 缓冲大小（建议 >= 384）
 * @return ESP_OK 自检执行完成（结果在 out 里）；其它表示压根没跑起来（未挂载/内存不足）
 *
 * @note   为什么需要它：文件系统层面出现过「目录项长度对、数据区全是 0」的现象
 *         （写卡返回成功但数据没落盘），必须把问题定位到“卡本身 / 驱动程序 /
 *         用的是不是内部 RAM 缓冲”哪一层。本函数对每个扇区：
 *           1. 先读回原内容（测完会原样写回，不动用户数据）；
 *           2. 用 **内部 RAM** 缓冲写一个特征串，回读比对；
 *           3. 用 **PSRAM** 缓冲写另一个特征串，回读比对（PSRAM 缓冲在
 *              ESP32-S3 上可能因为 DMA 不做 cache 回写而丢数据，正好对照）。
 *         测试位置取容量的 10% / 50% / 90% / 最后一个扇区 —— 卡几乎是空的，
 *         这些位置都在空闲簇里，写完还会还原，不破坏文件系统。
 */
esp_err_t sdcard_selftest(char *out, size_t out_size);

/**
 * @brief  写入速度阶梯探测：把 SD 时钟从 20MHz 一路降到 400kHz，看哪一档能真正写进卡
 *
 * @param  out      输出结果文本（每档一行结果）
 * @param  out_size 缓冲大小（建议 >= 320）
 * @return ESP_OK 探测完成（结果在 out 里）；其它表示没跑起来
 *
 * @note   什么时候需要它：读卡正常、写入却被静默丢弃。
 *         写 flash 需要更大的瞬时电流、对信号完整性也更敏感，
 *         所以“降频能不能救”是判断“卡坏 vs 时序/供电余量不足”的关键实验。
 *         探测过程中会把时钟停在**第一个能写成功的档位**上（本次运行有效），
 *         想让每次开机都用这个频率，就把 sdcard.h 里的 SDCARD_MAX_FREQ_KHZ 改掉。
 *         探测只写数据区的空闲扇区并还原原内容，不动文件系统。
 */
esp_err_t sdcard_write_probe(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* __SDCARD_H */
