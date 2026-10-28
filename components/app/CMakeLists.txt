# app 组件：上层应用业务 / LVGL 界面 / 对话掌机
# 只依赖 my_drivers 暴露的接口、bsp 的按键 GPIO 与 LVGL / 网络协议栈组件，
# 不直接操作系统寄存器、也不直接调 esp_wifi（WiFi 走 my_drivers/net）
#
# ---------------------------------------------------------------------------
# 私有密钥（app_config_secret.h）：
#   它不入库（见工程根目录 .gitignore），但 app_config.h 会无条件 #include 它。
#   这里在 **配置阶段** 保证它一定存在：不存在就从模板复制一份（内容是占位符）。
#   为什么不用 __has_include 让 include 变成可选：
#     那样在“先没密钥构建过一次”的情况下，编译依赖文件里不会记录这个头文件，
#     之后补上密钥时 ninja 认为无变化、**不会重编**，固件里还是占位符
#     （现象：一直提示“未配置密钥”）。做成普通 include 就不会有这个坑。
# ---------------------------------------------------------------------------
set(APP_SECRET_HDR   "${CMAKE_CURRENT_LIST_DIR}/app_config_secret.h")
set(APP_TEMPLATE_HDR "${CMAKE_CURRENT_LIST_DIR}/app_config_template.h")

if(NOT EXISTS "${APP_SECRET_HDR}")
    configure_file("${APP_TEMPLATE_HDR}" "${APP_SECRET_HDR}" COPYONLY)
    message(STATUS "app: 已由模板生成 app_config_secret.h —— 请填入真实密钥"
                   "（WiFi / 百度 / DeepSeek / Tavily），该文件不会被提交")
endif()

#   开机动画
#     app_splash.c                  开机动画（预留 TF 卡 MJPEG 钩子）
#   对话掌机（本次新增）
#     app.c / app.h                 应用入口 app_start()：初始化 + 创建双核任务
#     app_config.h                  WiFi/密钥/任务参数等全部宏定义
#     app_bus.c                     业务状态机 + 跨任务队列（按键/PCM/ASR/LLM/聊天）
#     app_key.c                     GPIO0 消抖状态机(esp_timer) + 业务状态机任务
#     app_audio.c                   音频采集任务（PCM 写 PSRAM 缓冲）
#     app_wifi.c                    WiFi(STA) 连接与断线重连任务
#     app_net.c                     网络任务：ASR -> LLM
#     app_asr.c                     百度短语音识别（token 缓存 + 分块 base64 上传）
#     app_llm.c                     DeepSeek Chat Completions
#     app_chat_history.c            最近 10 轮对话历史
#     app_chat_ui.c                 LVGL 聊天界面（气泡 + 状态栏）
#     app_gb2312.c                  UTF-8 <-> GB2312 与文本宽度工具
#     app_http.c                    HTTP 响应体累积读取 + JSON 解析兜底
#     app_web_search.c              Tavily 联网搜索（DeepSeek Function Calling 工具）
#     app_time.c                    SNTP 对时 + 时间格式化（喂给界面与大模型）
#     app_settings.c                TF 卡上的用户设置（背光亮度持久化）
#     app_power.c                   低功耗：IMU+触摸静止检测 -> 关屏/WiFi 省电 -> light sleep
#     app_wifi_store.c              连过的 WiFi 名单（SSID+密码）存内部 NVS，最多 10 个
#     app_wifi_ui.c                 附近 WiFi 列表弹窗 + 小键盘输密码弹窗
#     app_mem.c                     空闲任务里的内存监控（堆 / PSRAM / LVGL 池）
#   小游戏（保留）
#     game_launcher/                游戏管理器 + 主页 + 存档服务
#     games/game_2048/              2048 游戏
idf_component_register(
    SRCS
        "app_splash.c"
        "app.c"
        "app_bus.c"
        "app_key.c"
        "app_audio.c"
        "app_wifi.c"
        "app_net.c"
        "app_asr.c"
        "app_llm.c"
        "app_chat_history.c"
        "app_chat_ui.c"
        "app_gb2312.c"
        "app_http.c"
        "app_web_search.c"
        "app_time.c"
        "app_settings.c"
        "app_power.c"
        "app_wifi_store.c"
        "app_wifi_ui.c"
        "app_mem.c"
        "game_launcher/game_manager.c"
        "game_launcher/launcher_ui.c"
        "game_launcher/save_service.c"
        "games/game_2048/game_2048.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        my_drivers
        bsp
        esp_lvgl_port
        lvgl
        esp_http_client
        esp-tls
        mbedtls
        json
        esp_timer
        esp_wifi
    PRIV_REQUIRES
        esp_common
        esp_event
        esp_netif
        lwip
        heap
        nvs_flash
)

# 生成每个函数的栈用量（.su 文件，放在 build/esp-idf/app/CMakeFiles/__idf_app.dir/ 下）
# 用来审计“大数组/大结构体放在小栈任务里”这类问题：
#   grep -E "\s[0-9]{4,}\s" *.su
target_compile_options(${COMPONENT_LIB} PRIVATE "-fstack-usage")
