/**
 * ╔═══════════════════════════════════════════════════════════════════════════╗
 * ║                    XiaoZhi AI 聊天机器人 - 主程序                           ║
 * ╚═══════════════════════════════════════════════════════════════════════════╝
 * 
 * 📋 文件用途：
 *    实现 Application 类，负责整个设备的启动、事件驱动循环、状态管理
 * 
 * 🏗️ 系统架构（分层设计）：
 * 
 *    ┌─────────────────────────────────────────────────────────────┐
 *    │         User Interaction Layer (用户交互)                   │
 *    │  • 唤醒词检测 (Wake Word)                                   │
 *    │  • 按钮控制、语音命令                                        │
 *    └─────────────────────────────────────────────────────────────┘
 *                              ↓
 *    ┌─────────────────────────────────────────────────────────────┐
 *    │     Application Main Loop (主事件循环)                      │
 *    │  • FreeRTOS EventGroup - 事件驱动                           │
 *    │  • DeviceStateMachine - 状态转换                           │
 *    │  • Schedule Queue - 线程安全回调队列                        │
 *    └─────────────────────────────────────────────────────────────┘
 *                    ↙        ↓        ↘
 *    ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
 *    │ Audio Layer  │  │ Network Layer│  │ Display Layer│
 *    │              │  │              │  │              │
 *    │ • AFE 处理    │  │ • WebSocket  │  │ • LVGL/OLED  │
 *    │ • OPUS 编码   │  │ • MQTT       │  │ • 状态栏     │
 *    │ • 麦克风      │  │ • JSON-RPC   │  │ • 聊天界面   │
 *    │ • 扬声器      │  │ • MCP 工具   │  │              │
 *    └──────────────┘  └──────────────┘  └──────────────┘
 * 
 * 📊 运行流程（五个阶段）：
 * 
 *    阶段 1: app_main() → Initialize()
 *    ├─ 状态: Starting
 *    ├─ 位置: main.cc → application.cc
 *    └─ 职责: 快速初始化 UI、音频、网络回调（异步启动网络）
 * 
 *    阶段 2: Run() - 主事件循环（永不返回）
 *    ├─ 状态: Starting → Activating (后台) → Idle
 *    ├─ 位置: xEventGroupWaitBits() → 阻塞等待事件
 *    └─ 职责: 处理所有事件、驱动状态转换
 * 
 *    阶段 3: Network Connected → ActivationTask (后台任务)
 *    ├─ 状态: Activating
 *    ├─ 职责: CheckAssetsVersion() → CheckNewVersion() → InitializeProtocol()
 *    └─ 完成: MAIN_EVENT_ACTIVATION_DONE → 状态转换为 Idle
 * 
 *    阶段 4: Wake Word Detection (离线)
 *    ├─ 状态: Idle
 *    ├─ 职责: AFE 唤醒词检测 (ESP-SR) → 本地模型运行
 *    └─ 触发: MAIN_EVENT_WAKE_WORD_DETECTED → 启动对话
 * 
 *    阶段 5: Voice Interaction (对话)
 *    ├─ 状态: Connecting → Listening → Speaking → Idle
 *    ├─ 职责: 音频收发、网络通信、UI 更新
 *    └─ 循环: 直到用户停止或网络断开
 * 
 * 🎯 关键设计原则：
 * 
 *    1️⃣ 事件驱动 (Event-Driven)
 *       • FreeRTOS EventGroup 作为事件总线
 *       • 各个任务通过设置事件通知主循环
 *       • 主循环统一处理所有事件 → 避免竞争条件
 * 
 *    2️⃣ 单向状态机 (State Machine)
 *       • 合法的状态转换由 DeviceStateMachine 校验
 *       • HandleStateChangedEvent() 统一处理所有状态转换的副作用
 *       • LED、UI、音频都通过同一个函数协调
 * 
 *    3️⃣ 线程安全的 UI 更新
 *       • 只有主任务能访问 LVGL
 *       • 其他任务通过 Schedule() 函数加入回调队列
 *       • 主循环执行这些回调 → 安全且无锁
 * 
 *    4️⃣ 异步网络和音频处理
 *       • 网络操作在独立的 Protocol 任务中
 *       • 音频处理在 AudioService 任务中
 *       • 主任务只负责协调，不做 I/O 操作
 * 
 * 📡 消息格式（通过网络发送）：
 * 
 *    基础协议层: WebSocket / MQTT
 *                 ↓
 *    JSON-RPC 2.0 消息 (type + payload)
 *                 ↓
 *    消息类型:
 *    • "stt"    → 语音识别结果 (用户说的)     [Listening 时接收]
 *    • "tts"    → 文本转语音 (AI 回复)        [Speaking 时接收]
 *    • "llm"    → 大模型输出 (情感表达)        [实时]
 *    • "mcp"    → MCP 工具调用 (JSON-RPC)    [动态]
 *    • "system" → 系统命令 (重启、升级)       [特殊]
 *    • "alert"  → 警告信息                    [错误时]
 * 
 * 🔊 音频数据流：
 * 
 *    麦克风录制:
 *    Microphone → AFE (VAD, AEC) → OPUS Encoder
 *                      ↓
 *                  Send Queue → Network → Server
 * 
 *    扬声器播放:
 *    Server → Receive Queue → OPUS Decoder → Speaker
 * 
 * ⚡ 性能优化：
 * 
 *    • 主任务优先级 10 (高) → 确保快速响应事件
 *    • 激活任务优先级 2 (低) → 后台执行，不阻塞主循环
 *    • 功耗模式: 待机用 LOW_POWER, 对话用 PERFORMANCE
 *    • OPUS 帧长: 60ms → 权衡延迟和压缩率
 * 
 * 🛑 错误处理：
 * 
 *    • 网络错误 → MAIN_EVENT_ERROR → Alert() → 用户提示
 *    • 状态转换错误 → 静默忽略
 *    • 音频错误 → 可恢复则继续，否则显示 Alert
 *    • 致命错误 → Reboot() → esp_restart()
 */

#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"  // 日志标签


/**
 * 构造函数 - 初始化核心资源
 */
Application::Application() {
    event_group_ = xEventGroupCreate();  // 创建 FreeRTOS EventGroup (事件总线)

    // 检测 AEC (回音消除) 配置: 设备端 / 服务器端 / 禁用
#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;  // 设备端 AEC (实时模式)
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;  // 服务器端 AEC (自动停止模式)
#else
    aec_mode_ = kAecOff;           // 禁用 AEC (自动停止模式)
#endif

    // 创建时钟定时器: 每 1 秒触发一次 MAIN_EVENT_CLOCK_TICK (用于更新 UI)
    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

// 设置设备状态 (线程安全的状态转换)
bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);  // 状态机验证合法性 + 触发 MAIN_EVENT_STATE_CHANGED
}
/**
 * 初始化应用程序 - 系统启动时调用 (在 app_main() 中)
 * 注意: 此函数应该快速返回，不应该阻塞
 */
void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);  // 显示启动画面

    // 第一步: UI 初始化
    auto display = board.GetDisplay();
    display->SetupUI();                      // 初始化 LVGL/OLED
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());  // 显示版本信息

    // 第二步: 音频引擎初始化
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);        // 初始化 OPUS 编码器
    audio_service_.Start();                  // 启动麦克风 + AFE (音频前端处理)

    // 第三步: 注册音频回调
    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {  // 音频编码完成，待发送
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {  // 检测到唤醒词
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {  // 检测到语音活动
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // 第四步: 监听状态转换
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);  // 触发状态转换处理
    });

    // 第五步: 启动时钟定时器 (每秒更新 UI 状态栏)
    esp_timer_start_periodic(clock_timer_handle_, 1000000);  // 1000000 us = 1 秒

    // 第六步: 注册 MCP 工具 (设备能通过 AI 做什么)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();        // 通用工具: 获取状态、调整音量、拍照
    mcp_server.AddUserOnlyTools();      // 用户工具: 重启、固件升级、屏幕截图

    // 第七步: 注册网络事件回调 (后台任务会触发这些事件)
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:  // WiFi 扫描中
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);  // 标记网络未连接
                break;
            case NetworkEvent::Connecting: {  // 正在连接
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {  // 网络连接成功 ← 激活流程的起点
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);  // 🎯 关键事件
                break;
            }
            case NetworkEvent::Disconnected:  // 网络断开
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // 第八步: 异步启动网络连接 (不阻塞主循环)
    board.StartNetwork();              // 后台启动 WiFi/4G 连接
    
    // 立即更新 UI (显示正在扫描、连接中等状态)
    display->UpdateStatusBar(true);
}

/**
 * 主事件循环 - Application 的心脏，永远不返回
 * 核心逻辑: 事件驱动 (Event-Driven) + 状态转换 (State Machine)
 */
void Application::Run() {
    vTaskPrioritySet(nullptr, 10);  // 主任务优先级 10 (高) → 快速响应事件

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    // 永远循环，每次等待一个或多个事件
    while (true) {
        // ⏸️ 阻塞等待事件 (pdTRUE=清除已处理的事件, pdFALSE=不用全部事件)
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        // 事件处理 (按优先级顺序)
        if (bits & MAIN_EVENT_ERROR) {  // 🔴 错误事件
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {  // 🌐 网络连接成功
            HandleNetworkConnectedEvent();  // 启动激活流程
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {  // 🌐 网络断开
            HandleNetworkDisconnectedEvent();  // 关闭正在进行的对话
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {  // ✅ 激活完成
            HandleActivationDoneEvent();  // 进入待机状态
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {  // 🔄 状态转换
            HandleStateChangedEvent();  // 更新 UI、音频、LED
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {  // 👂 手动启动/停止对话 (按钮触发)
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {  // 👂 开始录音
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {  // 🛑 停止录音
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {  // 📤 音频编码完成，发送到服务器
            // 弹出所有待发送的 OPUS 数据包，通过网络发送
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;  // 网络阻塞，停止继续发送
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {  // 🎤 唤醒词检测 (离线)
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {  // 🔊 语音活动状态变化 (用于 LED 指示)
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();  // LED 随着有无语音而变亮/变暗
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {  // 📋 执行其他任务调度的回调
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);  // 原子性交换 (不复制)
            lock.unlock();
            for (auto& task : tasks) {
                task();  // 在主任务中执行 → 安全更新 UI
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {  // ⏰ 每秒时钟事件
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();  // 更新状态栏 (时间、电池、网络等)
        
            if (clock_ticks_ % 10 == 0) {  // 每 10 秒
                SystemInfo::PrintHeapStats();  // 打印内存使用统计（调试用）
            }
        }
    }
}

/**
 * 网络连接事件处理 - 触发激活流程
 * 🌐 场景: WiFi/4G 连接成功 → 启动设备初始化
 */
void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // 🚀 网络就绪 → 启动后台激活任务
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        // 创建后台任务 (优先级 2, 栈 8KB)
        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();  // ← 三步激活: 资源 → 固件 → 协议
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);      // 任务自我删除
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);  // 立即更新 UI
}

/**
 * 网络断开事件处理 - 优雅关闭对话
 * 🔴 场景: WiFi/4G 信号丢失 → 立即停止音频传输
 */
void Application::HandleNetworkDisconnectedEvent() {
    auto state = GetDeviceState();
    // 如果在对话中 → 立即关闭音频通道
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();  // ← 中止正在进行的对话
    }

    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}
/**
 * 激活完成事件处理 - 设备初始化成功 ✅
 * ✅ 此时: 资源已下载、固件已检查、协议已初始化
 */
void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();                        // 打印内存统计
    SetDeviceState(kDeviceStateIdle);                   // 进入待机状态

    has_server_time_ = ota_->HasServerTime();           // 标记是否收到服务器时间

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());         // 显示固件版本
    display->SetChatMessage("system", "");             // 清空系统消息

    ota_.reset();                                       // 释放 OTA 对象 (不再需要)
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // 恢复低功耗模式

    Schedule([this]() {
        // 在主任务中播放成功提示音 (100ms 后)
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

/**
 * 激活后台任务 - 设备初始化的三大步骤
 * 💾 运行环境: 独立的 FreeRTOS 任务
 * ⏱️ 典型耗时: 5-30 秒 (取决于网络和固件大小)
 */
void Application::ActivationTask() {
    ota_ = std::make_unique<Ota>();  // 创建 OTA 对象 (版本检查、固件升级)

    CheckAssetsVersion();              // 第1步: 检查/下载资源包 (字体、表情、音效)
    CheckNewVersion();                 // 第2步: 检查/升级固件版本
    InitializeProtocol();              // 第3步: 初始化通信协议 (WebSocket/MQTT)

    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);  // ← 通知主循环
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

/**
 * 初始化通信协议 - 建立与后台 API 的双向通道
 * 📡 基础层: WebSocket 或 MQTT (由 OTA 配置决定)
 * 📦 应用层: JSON-RPC 2.0 (工具调用、消息传递)
 */
void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);  // 显示 "正在连接服务器..."

    // 选择协议
    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();  // MQTT: 低延迟、轻量级
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();  // WebSocket: 双向实时
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    // 🔧 关键回调 #1: 连接成功
    protocol_->OnConnected([this]() {
        DismissAlert();  // 清除连接中的提示
    });

    // 🔧 关键回调 #2: 网络错误
    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);  // 触发错误处理
    });
    
    // 🔧 关键回调 #3: 接收 TTS 音频 (AI 回复)
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));  // OPUS → PCM → Speaker
        }
    });
    
    // 🔧 关键回调 #4: 音频通道打开
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);  // 关闭低功耗优化
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Sample rate mismatch: %d vs %d",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    // 🔧 关键回调 #5: 音频通道关闭
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);  // 恢复低功耗
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);  // 对话结束，回到待机
        });
    });
    
    // 🔧 关键回调 #6: 接收 JSON 消息 (STT/TTS/LLM/MCP/System)
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {  // 📢 TTS 状态变化
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);  // AI 开始说话
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        // 根据监听模式返回 Listening (自动停止) 或 Idle (手动停止)
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);  // 继续监听下一句
                        }
                    }
                });
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {  // 🎤 语音识别结果
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());  // 显示用户说的话
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {  // 🤖 LLM 情感
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());  // 更新 AI 表情
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {  // 🛠️ MCP 工具调用
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);  // 解析 (JSON-RPC 2.0)
            }
        } else if (strcmp(type->valuestring, "system") == 0) {  // ⚙️ 系统命令
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                if (strcmp(command->valuestring, "reboot") == 0) {
                    Schedule([this]() { Reboot(); }); // 远程重启
                }
            }
        }
    });
    
    protocol_->Start();  // 🚀 启动协议，连接到服务器
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() {
                ContinueOpenAudioChannel(kListeningModeManualStop);
            });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();
        auto wake_word = audio_service_.GetLastWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update),
            // then continue with OpenAudioChannel which may block for ~1 second
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue());

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();  // Clear messages first
            display->SetEmotion("neutral"); // Then set emotion (wechat mode checks child count)
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for playback queue to be empty before enabling voice processing
                // This prevents audio truncation when STOP arrives late due to network jitter
                if (listening_mode_ == kListeningModeAutoStop) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
            }

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            // Enable wake word detection in listening mode (configured via Kconfig)
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
            // Disable wake word detection in listening mode
            audio_service_.EnableWakeWordDetection(false);
#endif
            
            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

/**
 * 执行计划的回调 - 线程安全的任务队列
 * 💡 设计: 非主任务通过 Schedule() 将 UI 操作加入队列
 *         主循环在 MAIN_EVENT_SCHEDULE 时执行
 *         → 所有 UI 操作都在同一个任务中执行
 *         → 避免 LVGL 线程安全问题
 */
void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);  // 互斥锁保护
        main_tasks_.push_back(std::move(callback));  // 加入队列
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);  // 通知主循环
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

/**
 * 重启设备 - 优雅关闭所有资源后重启
 * 🔄 关闭顺序很重要! 不能反序!
 */
void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    
    // 第1步: 停止音频传输
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    
    // 第2步: 关闭网络连接
    protocol_.reset();  // 销毁 Protocol 对象
    
    // 第3步: 停止音频服务
    audio_service_.Stop();  // 关闭麦克风、扬声器、I2S

    // 第4步: 等待一切就绪
    vTaskDelay(pdMS_TO_TICKS(1000));  // 1 秒延迟
    
    // 第5步: 硬件重启
    esp_restart();  // ← 触发 ESP32 硬件重启
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload = std::move(payload)]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetProtocol() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

