/**
 * @file    app_config.h
 * @brief   对话掌机全局配置：任务参数、录音参数、界面配色、缓冲尺寸
 *
 * ============================ 密钥不在这里 ============================
 *   WiFi / 百度 / DeepSeek / Tavily 的密钥全部放在**私有头文件**里，本文件只保留占位符：
 *
 *       # 在工程根目录执行（只需一次）
 *       cp components/app/app_config_template.h components/app/app_config_secret.h
 *       # 然后编辑 components/app/app_config_secret.h 填入真实密钥
 *
 *   - app_config_template.h —— 入库，只有占位符
 *   - app_config_secret.h   —— 不入库（已在 .gitignore），放真实密钥
 *   - 本文件用 __has_include 引入 secret：**文件不存在也能编译**，
 *     存在时它的宏优先（下面的占位符都写在 #ifndef 里，不会覆盖它）
 *   - 也支持编译期覆盖：idf.py -DAPP_WIFI_SSID=\"myap\" build
 * ======================================================================
 *
 * 分层：本文件属于 app 层，只有宏定义，不包含任何实现。
 */

#ifndef __APP_CONFIG_H
#define __APP_CONFIG_H

/* ============================================================
 *  0. 私有密钥（不写进仓库！）
 *     顺序很重要：先引入 secret，再用 #ifndef 兜底占位符
 *
 *     注意：这里是**普通 include**，不做 __has_include 判断。
 *     文件由 components/app/CMakeLists.txt 在配置阶段保证存在
 *     （不存在就用 app_config_template.h 复制一份）。
 *     原因：__has_include 方式有个很坑的副作用——
 *     如果先在没有密钥的情况下构建过一次，编译依赖文件（.d）里
 *     就不会记录 secret 头文件；之后把密钥文件补上，ninja 认为“没变化”，
 *     **不会重新编译**，固件里仍然是占位符（表现就是一直说“未配置密钥”）。
 * ============================================================ */

#include "app_config_secret.h"      /* 真实密钥：git 忽略，不提交 */

/* ---------- 下面 6 个只有 secret 没提供时才生效（占位符会被代码识别为“未配置”） ---------- */

#ifndef APP_WIFI_SSID
#define APP_WIFI_SSID           "YOUR_WIFI_SSID"          /* ← 见 app_config_secret.h */
#endif
#ifndef APP_WIFI_PASSWORD
#define APP_WIFI_PASSWORD       "YOUR_WIFI_PASSWORD"      /* ← 见 app_config_secret.h */
#endif
#ifndef BAIDU_ASR_API_KEY
#define BAIDU_ASR_API_KEY       "YOUR_BAIDU_API_KEY"      /* ← 见 app_config_secret.h */
#endif
#ifndef BAIDU_ASR_SECRET_KEY
#define BAIDU_ASR_SECRET_KEY    "YOUR_BAIDU_SECRET_KEY"   /* ← 见 app_config_secret.h */
#endif
#ifndef DEEPSEEK_API_KEY
#define DEEPSEEK_API_KEY        "YOUR_DEEPSEEK_API_KEY"   /* ← 见 app_config_secret.h */
#endif
#ifndef TAVILY_API_KEY
#define TAVILY_API_KEY          "YOUR_TAVILY_API_KEY"     /* ← 见 app_config_secret.h */
#endif

/* ============================================================
 *  1. WiFi（STA 模式）
 *     SSID / 密码见第 0 节（私有密钥，不写在这里）
 * ============================================================ */

/** 断线自动重连间隔（毫秒） */
#define APP_WIFI_RECONNECT_INTERVAL_MS  5000
/** 首次连接的重试次数 */
#define APP_WIFI_CONNECT_RETRY          3
/** WiFi 状态轮询周期（毫秒） */
#define APP_WIFI_POLL_MS                10000

/* ============================================================
 *  2. 百度智能云：短语音识别（ASR）
 *     https://console.bce.baidu.com/ai/#/ai/speech/app/list
 * ============================================================ */

/* Baidu API Key / Secret Key 见第 0 节（私有密钥，不写在这里） */

/** 取 token 接口 */
#define BAIDU_ASR_TOKEN_URL     "https://aip.baidubce.com/oauth/2.0/token"
/** 短语音识别接口 */
#define BAIDU_ASR_URL           "https://vop.baidu.com/server_api"
/** 用户唯一标识（随便填，用于区分设备） */
#define BAIDU_ASR_CUID          "esp32s3-talking"
/** 识别请求超时（毫秒） */
#define BAIDU_ASR_TIMEOUT_MS    20000
/** 取 token 超时（毫秒）
 *  @note 百度这个接口偶尔会很慢（实测有过握手成功但 10 秒不回数据），
 *        所以给到 12 秒，失败还会换新连接重试一次（见 app_asr.c） */
#define BAIDU_ASR_TOKEN_TIMEOUT_MS  12000
/** Base64 分块大小（必须是 3 的倍数，避免分块产生 '=' 填充） */
#define APP_ASR_B64_CHUNK       3072
/** token 提前多久续期（秒）：剩余时间小于它就重新取 */
#define APP_ASR_TOKEN_RENEW_SEC 3600
/** 读 token 响应用的静态缓冲：实测响应约 1.4KB 且是 chunked，给足余量防截断 */
#define APP_ASR_TOKEN_RESP_BUF_SIZE (6 * 1024)
/** 读识别响应用的静态缓冲：要装得下 JSON 外壳 + APP_TEXT_MAX_LEN 的识别文本 */
#define APP_ASR_RESP_BUF_SIZE       (8 * 1024)
/** 上传失败且 WiFi 正好掉线时，最多等多久联网恢复（毫秒）再重试。
 *  实测 AP 会以 reason=4（inactivity）把设备踢掉，自动重连一般 3~5 秒能回来，
 *  等链路恢复再重试，比“没链路也硬试”成功率高得多 */
#define APP_ASR_WAIT_LINK_MS        15000

/* ============================================================
 *  3. DeepSeek：Chat Completions（LLM）
 *     https://platform.deepseek.com/api_keys
 * ============================================================ */

/* DeepSeek API Key 见第 0 节（私有密钥，不写在这里） */

/** 对话接口 */
#define DEEPSEEK_API_URL        "https://api.deepseek.com/chat/completions"
/** 模型名：deepseek-chat（V3 对话模型） */
#define DEEPSEEK_MODEL          "deepseek-chat"
/** 请求超时（毫秒） */
#define APP_LLM_TIMEOUT_MS      30000
/** 系统提示词 */
#define APP_LLM_SYSTEM_PROMPT   "你是一个友好的对话助手，请用简洁中文回答。对话请尽可能简短。"
/** 限制回复长度（token），LCD 也显示不下太长 */
#define APP_LLM_MAX_TOKENS      512
/** 读 HTTP 响应用的缓冲（PSRAM 分配）：回复正文 + tool_calls + usage 都给足 */
#define APP_LLM_RESP_BUF_SIZE   (32 * 1024)

/**
 * 请求体（JSON 字符串）缓冲大小，**放 PSRAM 静态区**（见 app_llm.c 的 s_body_buf）。
 *
 * 为什么不能放内部堆：请求体从「组好」到「HTTP 客户端清理」一直有效，
 * 而这整段时间正是 TLS 握手/收发吃内部堆的时候（in 16KB + out 4KB + 客户端收发缓冲）。
 * 两者叠加才是「搜索后追问」那一步内部堆见底的主因。
 *
 * 容量账（最坏情况，第 2 轮带工具结果的请求）：
 *   system 512 + 历史 4096 + user 2048 + assistant/tool_calls ~300
 *   + 工具结果 5120 + 外壳/model/stream ~100   ≈ 12.2KB
 *   JSON 转义会膨胀（\n → 2 字节），按 1.3 倍算 ≈ 16KB，留到 20KB。
 * 万一真超了，代码会退回 cJSON_PrintUnformatted（内部堆），只影响那一次，不会出错。
 */
#define APP_LLM_BODY_BUF_SIZE   (20 * 1024)

/** web_search 工具的描述：决定模型什么时候去联网搜索 */
#define APP_LLM_TOOL_DESC       \
    "联网搜索工具。遇到实时信息（新闻、天气、价格、比分、人物近况）或你不确定的知识时调用它，" \
    "query 用简短具体的关键词。"
/** 一次对话最多执行几个工具调用（防止模型一次丢一堆把内存吃满） */
#define APP_LLM_MAX_TOOL_CALLS  2

/** 历史对话正文合计上限（字节）：超出时只带最近几轮。
 *  请求体是 cJSON 建在内部 RAM 里的，10 轮历史能到 20KB+，
 *  再叠上 TLS 握手那 ~20KB 会把内部堆挤干（实测内部历史最低只剩 8KB，
 *  之后连 TCP 连接都建不起来 → “Failed to open new connection”）。 */
#define APP_LLM_HISTORY_BUDGET  4096

/* ============================================================
 *  3.5 Tavily 联网搜索（DeepSeek Function Calling 的 web_search 工具）
 *      https://app.tavily.com/home  ->  API Keys
 * ============================================================ */

/* Tavily API Key 见第 0 节（私有密钥，不写在这里） */

/** 搜索接口 */
#define TAVILY_SEARCH_URL       "https://api.tavily.com/search"
/** 搜索请求超时（毫秒） */
#define APP_WEB_SEARCH_TIMEOUT_MS       15000
/** 最多取几条结果（写进请求体，解析时也按这个数截断） */
#define APP_WEB_SEARCH_MAX_RESULTS      3
/** 拼给模型的摘要文本上限（字符），减少 token 消耗；受调用者缓冲大小再收一次。
 *  注意：调用者的缓冲按 "APP_WEB_SEARCH_TEXT_MAX + 1" 自动分配，改这里缓冲会跟着变。 */
#define APP_WEB_SEARCH_TEXT_MAX         (5 * 1024)
/** 读 Tavily 响应用的 PSRAM 缓冲（3 条结果的原始 JSON 可能几十 KB，给足） */
#define APP_WEB_SEARCH_RESP_BUF_SIZE    (64 * 1024)

/* ============================================================
 *  3.6 流式输出（SSE）与打字机显示
 * ============================================================ */

/** 1 = DeepSeek 用 SSE 流式返回（stream=true），边收边追加到气泡；
 *  0 = 退回一次性返回（界面对整段文字做伪打字机动画） */
#define APP_LLM_STREAM_ENABLE       1
/** HTTP 响应里单行 SSE（data: {...}）的最大长度；超长整行丢弃并告警 */
#define APP_HTTP_SSE_LINE_MAX       1024
/** 流式 tool_calls 的 arguments 分片拼起来后的最大长度 */
#define APP_LLM_TOOL_ARGS_MAX       768

/** 1 = 打开打字机效果（逐字追加，而不是一次性 set_text 整段） */
#define APP_CHAT_TYPE_ENABLE        1
/** 两次追加之间的间隔（毫秒） */
#define APP_CHAT_TYPE_INTERVAL_MS   100
/** 每个间隔追加几个“字”：1 个汉字算 1 个字，ASCII 算 1/3 个字
 *  （即 =1 时就是“每个间隔 1 个汉字 或 3 个英文字符”） */
#define APP_CHAT_TYPE_UNITS         1
/** 待显示文本超过这么多字节后开始加速追赶（0 = 关闭追赶，严格按上面速度打）
 *  @note 流式返回比打字速度快，完全不追赶的话长回复要打很久 */
#define APP_CHAT_TYPE_CATCHUP_BYTES 120
/** 追赶时每次最多追加几个“字”（限速，别一下全糊上去） */
#define APP_CHAT_TYPE_MAX_UNITS     8
/** 进入“AI 正在输出”状态后的兜底超时（毫秒）：
 *  万一界面没回报“打完”，也不至于一直锁着按键不让录音 */
#define APP_CHAT_TYPING_TIMEOUT_MS  90000

/* ============================================================
 *  4. 对话历史
 * ============================================================ */

/** 保留最近多少轮对话（1 轮 = user + assistant） */
#define APP_CHAT_HISTORY_MAX_ROUNDS     10
/** 历史上限条数（20 条，不含 system） */
#define APP_CHAT_HISTORY_MAX_MSGS       (APP_CHAT_HISTORY_MAX_ROUNDS * 2)
/** 单条历史消息最大字节数 */
#define APP_CHAT_HISTORY_MSG_MAX        1024

/* ============================================================
 *  5. 按键（GPIO0）
 * ============================================================ */

/** 扫描周期（毫秒），app_key 内部用 esp_timer 周期回调 */
#define APP_KEY_SCAN_MS             20
/** 消抖需要连续采到几次相同电平（2 * 20ms = 40ms） */
#define APP_KEY_DEBOUNCE_SAMPLES    2
/** 最长录音时间（毫秒），到点自动停止，防止忘松按键 */
#define APP_KEY_MAX_RECORD_MS       15000
/** 最短录音时间（毫秒），太短的录音直接丢弃 */
#define APP_KEY_MIN_RECORD_MS       400

/* ============================================================
 *  6. 双核任务划分与优先级
 *     Core 0 = PRO_CPU（网络核），Core 1 = APP_CPU（应用/显示核）
 * ============================================================ */

#define APP_CPU_NET_CORE            0
#define APP_CPU_APP_CORE            1

/** 网络任务（ASR 上传 + LLM 请求）：TLS + JSON 解析需要大栈 */
#define APP_NET_TASK_PRIO           5
#define APP_NET_TASK_STACK          8192
/** WiFi 任务 */
#define APP_WIFI_TASK_PRIO          5
#define APP_WIFI_TASK_STACK         (5 * 1024)
/** 按键/业务状态机任务
 *  @note 注意：消息结构体（app_text_msg_t 有 4KB+）绝不能放成局部变量，
 *        否则这个栈立刻爆掉；app_bus_post_* 已经改成用静态暂存区发送。 */
#define APP_KEY_TASK_PRIO           4
#define APP_KEY_TASK_STACK          (5 * 1024)
/** 音频采集任务：实时性要求高，优先级最高 */
#define APP_AUDIO_TASK_PRIO         6
#define APP_AUDIO_TASK_STACK        (5 * 1024)

/* ============================================================
 *  7. 队列长度与文本长度
 *
 *  ⚠️ 内存账（队列是**内部堆**，不是 PSRAM！）：
 *       asr 队列 2 × (APP_TEXT_MAX_LEN+8)   = 4.1KB
 *       llm 队列 2 × (APP_TEXT_MAX_LEN+8)   = 4.1KB
 *       chat 队列 6 × (APP_CHAT_TEXT_MAX+8) = 6.2KB
 *     内部堆总共才 ~139KB，还要养任务栈（~20KB）、WiFi/LWIP（~40KB）、
 *     TLS 连接（~22KB）。队列占太多会把内部堆挤到 20KB 以下，
 *     后果不是“分配失败报个错”，而是 WiFi 驱动拿不到收包缓冲、TLS 写阻塞
 *     （实测现象：上传卡 20 秒后 “Poll timeout”，errno 还是上一次 connect 的残留值）。
 *
 *     所以想放大文本请优先动 **PSRAM 上的**那几个：
 *     APP_LLM_RESP_BUF_SIZE / APP_WEB_SEARCH_* / EXT_RAM_BSS 标记的缓冲。
 *     运行时空闲任务会打印内部堆余量与最大可分配块，照着数据调。
 * ============================================================ */

#define APP_KEY_QUEUE_LEN           8       /* 按键事件 */
#define APP_AUDIO_QUEUE_LEN         4       /* 录音启停命令 */
#define APP_PCM_QUEUE_LEN           2       /* 录音数据 -> 网络任务 */
#define APP_ASR_QUEUE_LEN           2       /* ASR 结果 */
#define APP_LLM_QUEUE_LEN           2       /* LLM 结果 */
#define APP_CHAT_QUEUE_LEN          6       /* 聊天消息 -> 界面 */
#define APP_UI_QUEUE_LEN            4       /* 界面事件 -> 业务状态机（打字完成等） */

/** ASR 识别文本 / LLM 回复文本消息的缓冲长度（也是一轮回复的硬上限）
 *  @note 2048 字节能装约 1000 个汉字，对“简洁回答”的系统提示足够了 */
#define APP_TEXT_MAX_LEN            2048
/** 聊天消息（投递给界面的文本）长度
 *  @note 只要 >= APP_CHAT_BUBBLE_TEXT_MAX 就不会丢显示内容 */
#define APP_CHAT_TEXT_MAX           1024
/** 单个气泡最多显示多少字节（再长只影响显示，不影响交给模型的历史）
 *  @note 这段文字会占 LVGL 自己的内存池（CONFIG_LV_MEM_SIZE=64KB，实测占用 ~14%） */
#define APP_CHAT_BUBBLE_TEXT_MAX    1024
/** 聊天区最多保留多少个气泡（超出后删除最老的）
 *  @note 每个气泡约占 LVGL 池 2KB + 文字长度 */
#define APP_CHAT_MAX_BUBBLES        10
/** 气泡内文字最大像素宽度（320 宽的屏，留出内边距） */
#define APP_CHAT_BUBBLE_MAX_W       210

/* ============================================================
 *  8. TLS 证书校验
 *     APP_TLS_USE_CA_BUNDLE=1：用 IDF 内置根证书包校验（推荐，联网正常）
 *     APP_TLS_USE_CA_BUNDLE=0：开发阶段跳过校验，
 *                              需要 sdkconfig 打开
 *                              CONFIG_ESP_TLS_INSECURE=y 与
 *                              CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y
 * ============================================================ */

#define APP_TLS_USE_CA_BUNDLE       1

/* ============================================================
 *  8.5 空闲内存监控
 *      空闲任务只负责“到点触发”，真正读堆统计 + 打印放在监控任务里：
 *      空闲任务栈只有 CONFIG_FREERTOS_IDLE_TASK_STACKSIZE（本工程 1536 字节），
 *      在里面打印日志会把它的栈冲爆，而且空闲任务还要回收被删任务的资源，不能阻塞。
 * ============================================================ */

/** 1 = 打开空闲内存定期上报 */
#define APP_MEM_LOG_ENABLE          1
/** 上报间隔（毫秒）：空闲任务里按时间限流，不会刷屏也不会占 CPU */
#define APP_MEM_LOG_INTERVAL_MS     30000
/** 内部 RAM 空闲低于这个值时额外告警（字节） */
#define APP_MEM_WARN_INTERNAL       (24 * 1024)
/** PSRAM 空闲低于这个值时额外告警（字节） */
#define APP_MEM_WARN_PSRAM          (256 * 1024)
/** 任务栈剩余低于这个值时额外告警（字节，历史最小值） */
#define APP_MEM_WARN_STACK          1024

/** 监控任务优先级（比空闲任务高就行，别抢业务任务的 CPU） */
#define APP_MEM_TASK_PRIO           1
/** 监控任务栈：要装得下一次带不少参数的 ESP_LOGI（vsnprintf + UART） */
#define APP_MEM_TASK_STACK          4096

/* ============================================================
 *  9. 界面
 * ============================================================ */

/** 界面刷新/队列轮询周期（毫秒），在 LVGL 定时器里跑
 *  @note 决定“识别结果 / AI 回复”上屏的最大延迟，30ms 足够跟手又不费 CPU */
#define APP_CHAT_UI_TICK_MS         30
/** 顶部状态栏高度 */
#define APP_CHAT_TOPBAR_H           26
/** 左上角 WiFi 文字的最大宽度（超了用省略号，别挤到右边的状态字） */
#define APP_CHAT_WIFI_TEXT_MAX_W    140
/** 底部状态行高度 */
#define APP_CHAT_STATUSBAR_H        22
/** 界面配色（RGB888，内部转 RGB565） */
#define APP_COLOR_BG                0x0B1622    /* 背景 */
#define APP_COLOR_TOPBAR            0x14324F    /* 顶栏 */
#define APP_COLOR_TEXT              0xE6E6E6    /* 普通文字 */
#define APP_COLOR_USER_BUBBLE       0x2E6DA4    /* 用户气泡（浅蓝） */
#define APP_COLOR_AI_BUBBLE         0x3A3A3A    /* AI 气泡（浅灰） */
#define APP_COLOR_ERROR             0xFF5252    /* 错误文字 */
#define APP_COLOR_OK                0x66BB6A    /* 正常/已连接 */
#define APP_COLOR_WARN              0xFFD54F    /* 提示文字 / 太阳图标 */
#define APP_COLOR_PANEL             0x1E2A38    /* 弹窗面板底色 */

/* ============================================================
 *  10. 系统时间（SNTP 对时，app_time.c）
 * ============================================================ */

/**
 * 总开关。
 * ESP32-S3 没有带电池的 RTC，开机时间是 1970-01-01，
 * 不问时间的话大模型会自己编一个日期（问“今天几号”它答 2025-06-13 这种）。
 * 打开后：界面待机提示会显示 HH:MM，system 提示词里也会带上真实日期时间。
 */
#define APP_TIME_ENABLE             1

/**
 * 时区（POSIX TZ 字符串）。
 *   CST-8        = 中国标准时间 UTC+8（注意符号是反的：CST-8 表示东八区）
 *   UTC0         = 零时区
 *   想看更多写法（带夏令时）搜 “POSIX TZ string”。
 */
#define APP_TIME_TZ                 "CST-8"

/** NTP 服务器（按顺序尝试；国内优先，避免解析到国外慢节点）
 *  @note 能用几个取决于 sdkconfig 的 CONFIG_LWIP_SNTP_MAX_SERVERS（本工程设为 3） */
#define APP_TIME_SNTP_SERVER1       "ntp.aliyun.com"
#define APP_TIME_SNTP_SERVER2       "cn.pool.ntp.org"
#define APP_TIME_SNTP_SERVER3       "pool.ntp.org"

/** 单次对时最长等待（毫秒） */
#define APP_TIME_SYNC_TIMEOUT_MS    15000
/** 对时失败后多久重试（毫秒） */
#define APP_TIME_RETRY_MS           (30 * 1000)
/** 对时成功后多久复查一次（毫秒，默认 6 小时；RTC 有漂移，定期校准） */
#define APP_TIME_RESYNC_INTERVAL_MS (6 * 60 * 60 * 1000)

/** 对时任务的优先级 / 栈 / 核（低优先级，纯等待，放网络核） */
#define APP_TIME_TASK_PRIO          3
#define APP_TIME_TASK_STACK         3072
#define APP_TIME_TASK_CORE          0

/**
 * 是否把当前时间写进大模型的 system 提示词。
 * 关掉的话模型就不知道真实日期了（问时间会瞎猜）。
 */
#define APP_TIME_INJECT_TO_LLM      1

/** system 提示词缓冲大小（原文 + 时间那句话 + 结束符） */
#define APP_LLM_SYSTEM_PROMPT_MAX   512

/* ============================================================
 *  11. 背光与设置持久化（app_settings.c + 界面上的太阳图标弹窗）
 * ============================================================ */

/** 默认背光亮度（%）：读不到设置文件、或 TF 卡没插时用它 */
#define APP_LCD_BRIGHTNESS_DEFAULT  50

/**
 * 滑条能调到的**实际**最小亮度（%）。
 *
 * 为什么默认不是 0：滑条可以拖到 0，但 0% 就是全黑。如果把这个值存进 TF 卡，
 * 下次开机屏幕就是黑的，用户既看不到太阳图标也点不到【确认】——机器等于锁死。
 * 所以默认下限 5%（很暗但看得清）。真要“一键关屏”就把它改成 0，
 * 但黑屏后只能靠串口或拔卡删 /sd/chat_settings.txt 恢复。
 */
#define APP_LCD_BRIGHTNESS_MIN      5

/** 设置文件路径（TF 卡 FATFS，挂载点 /sd 见 my_drivers/sdcard） */
#define APP_SETTINGS_PATH           "/sd/chat_settings.txt"

/** 文件头标识：读文件时先认这行，避免误读同名的别的文件 */
#define APP_SETTINGS_MAGIC          "ESP32TALK_CFG"

/**
 * TF 卡不可用时，把设置写进**内部 NVS**（flash 上的 nvs 分区，24KB）。
 *
 * 为什么要它：不是每张卡都好用 —— 实测遇到过“数据区已经损坏”的卡：
 *   目录项/长度都写得进去，但数据区写入被静默丢弃（裸扇区测试全 FAIL，
 *   连电脑写进去的文件读回来也是全 0）。这种卡上根本存不住东西。
 * 打开后：**两边都写**（TF 卡 + NVS 镜像），读的时候取“序号更大的那一份”，
 *   所以卡坏了照样能记住设置；换回好卡也能自动用上。
 */
#define APP_SETTINGS_NVS_ENABLE     1
/** NVS 命名空间与键名 */
#define APP_SETTINGS_NVS_NAMESPACE  "chatset"
#define APP_SETTINGS_NVS_KEY_VALUE  "backlight"
#define APP_SETTINGS_NVS_KEY_SEQ    "seq"

/* ============================================================
 *  12. WiFi 状态显示（左上角精简版：只显示“连没连”和“信号档位”）
 * ============================================================ */

/**
 * 信号强度分档（dBm，数值越大越好）。
 *   >= APP_WIFI_RSSI_STRONG -> 「WiFi 强」
 *   >= APP_WIFI_RSSI_MEDIUM -> 「WiFi 中」
 *   更低                     -> 「WiFi 弱」
 * 经验值：-60 dBm 以上很稳，-75 以下开始丢包。
 */
#define APP_WIFI_RSSI_STRONG        (-60)
#define APP_WIFI_RSSI_MEDIUM        (-75)

/** WiFi 状态刷新周期（毫秒）：2~3 秒刷一次足够，别高频刷屏占 CPU */
#define APP_WIFI_STATUS_REFRESH_MS  2500

/**
 * app 层手动补连的最小间隔（毫秒）。
 * bsp_wifi 自己的重连任务会每 3 秒试一次、试 3 次就放弃；
 * 超过这个时间还没连上，app 层才会去“扫一遍附近的已知网络并尝试连接”，
 * 避免两边同时 esp_wifi_connect() 互相打架（见 app_wifi.c 注释）。
 *
 * @note  现在这条路径不只是重连“编译期那个 SSID”，而是把
 *        app_config_secret.h 里的 SSID + NVS 里记过的网络都当成候选
 *        （换个地方、换个热点照样能自己连上）。
 */
#define APP_WIFI_MANUAL_RECONNECT_MS APP_WIFI_KNOWN_SCAN_INTERVAL_MS

/* ============================================================
 *  13. WiFi 扫描 / 已保存网络（app_wifi_store + app_wifi_ui）
 *
 *  需求：
 *    1. 顶栏的 WiFi 文字变成一个按钮，点开能看到附近的 WiFi；
 *       点某个 WiFi 名弹出小键盘输入密码，连接成功后**账号密码存进 NVS**；
 *    2. 没连上的时候定期扫描附近、发现“记过的”网络就自动连接；
 *    3. 最多记 10 个，存满时**覆盖最久没用过的那个**。
 * ============================================================ */

/** 最多记住多少个连过的 WiFi（存满后覆盖“最久没用过”的那个） */
#define APP_WIFI_STORE_MAX              10

/** NVS 命名空间（NVS 名字最长 15 个字符） */
#define APP_WIFI_STORE_NVS_NAMESPACE    "wifistore"

/** 扫描结果最多缓存多少条（界面列表用；去重后按信号从强到弱排序） */
#define APP_WIFI_SCAN_LIST_MAX          20

/** WiFi 请求队列长度（界面 -> WiFi 任务：扫描 / 连接） */
#define APP_WIFI_REQ_QUEUE_LEN          3

/**
 * 未连接时，每隔多久扫一次附近的 AP 并尝试连接“记录过的”网络（毫秒）。
 * 底层的自动重连只认最后一次连接的那个 AP，换个地方（或换热点）就没用了，
 * 所以要靠这条“扫一遍已知网络”的兜底路径。
 */
#define APP_WIFI_KNOWN_SCAN_INTERVAL_MS 30000

/** 一轮自动连接最多试几个候选 AP（避免一个一个试太久、把用户自己的操作挤掉） */
#define APP_WIFI_TRY_MAX                3

/* ---------- WiFi 界面的尺寸/节奏（app_wifi_ui.c） ---------- */

/** 列表里每一行的高度（像素） */
#define APP_WIFI_LIST_ROW_H             30
/** 弹窗轮询周期（毫秒）：看扫描/连接有没有结果 */
#define APP_WIFI_UI_TICK_MS             150
/** 连接成功后隔多久自动关掉弹窗（毫秒） */
#define APP_WIFI_UI_CLOSE_DELAY_MS      1500

/* ============================================================
 *  14. 低功耗 / 自动省电（app_power.c：IMU + 触摸静止检测）
 *
 *  行为（“软省电”方案：不进 light sleep、不碰面板/SPI/LVGL）：
 *    1. 一直读取板载 IMU（QMI8658）加速度/角速度，并轮询触摸屏；
 *    2. “长时间静止且没有触摸”超过用户在【设置】里选的时长（30 秒 / 1 分钟 / 永不）
 *       -> 进入软省电：背光降到最小、WiFi 切最大省电档、屏蔽录音键；
 *    3. 省电期间一旦 触摸 / IMU 超过阈值 / 按键按下 -> 立刻恢复
 *       （背光回用户亮度、录音键解禁、WiFi 回默认功耗档）。
 *
 *  为什么不用 light sleep：ESP32-S3 没有 SPI retention，这块板的 ST7789 在
 *  light sleep 之后不再响应任何命令（SLPIN/SLPOUT/DISPON/整屏填色都没反应），
 *  “关屏 + 拆 SPI3 总线重建”那条路实机验证会把面板弄死，已放弃。
 * ============================================================ */

/** 是否启用自动省电（0 = 完全不启用，行为与旧固件一致；app_power_task 会直接退出） */
#define APP_POWER_ENABLE                 1

/** 任务优先级 / 栈 / 核（低优先级轮询，放应用核，别抢音频和界面）
 *  栈 8192：这条路径要走 I2C 触摸读取 + IMU 读取 + LVGL 锁；
 *  4096 时实机报过 "A stack overflow in task app_power"（当时的唤醒恢复路径里
 *  还有 LVGL 整屏渲染），故加倍保留余量。 */
#define APP_POWER_TASK_PRIO              2
#define APP_POWER_TASK_STACK             8192
#define APP_POWER_TASK_CORE              APP_CPU_APP_CORE

/** 正常模式下多久采一次 IMU + 触摸（毫秒） */
#define APP_POWER_SAMPLE_MS              50

/** 软省电模式下多久醒一次检查唤醒源（毫秒）。
 *  软省电只降背光、CPU 照常跑，所以轮询代价可以忽略；20 ms 保证
 *  “轻点一下屏幕/快按一下键”也能在下一拍被采到（再配 app_power 里的
 *  s_wake_req 闩锁，落在两次采样之间的操作也不会丢）。 */
#define APP_POWER_SLEEP_POLL_MS          20

/* ---------- IMU 唤醒灵敏度（设置页“弱 / 中 / 强”三档） ----------
 * 判据和下面“静止判定”是同一套：相邻两次采样的加速度 L1 差（单位 g）
 * 或三轴角速度绝对值之和（单位 °/s）超过阈值就算“动了一下”，触发唤醒。
 * 三档只是换阈值，触摸唤醒不受档位影响（任何触摸都直接唤醒）。 */

/** 档位编号（同时是 NVS 里存的值：0 = 弱，1 = 中，2 = 强） */
#define APP_POWER_IMU_SENS_WEAK          0u
#define APP_POWER_IMU_SENS_MED           1u
#define APP_POWER_IMU_SENS_STRONG        2u
#define APP_POWER_IMU_SENS_COUNT         3u

/** 默认档：中（和老固件的行为一致） */
#define APP_POWER_IMU_SENS_DEFAULT       APP_POWER_IMU_SENS_MED

/** 加速度唤醒阈值（单位 g，芯片 ±4g 档；越大越“钝”） */
#define APP_POWER_IMU_ACC_G_WEAK         0.10f
#define APP_POWER_IMU_ACC_G_MED          0.30f
#define APP_POWER_IMU_ACC_G_STRONG       0.60f

/** 角速度唤醒阈值（单位 °/s，芯片 ±512dps 档；三轴绝对值之和） */
#define APP_POWER_IMU_GYRO_DPS_WEAK      8.0f
#define APP_POWER_IMU_GYRO_DPS_MED       16.0f
#define APP_POWER_IMU_GYRO_DPS_STRONG    32.0f

/** 兼容旧名字：这两个宏就等于“中档”阈值（不要另外定义一套数值） */
#define APP_POWER_MOTION_ACC_G           APP_POWER_IMU_ACC_G_MED
#define APP_POWER_MOTION_GYRO_DPS        APP_POWER_IMU_GYRO_DPS_MED

/** 休眠超时选项（0 = 永不自动休眠） */
#define APP_POWER_TIMEOUT_NEVER          0u
#define APP_POWER_TIMEOUT_30S            30000u
#define APP_POWER_TIMEOUT_60S            60000u
#define APP_POWER_TIMEOUT_DEFAULT_MS     APP_POWER_TIMEOUT_30S

/** 休眠超时在 NVS 里的键名（和背光同属 chatset 命名空间） */
#define APP_SETTINGS_NVS_KEY_SLEEP_MS    "sleep_ms"

/** IMU 唤醒灵敏度档位在 NVS 里的键名（0/1/2，见 APP_POWER_IMU_SENS_*） */
#define APP_SETTINGS_NVS_KEY_IMU_SENS    "imu_sens"

#endif /* __APP_CONFIG_H */
