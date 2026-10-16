# ESP32-S3 对话掌机（按键录音 → 百度 ASR → DeepSeek LLM → LCD 显示）

立创·实战派 ESP32-S3（LCKFB-SZPI-ESP32-S3-VA，ESP32-S3-WROOM-1-N16R8 / 16MB Flash + 8MB Octal PSRAM）
+ 2.0 寸 IPS 320×240（ST7789，SPI，LVGL9）
+ ES8311（DAC）+ ES7210（ADC，板载双麦）
+ GPIO0 录音按键。

```
┌──────────── ESP32-S3 ────────────┐
│ Core 1（应用/显示核）             │   Core 0（网络核）
│  lvgl_port_task（已有）           │    app_wifi_task  (prio 5)
│  app_key_task   (prio 4)          │    app_net_task   (prio 5, 栈 8KB)
│  app_audio_task (prio 6)          │        │
└───────────┬──────────────────────┘        │
            │ key_event_queue               │ pcm_queue
            │ audio_ctrl_queue              │
  GPIO0 ──> app_key ──> app_audio ──(PCM,PSRAM)──> app_net
                                     app_asr(百度) ─> asr_result_queue
                                     app_llm(DeepSeek) ─> llm_result_queue
            chat_msg_queue ──> app_chat_ui(LVGL 定时器) ──> LCD 中文气泡
```

## 一、填入密钥（不进仓库）

密钥统一放在**私有头文件**里，代码仓库只保留占位符模板：

| 文件 | 是否提交 | 内容 |
| --- | --- | --- |
| `components/app/app_config_template.h` | ✅ 提交 | 只有占位符，作为模板 |
| `components/app/app_config_secret.h` | ❌ 不提交（已在 `.gitignore`） | 真实密钥 |
| `components/app/app_config.h` | ✅ 提交 | 普通设置；`#include "app_config_secret.h"` 拿到密钥 |

**clone 后只需一步**：

```bash
cp components/app/app_config_template.h components/app/app_config_secret.h
# 然后编辑 app_config_secret.h，填入下面 6 个值：
```

| 宏 | 说明 |
| --- | --- |
| `APP_WIFI_SSID` / `APP_WIFI_PASSWORD` | 你的 WiFi 名称 / 密码（2.4GHz） |
| `BAIDU_ASR_API_KEY` / `BAIDU_ASR_SECRET_KEY` | 百度智能云「短语音识别标准版」应用的 API Key / Secret Key |
| `DEEPSEEK_API_KEY` | DeepSeek 开放平台的 API Key |
| `TAVILY_API_KEY` | Tavily 的 API Key（联网搜索工具用） |

其实**忘了复制也不会报错**：`components/app/CMakeLists.txt` 在配置阶段发现
`app_config_secret.h` 不存在时，会自动从模板复制一份（内容是占位符），
运行时日志会明确提示"还没有配置 xxx（填 components/app/app_config_secret.h）"。

也可以完全不用这个文件，直接在编译期覆盖（占位符都在 `#ifndef` 里，
命令行 `-D` 优先级更高）：

```bash
idf.py -DAPP_WIFI_SSID=\"myap\" -DAPP_WIFI_PASSWORD=\"12345678\" build
```

> ⚠️ **踩过的坑（务必知道）**：最初用 `#if __has_include("app_config_secret.h")`
> 让这个 include 变成可选的，结果出现过一个很隐蔽的问题——
> 如果在**还没有密钥文件**的情况下构建过一次，编译依赖文件（`.d`）里不会记录
> `app_config_secret.h`；之后你把密钥文件补上，ninja 认为"没有文件变化"，
> **不会重新编译**，固件里装的还是占位符，表现为一直提示"未配置密钥"，
> 而源码看起来完全正确。
> 现在改成 **无条件 `#include`** + CMake 保证文件存在，依赖关系正常，
> 改密钥一定会触发重新编译（实测：touch 该文件后 app_net/app_wifi/app_asr/
> app_llm/app_web_search 都会重编）。

**自检**：填完密钥后可以看看固件里到底装了什么（占位符构建会比正常小 ~30KB）：

```bash
python -c "d=open('build/empty_proj.bin','rb').read(); print(d.count(b'tvly-dev'), d.count(b'YOUR_TAVILY_API_KEY'))"
# 期望输出：1 0   （1 个真实 key，0 个占位符）
```

## 二、编译与烧写

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

字库：工程根目录的 `hzk16.bin` 会被自动裁剪并烧到 `fontbin` 分区（见根 `CMakeLists.txt`），
没有它中文会显示成占位框。

## 三、操作流程

1. 上电 → 开机动画 → 聊天界面（顶部 WiFi 状态 / 底部状态行）。
   顶栏左上角的 **WiFi 文字是个按钮**：点它 → 附近 WiFi 列表 → 点某个 WiFi 名 →
   小键盘输密码 → 连接成功自动存进 NVS（详见 9.3 节）。
2. **按住 GPIO0**：状态行显示「录音中... 松开发送」，音频任务开始 16kHz/16bit 单声道采集，
   PCM 写进 PSRAM 缓冲（最大 15 秒 = 480000 字节）。按住超过 15 秒会自动停止录音。
3. **松开**：状态行「识别中...」，PCM 交给网络任务上传百度 ASR。
4. 识别文本作为用户气泡显示在右侧，状态行「AI 正在回复...」，于是请求 DeepSeek。
5. AI 回复**逐字冒出来**（SSE 流式 + 打字机效果，见第七节），显示完后状态回到「按住按键说话」。
   打字期间按键不生效（状态是 `AI_TYPING`），避免用户对着半句话追问；
   万一「显示完」的回报丢了，90 秒兜底强制回待机（`APP_CHAT_TYPING_TIMEOUT_MS`）。
6. 任何一步出错都会在状态行红色显示原因（网络超时 / 密钥未配置 / 音频太短 …），并回到待机。

气泡最多保留 10 条（`APP_CHAT_MAX_BUBBLES`），超出自动删除最老的（LVGL 堆只有 64KB，不能无限堆）。

## 四、目录与分层

```
main/main.c                      组装层：board_init() -> app_splash_run() -> app_start() -> 删除 main 任务
components/bsp/                  ESP32-S3 片上外设（I2C/SPI/GPIO/WiFi）
  bsp_gpio.c/h                   ★新增：录音按键 GPIO0（输入+上拉+读电平）
components/my_drivers/           板上外部器件驱动
  audio/audio.c/h                ES8311/ES7210（已有）
  audio/audio_recorder.c/h       ★新增：PSRAM 环形缓冲录音器（16k/16bit/单声道）
  font_hzk16/hzk16.c/h           HZK16 字库（新增 hzk16_utf8_to_gb2312() 公开接口）
  net/net.c/h, lcd.c, board/…    已有驱动，未修改
components/app/                  应用层
  app.c/h                        ★app_start()：初始化 + 创建四个双核任务
  app_config.h                   ★全部配置宏（WiFi/密钥/任务/录音/界面）
  app_bus.c/h                    ★业务状态机 + 5 个跨任务队列
  app_key.c/h                    ★GPIO0 消抖状态机(esp_timer 20ms) + 业务状态机任务
  app_audio.c/h                  ★音频采集任务
  app_wifi.c/h                   ★WiFi(STA)：状态刷新 + 扫描/连接请求 + 已知网络自动连接
  app_wifi_store.c/h             ★新增：连过的 WiFi（SSID+密码）存内部 NVS，最多 10 个
  app_wifi_ui.c/h                ★新增：附近 WiFi 列表弹窗 + 小键盘输密码弹窗
  app_net.c/h                    ★网络任务：ASR -> LLM
  app_asr.c/h                    ★百度短语音识别
  app_llm.c/h                    ★DeepSeek 对话（流式 SSE + 工具调用）
  app_time.c/h                   ★SNTP 对时 + 时间格式化（界面与大模型都用）
  app_settings.c/h               ★用户设置持久化（TF 卡上的背光亮度）
  app_chat_history.c/h           ★最近 10 轮对话历史
  app_chat_ui.c/h                ★LVGL 聊天界面
  app_gb2312.c/h                 ★utf8_to_gb2312() 与文本宽度工具
  app_splash.c, game_launcher/…  已有，保留
```

调用层级：`app/` → `my_drivers/`、`bsp/`；`my_drivers/` → `bsp/`；`bsp/` 不依赖上层。
app 层不直接碰 I2C/I2S 寄存器，WiFi 走 `my_drivers/net`，按键电平走 `bsp/bsp_gpio`。

## 五、任务与队列

| 任务 | 核心 | 优先级 | 栈 | 职责 |
| --- | --- | --- | --- | --- |
| `app_wifi_task` | 0 | 5 | 5120 | WiFi 连接 + 每 2.5 秒刷新左上角状态 + 执行界面发来的扫描/连接请求 + 掉线时扫已知网络自动重连 |
| `app_net_task` | 0 | 5 | 8192 | 取录音 → 百度 ASR → DeepSeek → 投递结果 |
| `app_key_task` | 1 | 4 | 4096 | 消费按键事件驱动状态机（IDLE→RECORDING→UPLOADING→LLM_REQUEST→IDLE） |
| `app_audio_task` | 1 | 6 | 5120 | 命令驱动采集 PCM 到 PSRAM，松手后上交数据 |
| `app_time`（对时） | 0 | 3 | 3072 | 等 WiFi 连上 → SNTP 对时 → 每 6 小时复查（见第八节） |
| `lvgl_port_task` | 1 | 已有 | 已有 | LVGL 刷新；聊天界面靠 lv_timer 在它的上下文里更新 |

| 队列 | 方向 |
| --- | --- |
| `key_event_queue` | app_key 定时器 → app_key_task |
| `audio_ctrl_queue` | app_key_task → app_audio_task |
| `pcm_queue` | app_audio_task → app_net_task |
| `asr_result_queue` | app_net_task → app_key_task |
| `llm_result_queue` | app_net_task → app_key_task（只报成功/失败，正文走 chat_msg_queue 流式投递） |
| `chat_msg_queue` | 任意任务 → 聊天界面（LVGL 定时器消费） |
| `ui_event_queue` | 聊天界面 → app_key_task（打字机“显示完了”回报） |

按键扫描用 `esp_timer` 20ms 周期回调（精度不受任务调度影响），
消抖状态机：`IDLE → DEBOUNCE_PRESS → PRESSED → DEBOUNCE_RELEASE → RELEASED`，
连续 2 次（40ms）同电平才算稳定；按下超过 15 秒产生 `TIMEOUT` 事件自动结束录音。

## 六、几个实现要点

- **录音格式**：板上是 ES7210 双麦 + I2S 立体声，录音器读立体声后做 `(L+R)/2` 降混成单声道，
  再以 16kHz/16bit 单声道交给 ASR，正好满足百度接口要求。
- **上传不复制**：识别请求体是 `{"format":"pcm",...,"speech":"<base64>","len":N}`，
  640KB 的 base64 如果整块拼进 JSON 会白吃一份内存，
  所以这里先算好 `Content-Length`，然后「JSON 前缀 + 分块 base64 + JSON 后缀」边编边发，
  只用 4KB 的编码缓冲。
- **token 缓存**：百度 access_token 有效期 30 天，缓存在内存里，提前 1 小时续期；
  服务端返回 `err_no=3302`（token 失效）时自动强制刷新并重试一次。
  ⚠️ 注意：取 token 的响应里带一整串 `scope`（几十项权限），实测约 1.4KB 且是
  **chunked 传输（没有 Content-Length）**，接收缓冲给小于这个长度就会被
  截断成非法 JSON —— 现在给的是 4KB（`s_token_resp` / `s_asr_resp`），
  日志里若出现「响应超过缓冲上限…已被截断」就是这个缓冲需要加大。
- **响应读取**：三个云端请求的响应体统一由 `app_http.c` 的事件回调累积
  （`http_on_body` 每解码出一段 body 就派发一次 `HTTP_EVENT_ON_DATA`，
  同一段只派发一次），再由 `app_http_pump()` 把 socket 抽干；
  解析失败时会把完整响应体打出来，便于定位。
- **对话历史**：内存里保留最近 10 轮（20 条），开机清空；请求时自动拼 `system + 历史 + 新 user`。
- **流式回复（SSE）**：请求 DeepSeek 时带 `stream: true`，
  响应是 `data: {...}` 一行一条的 SSE。`app_http.c` 提供了「按行回调」
  （`app_http_set_line_cb()`），`app_llm.c` 在每一行里取 `choices[0].delta.content`，
  取到一段就立刻回调出去 —— 这就是打字机的数据源。
  流式拿不到任何内容时自动降级为整段请求（把整段当一段“增量”回调），
  界面逻辑不用区分两种情况；但**一旦已经送过字就不再降级**，
  否则会出现“前半截流式 + 后半截整段”的重复文字。
  细节见第七节。
- **UTF-8 边界（这个坑很深）**：所有“按字节截断”的地方都必须回退到字符边界，
  否则会出现半个汉字，而半个汉字送进 JSON 请求体会让服务端直接回 `400 Invalid UTF-8`。
  涉及的函数：`app_utf8_char_bytes()`（判字符长度）、
  `bus_copy_text()`（总线拷贝）、`app_chat_history_add()`（历史截断）、
  `ws_append()`（联网搜索结果拼接）、`ui_type_push()`（打字机目标文本）。
  排查方法：日志里出现 `HTTP 400` + 服务端说 `Invalid UTF-8`，就是这里。
- **中文显示**：HZK16 是 GB2312 点阵，LVGL 的 hzk16 字体内部已经做了 Unicode→GB2312 转换，
  所以 label 直接喂 UTF-8 即可；`utf8_to_gb2312()` 用于按点阵宽度估算/截断文本。
- **TLS**：默认用 IDF 内置根证书包校验（`APP_TLS_USE_CA_BUNDLE=1`）。
  开发阶段若要跳过校验，把 `sdkconfig.defaults` 里的
  `CONFIG_ESP_TLS_INSECURE` / `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY` 打开，
  并把 `APP_TLS_USE_CA_BUNDLE` 改成 0。

## 七、流式回复（SSE）与打字机效果

目标：AI 的话**一段一段冒出来**，而不是等 5 秒后整段砸在屏幕上。
分两件事：**网络侧流式收包**（`app_llm.c` + `app_http.c`）和
**界面侧逐字显示**（`app_chat_ui.c`）。

### 7.1 网络侧：SSE 一行一行解析

```
请求体带 "stream": true  →  响应是一串 SSE：
   data: {"choices":[{"delta":{"content":"你"},"index":0}]}
   data: {"choices":[{"delta":{"content":"好"}}]}
   ...
   data: [DONE]
```

- `app_http.c` 新增 `app_http_set_line_cb()`：收包时按 `\n` 切行，
  每凑够一整行就回调一次（缓冲 `s_sse_line[APP_HTTP_SSE_LINE_MAX]`，
  行太长会丢弃并告警）；**同时仍然把整个响应累积到原来的 buffer 里**，
  所以整段解析的老逻辑完全不受影响。
- `app_llm.c` 的 `llm_sse_line()` 逐行用 cJSON 解析，取
  `choices[0].delta.content` 追加到 `s_stream`（累积全文）并立刻回调上层。
- **流式下的工具调用**：`tool_calls` 在流式里是**分片**下发的
  （先给 `id`/`name`，`arguments` 字符串一点点拼），所以 `s_stream.tools[]`
  按 `index` 累积片段，收完再一次性执行搜索，然后带结果**再发一次流式请求**
  （`llm_stream_restart()` 重置收包状态但保留回调与已累积正文）。
- 降级策略：整轮流式没拿到正文也没拿到工具调用 → 记 `AI无响应(流式)`，
  `app_llm_chat_stream()` 看到「没往界面送过字」就重发一次非流式请求。

### 7.2 界面侧：打字机（逐字显示）

```
app_net_task 收包 → net_llm_delta_cb() → CHAT_MSG_AI_DELTA →(队列)→ LVGL 定时器
                                                            ↓
                    追加进 s_type_target（目标全文，1KB，UTF-8 安全截断）
                                                            ↓
        30ms 定时器：每 APP_CHAT_TYPE_INTERVAL_MS(100ms) 从目标里取 1 个汉字
        （ASCII 算 1/3 个汉字）刷到 label，并 lv_obj_scroll_to_view 滚到底部
                                                            ↓
              收到 CHAT_MSG_AI_END → 把剩下的字一次补完 → 回报 TYPING_DONE
```

几个关键设计：

| 点 | 做法 / 原因 |
| --- | --- |
| 只在 LVGL 任务里碰控件 | 网络任务的回调只投队列（`app_bus_post_chat`），跨线程操作控件会随机崩 |
| 增量消息不丢 | `CHAT_MSG_AI_DELTA` 投递时会等队列空位（最多 300ms），丢一段就是回复缺字 |
| 目标文本裁剪 | 按 **完整 UTF-8 字符**截断，绝不留半个汉字（`ui_type_push()`） |
| label 只显示前缀 | 临时把 `s_type_target[s_type_shown]` 置 `'\0'` 后 `lv_label_set_text()`，刷完恢复 |
| 换行排版只切一次 | 文字超过气泡宽度才切成 `WRAP` 并定宽，避免每帧抖动 |
| 输入被锁住 | 打字期间状态是 `APP_STATE_AI_TYPING`，`app_key_task` 直接忽略按下 |
| 兜底 | `APP_CHAT_TYPING_TIMEOUT_MS`(90s) 内没收到 TYPING_DONE 就强制回待机 |
| 追赶 | 积压超过 `APP_CHAT_TYPE_CATCHUP_BYTES`(120B) 自动加速（每档 +1 字，最多 8 字/次） |

调参都在 `app_config.h`：`APP_CHAT_TYPE_INTERVAL_MS`（快慢）、
`APP_CHAT_TYPE_UNITS`（每次几个字）、`APP_CHAT_TYPE_CATCHUP_BYTES`（设 0 关闭追赶）、
`APP_CHAT_UI_TICK_MS`（定时器周期，默认 30ms）、`APP_CHAT_TYPING_TIMEOUT_MS`。

> ⚠️ **顺序有个坑**：`app_net` 必须先投 `llm_result`，再投 `CHAT_MSG_AI_END`。
> 反过来先发 AI_END 的话，界面可能已经把字打完并回报完成，
> 状态机这时才收到结果，就会把状态推成 `AI_TYPING` 而永远等不到回报 —— 按键就此失灵。

## 八、系统时间（SNTP 对时，`app_time.c`）

**为什么要它**：ESP32-S3 没有带电池的 RTC，开机时间是 1970-01-01。
大模型自己不知道“现在”，于是问它「今天是几号」，它会一本正经地编一个日期
（本工程实测：答成「2025年6月13日，星期五」）。加上对时后：
system 提示词里会带上真实日期时间，界面待机提示也会显示 `HH:MM`。

### 8.1 怎么工作

```
app_start() → app_time_start()              创建对时任务（Core 0 / 优先级 3 / 栈 3072）
                    ↓
      对时任务循环：app_wifi_is_connected()？没有就连 1 秒等一次
                    ↓ 有链路
      app_time_sync_once(15s)：esp_sntp_restart() → 等 time() 晚于 2020-01-01
                    ↓ 成功                        ↓ 失败
            记日志、等 6 小时再复查          等 30 秒重试
```

| 接口 | 用途 |
| --- | --- |
| `app_time_is_synced()` | 时间是否可信（SNTP 回调触发过，或 `time()` 晚于 2020-01-01） |
| `app_time_now_str(buf, len)` | `2026-09-25 16:30:12 星期四` |
| `app_time_now_clock(buf, len)` | `16:30`（界面状态行用） |
| `app_time_format(buf, len, fmt)` | `strftime` 包装，例如 `"%Y/%m/%d"` |
| `app_time_llm_text(buf, len)` | 给大模型的一句话；**没对时时返回“不知道当前时间”**而不是 1970 |
| `app_time_wait_synced(ms)` | 阻塞等时间就绪（别的模块想立刻用时间时） |

### 8.2 三个容易踩的点

1. **不能等 lwip 自己的轮询周期**：`CONFIG_LWIP_SNTP_UPDATE_DELAY` 默认 1 小时，
   开机时 WiFi 还没连上、第一次请求必然失败，那就要等一小时。
   本模块自己管重试节奏（30 秒一次），每次重试都主动 `esp_sntp_restart()`。
   → 顺带把 `CONFIG_LWIP_SNTP_MAX_SERVERS` 从 1 改成 3，可以配三个 NTP 服务器。
2. **时区必须先设**：`setenv("TZ", APP_TIME_TZ, 1); tzset();`，
   `APP_TIME_TZ` 默认 `"CST-8"`（东八区，注意符号是反的），
   否则你会看到 UTC 时间（比北京时间晚 8 小时）。
3. **没对时时不要编时间**：`app_time_llm_text()` 在未同步时给的是
   “设备还没和网络对时，你不知道当前日期和时间”，
   这样模型会老实说自己无法确定，而不是把 1970 当答案。

> 证书校验与时间的关系：本工程 `CONFIG_MBEDTLS_HAVE_TIME_DATE` 是**关**的，
> 也就是 mbedtls 不校验证书的有效期，所以“时间还是 1970”也能正常 TLS 握手。
> 如果你哪天把这项打开，就必须先对时再发 HTTPS 请求（否则全部握手失败）。

## 九、背光调节与 WiFi 状态显示（顶栏左上角）

### 9.1 背光（太阳图标 → 弹窗）

- **入口**：顶栏左上角 WiFi 文字右边的**太阳图标**。它是用 LVGL 基本图形画的
  （18×18 可点区域 + 中间实心黄圆 + 上下左右四根小光线）；子对象都关掉 CLICKABLE，
  这样点圆点或光线也会冒泡给容器，不会“点中间没反应”。
  不用图片资源、也不用字体图标 —— 中文字库是 GB2312 点阵，里面没有 ☀ 这类符号。
- **弹窗**：标题「屏幕背光设置」+ 当前值「亮度 60%」+ 滑条（0~100）+【确认】。
  拖动滑条**实时**调用 `lcd_brightness_set()`（`persist=false`，不写卡），
  只有点【确认】才写一次 TF 卡并关闭弹窗 —— 每拖一格就写卡既慢又费卡。
- **弹窗行为**：半透明遮罩吃掉下层点击；**只有【确认】会关闭**，
  避免用户“点外面以为取消了，其实亮度已经改了”。
- **持久化：TF 卡文件 + 内部 NVS 双份**（`app_settings.c`）
  - **TF 卡**：`/sd/chat_settings.txt`，纯文本可手改：

    ```
    ESP32TALK_CFG 1
    # 对话掌机的用户设置，由固件自动生成
    # 删掉本文件即恢复默认亮度 50%
    # backlight 取值范围 5~100
    backlight=60
    seq=7
    ```
  - **NVS**（nvs 分区 24KB，`chatset` 命名空间）：存 `backlight` + `seq` 两个键。
  - **保存**：两边**都写**（TF 卡先写并回读自检，NVS 再镜像一份）；
    卡不可用/写不进时就只写 NVS，并在状态行提示一次「TF卡写不进，已存到内部存储」。
  - **开机读取**：两边都读，**取 `seq` 更大的那一份**（默认值优先级最低）。
    这样拔卡期间改过设置、之后又插回旧卡，也不会被卡上的旧值顶掉。
  - 两个都失败（卡坏 + NVS 也写不进）才报「保存失败」——正常不会发生。

  为什么加 NVS：实测遇到过**数据区损坏的卡** —— 目录项/长度写得进去，
  数据区写入被静默丢弃（裸扇区自检全 FAIL，连电脑写进去的文件读回来也是全 0），
  这种卡上根本存不住东西。有了 NVS 兜底，卡坏了设置照样记得住。
  开关：`APP_SETTINGS_NVS_ENABLE`（`app_config.h`）。

  写盘/读盘都用 VFS 的裸接口，不走 stdio：
  `open(O_RDONLY) → read()`（长度用 `stat`）读，`open(O_WRONLY|O_CREAT|O_TRUNC) → write() → fsync() → close()` 写，
  写完还会**立刻用同一套接口回读自检**，把“报成功其实没落盘”当场暴露出来。

- **⚠️ 真正的原因在 `sdkconfig`：`CONFIG_FATFS_ALLOC_PREFER_EXTRAM` 必须关掉**

  踩了很久的坑，症状是**写进去的文件长度对、内容全是空的**（电脑上看也是空文件）：

  ```
  W APP_SET: /sd/chat_settings.txt（157 字节）里没有 backlight= 这一项，用默认值
  W APP_SET: 第一行原始内容：「」
  W APP_SET: 写完自检不一致：文件里读到 -1，本次写入的是 68
  ```

  157 字节正好等于写进去的 5 行长度 ⇒ 目录项是对的，**数据区是 0**。

  原因是 ESP32-S3 的 SDMMC 驱动写卡前不做 cache 回写：

  ```c
  /* components/esp_driver_sdmmc/src/sdmmc_transaction.c */
  #if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
      esp_cache_msync(buf, len, ...C2M);   /* 把 CPU cache 刷回内存后再 DMA */
  #endif
  ```

  而 `SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE` **只有 ESP32-P4 定义**（`soc/*/soc_caps.h`），
  S3 上这段被编译掉。于是：

  | FatFS 缓冲所在 | 结果 |
  | --- | --- |
  | 内部 RAM（S3 不经 L1 cache） | DMA 读到的一定是最新数据 ✅ |
  | PSRAM（`ALLOC_PREFER_EXTRAM=y` 的默认行为） | DMA 读到还躺在 cache 里的旧内容（整片 0）❌ |

  所以 `sdkconfig` / `sdkconfig.defaults` 里固定写上：

  ```
  CONFIG_FATFS_PER_FILE_CACHE=y
  # CONFIG_FATFS_ALLOC_PREFER_EXTRAM is not set
  ```

  代价只是 FatFS 的窗口/文件缓冲改用内部 RAM，多占 1~2KB。
  **这条对游戏存档同样重要**（`save_service.c` 也走同一套 FatFS 缓冲）。
- **读盘是宽松的**：只要文件里有一行 `backlight=N` 就用它，
  文件头 `ESP32TALK_CFG` 只作为提示，**不作为拒绝理由**
  （手改过的文件可能带 UTF-8 BOM `EF BB BF`，日志会把第一行按 `\xEF\xBB\xBF...` 转义打出来）。
  读失败时还会打印**读到多少字节 / 目录项多少字节 / 前 24 字节 HEX / TF 卡根目录 / 卡剩余容量**，
  是空文件、簇链断了、还是卡满了，一眼就能看出来。
- **TF 卡没插**：不报错，直接用默认 50%；用户照样能调亮度，只是**重启后回到 50%**
  （日志会写「TF 卡不可用…设置无法持久化」）。点【确认】时若卡不可用，
  状态行会红字提示「亮度已生效，但保存失败」。
- **0% 的坑（重要）**：滑条是 0~100，但**实际下限是 `APP_LCD_BRIGHTNESS_MIN = 5`**。
  因为 0% 是全黑，一旦存进卡里，下次开机就是黑屏：用户既看不到太阳图标、
  也点不到【确认】，机器等于锁死。真要“一键关屏”就把它改成 0，
  但黑屏后只能靠串口或拔卡删文件恢复。
- 开机在 `app_start()` 最前面读设置并应用（`lcd_init()` 里背光本来是 100%），
  所以不会出现“先刺眼亮一下再变暗”。

### 9.2 WiFi 状态（精简版 + 可点的文字按钮）

左上角只显示两项：**连没连 + 信号档位**，不显示 IP、不显示 dBm：

| 情况 | 显示 |
| --- | --- |
| 未连接 | `WiFi未连接` |
| RSSI ≥ `APP_WIFI_RSSI_STRONG`(-60 dBm) | `WiFi 强` |
| RSSI ≥ `APP_WIFI_RSSI_MEDIUM`(-75 dBm) | `WiFi 中` |
| 更低 | `WiFi 弱` |
| 正在连 | `WiFi连接中` |

- **这行字本身是个按钮**（见 9.3）：点它弹出附近 WiFi 列表。
  实现上就是“透明底的 `lv_button` 里放原来那个 label”，按下时半透明黄底做反馈 ——
  视觉上还是那行字，只是变得能点了。文字内容仍然由 `CHAT_MSG_WIFI` 消息驱动，
  所以刷新逻辑一行没改。
- **刷新在独立任务里**：就是 `app_wifi_task`（Core 0、优先级 5），和 LVGL 任务
  （Core 1）分开，所以 `net_is_connected()` / `net_get_rssi()` 绝不会卡住界面渲染。
- **刷新周期** `APP_WIFI_STATUS_REFRESH_MS = 2500`（2.5 秒，符合“2~3 秒、别刷屏”）；
  文字没变化时 `wifi_post_status()` 直接返回，连队列消息都不产生，界面更不会白重绘。
- IP 和 dBm 数值照旧打在**串口日志**里
  （`已连接 xxx：IP 192.168.1.9，信号 -52 dBm（强）`）。

### 9.3 点 WiFi 文字选网络 + 记住密码（`app_wifi_ui.c` / `app_wifi_store.c`）

**需求**：WiFi 状态那行字要能点，点开看到附近的 WiFi；点某个 WiFi 名弹出小键盘输密码；
连上的账号密码存进内部 NVS；最多存 10 个，存满覆盖最久没用过的；
没连上的时候定期扫附近“记过的”网络并自动连回去。

#### 9.3.1 界面长什么样

```
点左上角「WiFi未连接」                点列表里的某一项
┌────────────────────────────┐        ┌────────────────────────────┐
│ 附近 WiFi          共 8 个 │        │ 输入 WiFi 密码  正在连接... │
│ ┌────────────────────────┐ │        │ MyComputer                 │
│ │ MyComputer      已存 强│ │  ───▶  │ ┌────────────────────────┐ │
│ │ TP-LINK_5G      开放 中│ │        │ │ ******                 │ │
│ │ Xiaomi_A1            弱│ │        │ └────────────────────────┘ │
│ └────────────────────────┘ │        │   【连接】    【取消】     │
│   【重新扫描】   【关闭】   │        │ ┌── 小键盘（lv_keyboard）─┐│
└────────────────────────────┘        └────────────────────────────┘
```

- 列表行右边是信号档位（强/中/弱），另外会标 **「已存」**（NVS 里记过，行底色更亮）
  或 **「开放」**（不需要密码）。
- 点行 → 弹密码框；**开放网络**和**以前连过的网络**都可以直接点【连接】
  （留空 = 用 NVS 里记着的密码连），不用每次重输。
- 密码框是掩码输入（`*`），每敲一个字会**短暂显示原文** 600ms，确认自己没敲错。
  掩码字符特意用 ASCII 的 `*` 而不是 LVGL 默认的 `•`：中文字库是 GB2312 点阵，
  `•` 这种符号不一定有字模，`*` 走西文字体一定有。
- 小键盘就是 LVGL 的 `lv_keyboard`（自带字母/数字/符号切换、退格、确认、关闭），
  确认键 = 【连接】，关闭键 = 【取消】。
- 连接成功 → 提示「连接成功」，1.5 秒后**自动关掉弹窗**；失败 → 红字说明原因
  （「密码错误或热点不可达」/「连接超时」），并且**让【连接】按钮恢复可用**，方便改了密码再试。
- 弹窗期间会置“界面模态”标记（`app_bus_set_ui_modal`），
  **按住 GPIO0 录音键不生效** —— 用户正在输密码，屏幕底下冒出一句“录音中...”纯属误触。
  背光滑条弹窗同样会置这个标记。

#### 9.3.2 为什么界面不卡

扫描/连接**都是阻塞调用**：`net_scan()` 是全信道主动扫描（1~3 秒），`net_connect()`
最长 15 秒。要是放在 LVGL 任务里，界面直接冻住。

所以分工是：

```
LVGL 任务（点击回调 / lv_timer）             app_wifi_task（Core 0）
   app_wifi_scan_request()  ──┐
   app_wifi_connect_request() ─┴─▶ 请求队列 ─▶ net_scan() / net_connect()
   每 150ms 轮询：                                   │
     app_wifi_scan_state()   ◀── 状态 + 版本号 ◀─────┘
     app_wifi_scan_version()
     app_wifi_connect_state()
     app_wifi_connect_version()
```

- **用“版本号”判断有没有新结果**，不是“状态 == DONE”：状态是边沿型的，界面很容易错过
  （比如弹窗没开的时候刚好扫完一次），版本号只增不减，就永远不会漏。
- 列表结果由 `app_wifi_scan_snapshot()` **整体拷贝**给界面（内部有互斥锁），
  去重（同名 AP 只留最强的）+ 按信号从强到弱排序都在 WiFi 任务里做好。
- 顺带的好处：`app_wifi_init()` 在 `app_start()` 里就先调了一次（建请求队列 + 读 NVS 名单），
  所以界面一建好就能立刻用这两个接口，不用等 WiFi 任务被调度。

#### 9.3.3 名单存在哪、怎么淘汰（`app_wifi_store.c`）

内部 NVS（命名空间 `wifistore`，**不写 TF 卡** —— 密码放明文文本文件里更不安全，
而且卡可能被拔走）：

| 键 | 内容 |
| --- | --- |
| `cnt` | 记录条数 |
| `csq` | 全局“使用序号”计数器（只增不减） |
| `ssid<i>` / `pw<i>` / `sq<i>` | 第 i 条的 SSID / 密码 / 序号 |

- 每条记录的 `sq` 不是“保存时间”，而是**最近使用序号**：每次连接成功都会刷新，
  所以**存满 10 个时被覆盖的是“最久没用过的那个”**，常用的几个不会被新网络挤掉。
  日志里会明确写：`WiFi 名单已满（10 个），覆盖最久没用过的「xxx」-> 记下「yyy」`。
- 写入用整表刷一遍（30 来个键 + 一次 commit），只在“连接成功”时发生，不值得做增量。
- 名单整个放内存（10 条 × ~104 字节 ≈ 1KB），界面每画一行都要问一次“这个存过没”，
  走内存就不碰 flash 了。读写都有一把互斥锁保护（写者是 WiFi 任务，读者可能是 LVGL 任务）。
- NVS 打不开也不影响连接，只是“记不住”：日志会写 `NVS 初始化失败…（WiFi 名单这次存不住）`。
- 开机日志会把名单打出来（`app_wifi_store_dump()`），排查“为什么连的不是我以为的那个网”很有用。

#### 9.3.4 什么时候会自动连（`app_wifi.c`）

```
开机：先连 app_config_secret.h 里的 SSID（有配置的话）
        │ 连上 → 顺手把它也记进 NVS（以后改配置也不怕）
        ▼ 没连上
      等 5 秒（让 bsp_wifi 自己的重连任务先折腾，别两边抢 esp_wifi_connect）
        ▼ 还是没连上
      扫一遍附近 → 候选 = 编译期 SSID + NVS 里记着的所有网络
        │  按“扫到的信号强弱”排序（没扫到的垫底）
        ▼
      挨个试（一轮最多 APP_WIFI_TRY_MAX = 3 个），连上就记住 + 刷新顶栏
        │
        ▼ 之后主循环里
      没连上就每 APP_WIFI_KNOWN_SCAN_INTERVAL_MS(30 秒) 再走一遍上面这条路径
```

- 先用**扫描**筛一遍的好处：能知道哪个候选就在附近，不用对着一个不存在的 AP 干等超时。
- 30 秒这个周期远大于底层 `bsp_wifi_reconnect_task` 的 3 秒节奏，不会两边拉锯
  （旧版那条“每 20 秒重连一次 `APP_WIFI_SSID`”的兜底已经被这条路径取代，
  `APP_WIFI_MANUAL_RECONNECT_MS` 只是留作兼容的别名）。
- 界面上的连接请求会**把自动连接的倒计时清零**：用户刚操作过，别跟他抢。
- **编译期 SSID 没填也能用**：以前 `APP_WIFI_SSID` 还是占位符时 WiFi 任务会直接退出，
  现在它照样跑 —— 界面选一个网络连上，NVS 里就有了，下次开机自己会连回来。

#### 9.3.5 调参入口（`app_config.h` 第 13 节）

| 宏 | 默认 | 作用 |
| --- | --- | --- |
| `APP_WIFI_STORE_MAX` | 10 | 最多记几个网络（存满覆盖最久没用过的） |
| `APP_WIFI_SCAN_LIST_MAX` | 20 | 列表最多显示几个 AP（按信号排序） |
| `APP_WIFI_KNOWN_SCAN_INTERVAL_MS` | 30000 | 掉线后多久扫一次已知网络并尝试连接 |
| `APP_WIFI_TRY_MAX` | 3 | 一轮自动连接最多试几个候选 |
| `APP_WIFI_REQ_QUEUE_LEN` | 3 | 界面 → WiFi 任务的请求队列长度 |
| `APP_WIFI_UI_TICK_MS` | 150 | 弹窗轮询扫描/连接结果的周期 |
| `APP_WIFI_UI_CLOSE_DELAY_MS` | 1500 | 连接成功后多久自动关弹窗 |
| `APP_WIFI_LIST_ROW_H` | 30 | 列表行高 |


## 十、常见问题排查

| 现象 | 原因 / 处理 |
| --- | --- |
| 状态行「未配置SSID」 | `app_config.h` 里 `APP_WIFI_SSID` 还是占位符 |
| 状态行「获取token失败」 | 看 `APP_ASR` 日志：`error:invalid_client` = Key 填错；若出现「响应超过缓冲上限」= 加大 `s_token_resp` |
| 状态行「取token超时，请重试」 | 链路慢/刚断线重连，代码已自动重试一次；若持续出现，看下面那条「空闲十几秒被踢下线」 |
| 状态行「百度鉴权失败(检查Key)」 | Key/Secret 不对，或「短语音识别」服务没开通 |
| 状态行「未配置百度密钥」 | `app_config_secret.h` 里 `BAIDU_ASR_*` 还是占位符 |
| **空闲十几秒后第一次说话必失败**：日志 `与 AP 断开连接, reason=4` + `mbedtls_ssl_handshake returned -0x004C` | `reason=4` = AP 认为终端不活跃把它踢了（`WIFI_PS_NONE` 空闲时一个包都不发）；`-0x004C` = `MBEDTLS_ERR_NET_RECV_FAILED`。已改成 `WIFI_PS_MIN_MODEM`（bsp_wifi.c），并加了「识别失败且链路已断 → 等重连再重试」的兜底。若仍出现，换热点（实测 PC 热点不踢人，部分手机热点/WPA3 路由器会踢） |
| 日志 `getaddrinfo() returns 202` | 202 = lwip 的 `EAI_FAIL`，DNS 查询失败。已在 `sdkconfig.defaults` 打开 lwip 备用 DNS（`223.5.5.5`），主 DNS 不响应时自动切换 |
| 日志 `wifi:sta is connected, disconnect before connecting to new ap` | app 层和 bsp 层同时在调 `esp_wifi_connect()` 互相打架（会把已连上的链路断开重来）。已改成 app 层只在开机连一次，之后掉线超过 `APP_WIFI_KNOWN_SCAN_INTERVAL_MS`(30 秒) 才走“扫一遍已知网络”的兜底路径（限频，远慢于底层 3 秒的重连节奏） |
| 状态行「识别失败: 音频质量太差(3301)」 | 录音太小/太吵：确认对着板载麦克风说话，或调 `audio_set_mic_gain()`（默认 24dB） |
| 状态行「识别失败: 音频过短(3308)」 | 按住时间太短，多说一会儿（代码里 <400ms 的录音直接丢弃） |
| 状态行「识别失败: 鉴权失败(3302)」 | token 失效，代码会自动重取一次；仍失败说明 Key 无效或服务未开通 |
| 状态行「AI网络连接失败」 | 看 `APP_LLM` 日志；确认能访问 `api.deepseek.com`、Key 有效、账户有余额 |
| 状态行「AI服务错误: ...」 | 直接是 DeepSeek 返回的错误文本（额度/密钥/参数） |
| 日志 `流式没拿到内容…降级为一次性返回` | 服务端没给 SSE（或中途断了）：会自动整段请求一次，属正常兜底；若每次都这样，检查是否开了代理 |
| 回复缺字 / 只显示半句 | 看是否有 `聊天队列持续满，丢了 N 字节流式增量`；这是内部堆紧张导致界面来不及消费，先按 11.4 节释放内部 RAM |
| AI 答的日期是编的（例如问“今天几号”答成 2025 年） | 时间没对上：看有没有 `APP_TIME: 时间同步成功`。没有的话确认能访问 `ntp.aliyun.com`（UDP 123 不被热点屏蔽），或改 `APP_TIME_SNTP_SERVER1` |
| 界面时间比实际早/晚 8 小时 | 时区不对：`APP_TIME_TZ` 应为 `"CST-8"`（东八区，符号是反的） |
| `APP_TIME: 时间同步失败` 反复出现 | NTP 用的是 UDP 123，某些热点/校园网会拦；可先换成 `ntp.tencent.com`、`time.windows.com` 试，或忽略它（只有日期问答受影响） |
| 点左上角太阳图标没反应 | ①弹窗已经打开时再点无效（要先按【确认】）；②触摸没起来：看日志里 `lcd` 标签有没有触摸初始化失败 |
| 点左上角 WiFi 文字没反应 | ①背光弹窗开着（遮罩吃掉了点击），先关掉它；②看日志有没有 `APP_WIFI_UI: 打开 WiFi 列表弹窗`；③触摸没起来（同上一行） |
| 列表一直是「扫描中...」然后「扫描超时，请重试」 | WiFi 任务没在跑（初始化失败？）：看日志 `APP_WIFI: WiFi 初始化失败` / `创建 WiFi 请求队列失败`。扫描是阻塞的，任务卡在别的地方时也会这样 |
| 列表里没有我家 WiFi | 只显示 **2.4GHz**（ESP32-S3 不支持 5GHz）；隐藏 SSID 的 AP 也不会出现在列表里（没法点）。看串口日志 `扫描完成：N 个 AP` 确认到底扫到了几个 |
| 输完密码点【连接】没反应 | ①密码框和键盘都要求**触摸**输入，确认触摸正常；②看日志 `APP_WIFI: 界面请求连接「xxx」` 有没有出现；③`连接失败（密码错误或热点不可达）` 就是密码不对或 AP 太远 |
| 密码框里显示不出字 | 掩码字符是 ASCII 的 `*`（不是 `•`），输入时每敲一个字会显示原文 600ms。若连这个都看不到，检查 LVGL 内存是否被吃满（日志 `LVGL池` 占用率） |
| 连接成功但重启后不会自动连 | 看开机日志 `APP_WIFI_NVS: NVS 里读到 N/10 个 WiFi 记录`。如果 N=0，说明写 NVS 失败（紧跟一行 `写 NVS 失败：…`），多半是 nvs 分区满了/坏了 |
| 日志 `WiFi 名单已满（10 个），覆盖最久没用过的「xxx」-> 记下「yyy」` | 正常行为：最多记 10 个，满了就覆盖最久没用过的。想让常用的不被挤掉，就多用几次（每次连上都会刷新它的使用序号） |
| 换个地方开机半天连不上 | 看日志有没有 `掉线超过 30 秒，扫一遍附近的已知网络并尝试连接`；有的话再看 `已知网络 N 个，其中 M 个就在附近`。M=0 说明附近确实没有记过的网络，或者那两个热点是 5GHz |
| 日志 `自动重连 3 次均失败，停止重连` | 这是 `bsp_wifi` 自己的重连任务用尽次数了，属于正常；app 层的“扫已知网络”路径每 30 秒还会再试，不冲突 |
| 想清空记着的 WiFi | 用 `idf.py erase-flash`（会连 NVS 一起清掉），或者临时改代码调 `nvs_flash_erase()`。界面上没有“忘记网络”入口 |
| 亮度调完重启又变回 50% | 两个后端都写失败了：日志找 `TF 卡写不进去` 与 `写 NVS 失败`；插卡后重按一次【确认】即可 |
| 日志 `TF 卡写不进去(…)，背光设置已存到内部 NVS` | 正常行为（卡坏了/没插卡），设置仍然记得住；换张好卡后会自动重新写进卡里 |
| 日志 `底层自检 … 内部RAM:FAIL数据不符(读回全 0，写入被吞掉)` | **卡的数据区坏了**（与固件无关）：LBA0 能正常读出 `MSDOS5.0/FAT32/55AA` 说明读没问题，但数据区写不进——换卡 |
| **文件长度对、内容空**（电脑上看也是空文件）/ `写完自检不一致` | 就是本文第九节那条：`CONFIG_FATFS_ALLOC_PREFER_EXTRAM` 必须是 **n**（FatFS 缓冲不能用 PSRAM）。改了 sdkconfig 要重新编译烧写；开机第一行会自报 `编译配置：FatFS 缓冲在内部 RAM（FATFS_ALLOC_PREFER_EXTRAM=n）——正确` |
| 开机日志 `编译配置：FatFS 缓冲在 PSRAM（…=y）——…必须把它改成 n` | 烧的还是旧固件（sdkconfig 改动必须重新 build + flash 才会生效） |
| 日志 `底层自检（每处都写入并还原原内容）：LBA … 内部RAM:FAIL …` | `sdcard_selftest()` 绕开文件系统直接测扇区：内部 RAM 也失败 ⇒ 卡/驱动写不进去（假卡、坏块、接触不良）；只有 PSRAM 失败 ⇒ cache 一致性那条（固件没带 sdkconfig 修复）；两个都 OK ⇒ 卡没问题，问题在文件系统层 |
| 日志 `第一行原始内容：「」` | 数据区是空的（同上）；修复后应能看到 `读取 /sd/chat_settings.txt：157 字节，backlight=NN` |
| 日志 `目录项说 N 字节，实际只读到 M 字节` | 数据没落盘/簇链断了：看紧跟的 `HEX` 与 `TF 卡容量`（卡满？坏块？），必要时在 PC 上删掉该文件重存 |
| 开机日志 `文件头不是 ESP32TALK_CFG（但 backlight= 能解析）` | 文件被手改过（**记事本的“UTF-8”会加 BOM `EF BB BF`**）。值照样能用；日志会打印第一行的转义内容，照它就能看出多了什么字节 |
| 改了 `/sd/chat_settings.txt` 但不生效 | 只要文件里有一行 `backlight=60` 就行（解析很宽松：`backlight = 60`、Windows 换行、行尾注释都能认） |
| 屏幕全黑、怎么点都没反应 | 亮度被存成了 0（把 `APP_LCD_BRIGHTNESS_MIN` 改成 0 才会发生）：拔卡删掉 `/sd/chat_settings.txt` 再开机，或用串口 |
| 中文显示成方框 | `hzk16.bin` 没烧进 `fontbin` 分区，重新 `idf.py flash` |
| 录不到声音 / 全是静音 | 板载麦克风是 ES7210，确认 `audio_init()` 与 `audio_mic_init()` 都成功（看 `audio` 标签日志） |

## 十一、小游戏框架

原来的游戏中心（`game_launcher/`、`games/game_2048/`）代码完整保留。
聊天界面会新建并加载自己的 screen，会和游戏主页抢屏，
所以 `main/main.c` 里用 `APP_ENABLE_GAME_LAUNCHER` 开关控制，默认 `0`（不启动游戏框架）。
想同时保留，把它改成 1 即可。

## 十二、内存布局与缓冲大小

### 12.1 大文本缓冲全部放 PSRAM

板子有 8MB PSRAM，而内部 DIRAM 只有 ~334KB，还要留给任务栈、队列、WiFi/TLS 的运行时分配。
所以**大块文本缓冲一律用 `EXT_RAM_BSS_ATTR` 放进 PSRAM**（`sdkconfig` 里开了
`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`）：

| 缓冲 | 大小 | 位置 | 说明 |
| --- | --- | --- | --- |
| `s_asr_resp` / `s_token_resp` | 8KB / 6KB | PSRAM | 百度识别 / token 响应（token 响应实测 1.4KB 且是 chunked） |
| `s_asr_prefix` / `s_token_url` | 1KB / 512B | PSRAM | 拼请求用 |
| `s_asr_text` / `s_llm_reply` | 2KB / 2KB | PSRAM | ASR 文本 / AI 回复（一轮回复的硬上限 `APP_TEXT_MAX_LEN`） |
| `s_tool_results[2]` | ~10KB | PSRAM | Function Calling 工具结果（每条 `APP_WEB_SEARCH_TEXT_MAX+1` = 5KB） |
| `s_gb_tmp` | 4KB | PSRAM | UTF-8→GB2312 宽度估算 |
| `APP_LLM_RESP_BUF_SIZE` | 32KB | PSRAM（堆） | DeepSeek 响应，每次调用分配/释放 |
| `APP_WEB_SEARCH_RESP_BUF_SIZE` | 64KB | PSRAM（堆） | Tavily 响应，每次调用分配/释放 |
| `s_type_target` | 1KB | 内部 RAM（静态） | 打字机的目标全文（界面模块唯一的大静态量） |
| WiFi 名单（`app_wifi_store.c`） | ~1KB 静态 | 内部 RAM（静态） | 10 条 × (33+65+4) 字节，开机从 NVS 读进内存 |
| WiFi 扫描缓存（`app_wifi.c`） | ~2.5KB 静态 | 内部 RAM（静态） | 20 条原始结果 + 整理后列表（去重/排序用） |
| WiFi 弹窗（`app_wifi_ui.c`） | ~1KB 静态 + **LVGL 池** | 内部 RAM / LVGL 池 | 20 条列表快照；小键盘、输入框、每行按钮都吃 LVGL 池，弹窗关掉就全部归还 |
| `s_bubble_text` / `s_rx_msg`（界面、按键各一份） | 1KB×3 | 内部 RAM（静态） | 气泡裁剪缓冲、队列接收缓冲（**不能放栈上**） |
| `APP_CHAT_BUBBLE_TEXT_MAX` | 1KB/气泡 | **LVGL 内存池** | 只有它必须在 LVGL 池里（`CONFIG_LV_MEM_SIZE`） |

队列是运行时堆内存（内部 RAM），也按文本长度算过账：
`asr 2×2KB + llm 2×2KB + chat 6×1KB + ui 4×4B ≈ 14KB`。

### 12.2 内存监控：空闲任务触发 + 监控任务上报（`app_mem.c`）

分工（这点很重要，见下面的“坑”）：

| 角色 | 干什么 | 栈 |
| --- | --- | --- |
| 空闲任务（IDLE0）钩子 | 只做一次时间判断，到点 `xTaskNotifyGive()` 通知监控任务 | 1536 字节（IDF 默认，改不得） |
| 监控任务 `app_mem`（优先级 1） | 抢 LVGL 锁读池、读堆统计、打印 | 4096 字节（`APP_MEM_TASK_STACK`） |

开机先打一条基线，之后空闲任务每 30 秒触发一次：

```
I (xxx) APP_MEM: [启动基线] 内部空闲 160 KB（最大块 145 KB）| 历史最低 158 KB
I (xxx) APP_MEM: [启动基线] PSRAM 空闲 7.4 MB（最大块 7.4 MB）| LVGL池 12%（碎片 1%）
I (xxx) APP_MEM: [空闲任务] ...
W (xxx) APP_MEM: 内部 RAM 只剩 20 KB（阈值 24 KB）：建议调小 APP_TEXT_MAX_LEN ...
```

- **内部空闲 / 最大块**：最大块决定还能不能分配大缓冲（碎片多了就看它）；
- **历史最低**：开机以来最低水位，用来判断 TLS 握手那一下够不够；
- **LVGL 池**：气泡文字用的池子占用率与碎片率（读之前会先抢 LVGL 锁，拿不到就跳过这次统计）；
- 低于阈值会额外告警，并提示该调哪个宏。

> ⚠️ **踩过的坑**：第一版是直接在空闲任务钩子里 `ESP_LOGI`，结果开机就
> `A stack overflow in task IDLE0 has been detected`。原因是
> `CONFIG_FREERTOS_IDLE_TASK_STACKSIZE` 只有 1536 字节，而一次带多个参数的
> `ESP_LOGI`（vsnprintf + UART 写）就要几百字节，再加上读堆统计和 LVGL 池链表，
> 直接冲爆。而且空闲任务还负责回收被删除任务的 TCB/栈，绝不能在里面阻塞。
> 所以改成“空闲任务只发通知、监控任务干重活”。
> 如果你更想让空闲任务自己打印，就得把 `CONFIG_FREERTOS_IDLE_TASK_STACKSIZE`
> 提到 4KB 以上——但不推荐。

调参入口都在 `app_config.h`：`APP_MEM_LOG_INTERVAL_MS` 改周期、
`APP_MEM_LOG_ENABLE=0` 直接关掉、`APP_MEM_WARN_*` 改告警阈值、
`APP_MEM_TASK_PRIO` / `APP_MEM_TASK_STACK` 调监控任务本身。

### 12.3 任务栈监控与"栈炸弹"（踩过的大坑）

第二条日志里会多一行各任务栈剩余量（历史最小值，来自 `uxTaskGetStackHighWaterMark`）：

```
I (xxx) APP_MEM: 任务栈剩余(字节，历史最小值): app_wifi=3210 app_net=4520 app_key=2800
                 app_audio=3100 app_mem=2900 taskLVGL=3600
W (xxx) APP_MEM: 有任务栈剩余不足 1024 字节，随时可能爆栈（查局部大数组/大结构体）: ...
```

**踩过的坑**：把文本缓冲从 1152 放大到 4096 之后，
`app_bus_post_asr()` / `app_bus_post_chat()` 里的 `app_text_msg_t msg;` / `chat_msg_t msg;`
变成了 **4KB / 2KB 的局部变量**——它们落在**调用者的栈**上。
而 `app_audio_task`/`app_key_task` 的栈只有 4KB、`app_key_task` 里还有一个 4104 字节的
接收缓冲，于是栈被写穿，把相邻任务的栈/TCB 冲掉，表现为**另一个核上的任务**
莫名 `InstrFetchProhibited`（PC=0xffffffff，返回地址被写成 0xffffffff）。

修复方式（现在代码里就是这么写的）：

1. `app_bus_post_asr/llm/chat/chatf` 全部改用**静态暂存区 + 互斥锁**发送，
   函数自身栈帧降到 ~100 字节（锁只包住一次 `xQueueSend`）；
2. 任务里接收大消息的缓冲（`app_key_task` 的 ASR/LLM 结果、`app_chat_ui` 的聊天消息）
   一律改成 `static`；
3. `app_key`/`app_audio` 栈从 4096 提到 5120，留出余量。

**审计工具**：`components/app/CMakeLists.txt` 里加了 `-fstack-usage`，
每个源文件都会生成 `.su`，能看到每个函数的栈帧：

```bash
idf.py build
# 列出 app 组件里栈帧最大的函数（单位：字节）
grep -h -P '\t\d+\t' build/esp-idf/app/CMakeFiles/__idf_app.dir/*.su \
  | awk -F'\t' '{print $2, $1}' | sort -rn | head -20
```

修复前后对比：最大栈帧从 **4104 字节** 降到 **560 字节**（`llm_execute_tools`），
各任务入口函数（`app_net_task`/`app_audio_task`/`app_key_task`/`ui_timer_cb`）都在 64 字节以内。
新增代码后建议跑一次这个命令，只要没有几百字节以上的栈帧就很安全。

> 本次新增的 WiFi 代码也照样量过一遍（都是静态缓冲，函数里不放数组）：
> `app_wifi_connect_request` 256、`app_wifi_scan_request` 240、`wifi_try_known_aps` 176、
> `app_wifi_task` 160、`app_wifi_store.c` 160，`app_wifi_ui.c` 最大只有 80 字节
> （界面那些 20 行的列表、键盘控件、扫描快照全部是文件级静态量）。

### 12.4 文本缓冲尺寸怎么定（稳定优先）

内部堆只有 ~139KB，还要养任务栈（~20KB）、WiFi/LWIP（~40KB）、TLS 连接（~22KB）。
**队列是内部堆内存**，所以"把文本缓冲改大"最容易踩的坑就是把内部堆挤干——
挤干之后不是"分配失败报个错"，而是 WiFi 驱动拿不到收包缓冲、TLS 写阻塞，
表现成上传卡 20 秒后 `Poll timeout … 写请求体失败`。

当前配置（稳定优先，实测内部堆余量充足）：

| 项 | 值 | 在哪里占内存 |
| --- | --- | --- |
| `s_token_resp` | 6KB（百度实际响应 ~1.4KB） | PSRAM（`EXT_RAM_BSS_ATTR`） |
| `s_asr_resp` | 8KB | PSRAM |
| `APP_TEXT_MAX_LEN` | 2048（≈1000 汉字） | asr/llm 队列，内部堆 2×2KB |
| `APP_CHAT_TEXT_MAX` | 1024 | chat 队列，内部堆 6×1KB |
| `APP_CHAT_BUBBLE_TEXT_MAX` | 1024 | LVGL 池（64KB，实测占用 ~14%） |
| 三个队列合计 | **≈14KB** | 内部堆（改大前激进版是 32.8KB） |
| `APP_LLM_RESP_BUF_SIZE` | 32KB | PSRAM 堆（每次请求分配/释放） |
| `APP_WEB_SEARCH_RESP_BUF_SIZE` | 64KB | PSRAM 堆 |
| 工具结果 `s_tool_results[2]` | 2×5KB | PSRAM |

**想再放大文本**：优先动 PSRAM 上的（`APP_LLM_RESP_BUF_SIZE`、`APP_WEB_SEARCH_*`、
`APP_TEXT_MAX_LEN` 之外那几个 `EXT_RAM_BSS_ATTR` 缓冲）；
动队列尺寸前先看日志里的"内部空闲 / 最大块"，留出 30KB 以上余量再动。

**ASR 上传失败会自动重试**（两级，见 `app_asr.c`）：

1. 取 token 超时 → 等 1 秒重试一次；
2. 识别没拿到服务端响应（传输层失败，例如上传中途链路卡死）
   → **链路正常**就等 1.5 秒换新连接重试一次；
      **链路已断**（`app_wifi_is_connected()` 为假，通常伴随 `reason=4` 被踢）则最多等
     `APP_ASR_WAIT_LINK_MS`(15s) 让 WiFi 重连成功后再重试，等不到才报「WiFi 已断开」。

日志里能看到 `没拿到服务端响应（上传失败），1.5 秒后换新连接重试一次`。

### 12.5 双核任务分配（含 LVGL 任务，别让它被网络任务饿死）

| 核心 | 任务（优先级） |
| --- | --- |
| Core 0（网络核） | `app_wifi(5)`、`app_net(5)`、`app_mem(1)`、WiFi/LWIP 内部任务 |
| Core 1（应用核） | `app_audio(6)` > `taskLVGL(5)` > `app_key(4)` |

**踩过的坑**：`ESP_LVGL_PORT_INIT_CONFIG()` 默认 `task_affinity = -1`（不绑核），
而 `lcd_init()` 是在 **main 任务**里跑的、main 被钉在 CPU0，
于是 LVGL 任务也落在 CPU0，优先级 4 —— **比 `app_wifi`/`app_net`(5) 低**。
后果：只要网络任务在跑 TLS/HTTP，LVGL 就被饿死，界面不刷新，
表现为"识别结果和 AI 回复两个气泡一起冒出来""录音中的状态字不动""整机反应变慢"。

现在在 `lcd.c` 里显式指定：`lvgl_cfg.task_affinity = 1; lvgl_cfg.task_priority = 5;`
（`LCD_LVGL_TASK_CORE` / `LCD_LVGL_TASK_PRIO`）。

**对话上屏时序**（改完后，含流式与打字机）：

```
按住 GPIO0  → 状态“录音中... 松开发送”（30ms 内上屏）
松手        → 状态“识别中...”
百度返回识别结果
     → 用户气泡立刻上屏 + 状态“AI 正在回复...”（app_net 直接投递，不等转发）
DeepSeek 第一段正文到达（通常 1~2 秒）
     → AI 气泡出现并开始**逐字**冒字（状态“输出中”）
流结束      → 剩下的字打完 → 状态回到“按住按键说话”（这时才允许再次录音）
```

注意：网络本身的耗时没变（TLS 握手 + 上传 + 模型推理），
变的是**界面不再等网络**：用户说的话先看到，AI 的话边生成边显示。

### 12.6 注意事项

- 打开 `.bss` 放 PSRAM 后，**PSRAM 变成硬依赖**：IDF 会把 `lwip`/`net80211` 等的
  `.bss` 一并放到 PSRAM（这是该选项的官方行为，也正是它省出 ~50KB DIRAM 的原因）。
  板子上的 PSRAM 一直正常就没问题；想退回原来的内部 RAM 布局，把这个选项关成 `n` 即可
  （`EXT_RAM_BSS_ATTR` 会自动变成空属性，代码不用改，只是缓冲回到内部 RAM）。
- `CONFIG_LV_MEM_SIZE` 是**内部 RAM 的静态数组**，别盲目加大：它的池子是给
  LVGL 控件和气泡文字用的，气泡数 × 单个气泡文字长度别超过池子的一半。
- 想再放大文本能力，优先动 PSRAM 上的那几个宏（`APP_TEXT_MAX_LEN`、
  `APP_LLM_RESP_BUF_SIZE`、`APP_WEB_SEARCH_*`），不要动 LVGL 池。
