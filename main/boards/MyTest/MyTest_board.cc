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
#include <driver/uart.h>

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
                power_save_timer_->SetEnabled(false);//关闭省电模式，保持常亮以便语音唤醒
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
        power_save_timer_->SetEnabled(false);//关闭省电模式，保持常亮以便语音唤醒
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

        // 手动初始化 UART0 (TX=GPIO21, RX=GPIO20) 用于开关灯串口通知
        const uart_config_t uart_config = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        // UART0 可能已被控制台安装过驱动，先尝试卸载
        // uart_driver_delete(UART_NUM_0);// 直接修改menuconfig，Component config->ESP System Settings->Channel for console output
        uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
        uart_param_config(UART_NUM_0, &uart_config);
        uart_set_pin(UART_NUM_0, GPIO_NUM_21, GPIO_NUM_20, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

        const char* msg = "LIGHT_INIT\r\n";
        uart_write_bytes(UART_NUM_0, msg, strlen(msg));

        // 注册开关灯工具，触发时通过串口通知
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("user.control_light",
            "ontrol the smart light. MUST use this tool to turn the light on or off. The 'action' parameter MUST be exactly 'on' or 'off'.",
            PropertyList({
                Property("action", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto action = properties["action"].value<std::string>();
                if (action == "on" || action == "turn_on" || action == "open") {
                    ESP_LOGI(TAG, "开灯，发送串口通知");
                    uart_write_bytes(UART_NUM_0, "@LED2_ON\r\n", strlen("@LED2_ON\r\n"));
                    vTaskDelay(pdMS_TO_TICKS(20));
                    uart_write_bytes(UART_NUM_0, "@LED1_ON\r\n", strlen("@LED1_ON\r\n"));
                    vTaskDelay(pdMS_TO_TICKS(20));
                    uart_write_bytes(UART_NUM_0, "@LED1_UP\r\n", strlen("@LED1_UP\r\n"));
                    vTaskDelay(pdMS_TO_TICKS(20));
                    uart_write_bytes(UART_NUM_0, "@LED2_UP\r\n", strlen("@LED2_UP\r\n"));
                } else if (action == "off" || action == "turn_off" || action == "close") {
                    ESP_LOGI(TAG, "关灯，发送串口通知");
                    uart_write_bytes(UART_NUM_0, "@LED2_OFF\r\n", strlen("@LED2_OFF\r\n"));
                    vTaskDelay(pdMS_TO_TICKS(20));
                    uart_write_bytes(UART_NUM_0, "@LED1_OFF\r\n", strlen("@LED1_OFF\r\n"));
                }
                return true;
            });
    }

public:
    // 构造函数：初始化BOOT按钮（GPIO3，低电平触发，内部上拉）并依次初始化各子系统
    MyTest() : boot_button_(BOOT_BUTTON_GPIO, false, 0, 0, true) {  
        // InitializePowerManager();       // 1. 启动电池监控和充电状态管理（GPIO12 让给 UART）
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
        if (!adc_battery_monitor_) {
            return false;
        }
        charging = adc_battery_monitor_->IsCharging();
        discharging = adc_battery_monitor_->IsDischarging();
        level = adc_battery_monitor_->GetBatteryLevel();
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
