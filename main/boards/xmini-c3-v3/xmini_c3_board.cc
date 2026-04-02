#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/oled_display.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "settings.h"
#include "config.h"
#include "power_save_timer.h"
#include "adc_battery_monitor.h"
#include "press_to_talk_mcp_tool.h"

#include <wifi_manager.h>
#include <esp_log.h>
#include <esp_efuse_table.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#define TAG "XminiC3Board"

class XminiC3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
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

    void InitializeCodecI2c() {
        // I2C总线配置结构体，定义硬件参数
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,                      // 使用I2C端口0
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,      // SDA数据线GPIO编号
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,      // SCL时钟线GPIO编号
            .clk_source = I2C_CLK_SRC_DEFAULT,          // 使用默认时钟源
            .glitch_ignore_cnt = 7,                     // 毛刺忽略计数（抗干扰）
            .intr_priority = 0,                         // 中断优先级
            .trans_queue_depth = 0,                     // 传输队列深度（0=自动）
            .flags = {
                .enable_internal_pullup = 1,            // 启用内部上拉电阻
            },
        };
        // 创建并初始化I2C总线
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));

        // 扫描I2C总线上地址0x18的设备（ES8311音频编码器），超时1000ms
        if (i2c_master_probe(codec_i2c_bus_, 0x18, 1000) != ESP_OK) {
            // 如果探测失败，说明硬件连接有问题，循环输出错误日志
            while (true) {
                ESP_LOGE(TAG, "Failed to probe I2C bus, please check if you have installed the correct firmware");
                vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
        }
    }

    void InitializeSsd1306Display() {
        // SSD1306 OLED屏幕的I2C通信配置
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,                           // SSD1306的I2C地址
            .on_color_trans_done = nullptr,             // 无颜色传输完成回调
            .user_ctx = nullptr,                        // 用户上下文指针
            .control_phase_bytes = 1,                   // 控制字节数
            .dc_bit_offset = 6,                         // 数据/命令标志位偏移
            .lcd_cmd_bits = 8,                          // 命令位宽度
            .lcd_param_bits = 8,                        // 参数位宽度
            .flags = {
                .dc_low_on_data = 0,                    // DC低电平表示数据
                .disable_control_phase = 0,             // 不禁用控制阶段
            },
            .scl_speed_hz = 400 * 1000,                 // I2C时钟频率400kHz
        };

        // 为SSD1306创建I2C接口
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(codec_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        // LCD面板驱动配置
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;              // 无重置GPIO（硬件不支持或已外部处理）
        panel_config.bits_per_pixel = 1;               // 单色显示（1比特/像素）

        // SSD1306特定配置
        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),  // 屏幕高度（通常64像素）
        };
        panel_config.vendor_config = &ssd1306_config;

        // 创建SSD1306驱动实例
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // 重置显示屏
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        // 初始化显示屏硬件，失败时使用虚拟显示代替
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();  // 创建虚拟显示对象，确保程序继续运行
            return;
        }

        // 启动显示屏
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        // 创建OLED显示抽象层实例，配置镜像模式
        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
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
    XminiC3Board() : boot_button_(BOOT_BUTTON_GPIO, false, 0, 0, true) {  
        InitializePowerManager();       // 1. 启动电池监控和充电状态管理
        InitializePowerSaveTimer();     // 2. 配置功耗节省定时器
        InitializeCodecI2c();           // 3. 初始化I2C总线与音频编码器通信
        InitializeSsd1306Display();     // 4. 初始化OLED屏幕驱动
        InitializeButtons();            // 5. 配置按钮事件处理
        InitializeTools();              // 6. 初始化语音对话工具
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

    // 获取音频编码解码器实例，配置采样率、GPIO映射和编码器地址
    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);  // 注：PA_PIN为功放引脚，ES8311_ADDR为I2C芯片地址(0x18)
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

// 宏展开：向系统注册 XminiC3Board 为该硬件板的驱动类
DECLARE_BOARD(XminiC3Board);
