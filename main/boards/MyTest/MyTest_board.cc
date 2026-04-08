#include "wifi_board.h"
#include "codecs/inmp441_audio_codec.h"
#include "config.h"
#ifdef USE_TFT_DISPLAY
#include "display/lcd_display.h"
#else
#include "display/oled_display.h"
#endif
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
#ifndef USE_TFT_DISPLAY
#include <driver/i2c_master.h>
#endif
#ifdef USE_TFT_DISPLAY
#include <driver/spi_master.h>
#endif
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#define TAG "MyTest"

class MyTest : public WifiBoard {
private:
#ifndef USE_TFT_DISPLAY
    i2c_master_bus_handle_t codec_i2c_bus_;
#endif
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

#ifndef USE_TFT_DISPLAY
    void InitializeI2c() {
        // I2C总线配置结构体，仅用于SSD1306 OLED屏幕
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }
#endif

#ifdef USE_TFT_DISPLAY
    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = LCD_MOSI_PIN;
        buscfg.miso_io_num = LCD_MISO_PIN;
        buscfg.sclk_io_num = LCD_SCLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeSt7789Display() {
        // SPI接口配置
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = LCD_CS_PIN;
        io_config.dc_gpio_num = LCD_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;          // SPI时钟40MHz
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install ST7789V2 driver");
        // LCD面板驱动配置
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = LCD_RST_PIN;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;               // RGB565
        panel_config.data_endian = LCD_RGB_DATA_ENDIAN_BIG;

        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));

        // 重置并初始化显示屏
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }

        // ST7789需要反转颜色
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, true));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        // 配置背光引脚
        gpio_config_t bl_gpio_config = {
            .pin_bit_mask = 1ULL << LCD_BL_PIN,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&bl_gpio_config));
        gpio_set_level(LCD_BL_PIN, 1);                  // 打开背光

        // 创建SPI LCD显示抽象层实例
        display_ = new SpiLcdDisplay(panel_io_, panel_,
            DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }
#else
    void InitializeSsd1306Display() {
        // SSD1306 OLED屏幕的I2C通信配置
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(codec_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
        ESP_LOGI(TAG, "SSD1306 driver installed");

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }

        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }
#endif

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
#ifdef USE_TFT_DISPLAY
        InitializeSpi();                // 3. 初始化SPI总线
        InitializeSt7789Display();      // 4. 初始化ST7789V2 LCD屏幕驱动
#else
        InitializeI2c();                // 3. 初始化I2C总线（仅用于SSD1306）
        InitializeSsd1306Display();     // 4. 初始化SSD1306 OLED屏幕驱动
#endif
        InitializeButtons();            // 6. 配置按钮事件处理
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
