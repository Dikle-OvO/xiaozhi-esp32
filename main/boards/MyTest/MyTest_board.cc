#include "wifi_board.h"
#include "codecs/inmp441_audio_codec.h"
#include "config.h"
#include "display/display.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "settings.h"
#include "power_save_timer.h"
#include "adc_battery_monitor.h"
#include "press_to_talk_mcp_tool.h"

#include <wifi_manager.h>
#include <esp_log.h>
#include <esp_efuse_table.h>

#define TAG "MyTest"

class MyTest : public WifiBoard {
private:
    Display* display_ = nullptr;
    Button boot_button_;
    PowerSaveTimer* power_save_timer_ = nullptr;
    AdcBatteryMonitor* adc_battery_monitor_ = nullptr;
    PressToTalkMcpTool* press_to_talk_tool_ = nullptr;

    void InitializePowerManager() {
        adc_battery_monitor_ = new AdcBatteryMonitor(ADC_UNIT_1, ADC_CHANNEL_3, 100000, 100000, GPIO_NUM_12);
        adc_battery_monitor_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
            } else {
                power_save_timer_->SetEnabled(true);
            }
        });// lambda表达式：匿名函数
    }

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(160, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
        });
        power_save_timer_->SetEnabled(true);
    }



    void InitializeButtons() {
        // BOOT按钮点击事件处理
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            // 启动阶段特殊处理：按BOOT进入WiFi配置无需重启
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            // 正常阶段：切换聊天状态（除非已启用语音对话模式）
            if (!press_to_talk_tool_ || !press_to_talk_tool_->IsPressToTalkEnabled()) {
                app.ToggleChatState();
            }
        });
        // BOOT按钮按下事件处理
        boot_button_.OnPressDown([this]() {
            // 唤醒功耗节省定时器，保持屏幕常亮
            if (power_save_timer_) {
                power_save_timer_->WakeUp();
            }
            // 语音对话模式：按下BOOT开始录音
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StartListening();
            }
        });
        // BOOT按钮松开事件处理
        boot_button_.OnPressUp([this]() {
            // 语音对话模式：松开BOOT停止录音
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StopListening();
            }
        });
    }

    void InitializeTools() {
        // 创建并初始化语音对话工具（按键唤醒模式）
        press_to_talk_tool_ = new PressToTalkMcpTool();
        press_to_talk_tool_->Initialize();
    }

public:
    // 构造函数：初始化BOOT按钮（GPIO3，低电平触发，内部上拉）并依次初始化各子系统
    MyTest() : boot_button_(BOOT_BUTTON_GPIO, false, 0, 0, true) {  
        InitializePowerManager();       // 1. 启动电池监控和充电状态管理
        InitializePowerSaveTimer();     // 2. 配置功耗节省定时器
        display_ = new NoDisplay();     // 3. 无显示屏
        InitializeButtons();            // 4. 配置按钮事件处理
        InitializeTools();              // 5. 初始化语音对话工具
    }

    // 获取LED驱动实例，使用单色LED（GPIO2）
    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    // 获取显示屏驱动实例
    virtual Display* GetDisplay() override {
        return display_;
    }

    // 获取音频编码解码器实例（INMP441 仅麦克风输入，无功放输出）
    virtual AudioCodec* GetAudioCodec() override {
        static Inmp441AudioCodec audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DIN, I2S_STD_SLOT_LEFT);
        return &audio_codec;
    }

    // 获取当前电池状态：电量百分比、充电标志、放电标志
    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = adc_battery_monitor_->IsCharging();      // 查询是否正在充电
        discharging = adc_battery_monitor_->IsDischarging();// 查询是否正在放电
        level = adc_battery_monitor_->GetBatteryLevel();    // 获取当前电量百分比（0-100）
        return true;
    }

    // 设置功耗等级，低功耗模式外的等级会唤醒设备保持屏幕常亮
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();  // 非低功耗模式时唤醒定时器
        }
        WifiBoard::SetPowerSaveLevel(level);  // 调用父类方法处理其他逻辑
    }
};

// 宏展开：向系统注册 MyTest 为该硬件板的驱动类
DECLARE_BOARD(MyTest);
