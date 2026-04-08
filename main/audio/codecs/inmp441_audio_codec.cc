#include "inmp441_audio_codec.h"

#include <esp_log.h>

#include <vector>

#define TAG "Inmp441AudioCodec"

Inmp441AudioCodec::Inmp441AudioCodec(int input_sample_rate, int output_sample_rate,
    gpio_num_t bclk, gpio_num_t ws, gpio_num_t din, i2s_std_slot_mask_t mic_slot_mask) {
    duplex_ = false;
    input_reference_ = false;
    input_channels_ = 1;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = static_cast<uint32_t>(input_sample_rate_),
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = mic_slot_mask,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
#ifdef I2S_HW_VERSION_2
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
#endif
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk,
            .ws = ws,
            .dout = I2S_GPIO_UNUSED,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            }
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "INMP441 RX channel created");

    // 检测 INMP441 是否实际连接：读取数据检查是否非零且有波动
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    const int check_samples = 256;
    std::vector<int32_t> check_buf(check_samples, 0);
    size_t bytes_read = 0;
    bool mic_detected = false;
    for (int retry = 0; retry < 3 && !mic_detected; retry++) {
        i2s_channel_read(rx_handle_, check_buf.data(), check_samples * sizeof(int32_t), &bytes_read, pdMS_TO_TICKS(200));
        int total = bytes_read / sizeof(int32_t);
        if (total == 0) continue;
        int nonzero_count = 0;
        int32_t min_val = check_buf[0], max_val = check_buf[0];
        for (int i = 0; i < total; i++) {
            if (check_buf[i] != 0) nonzero_count++;
            if (check_buf[i] < min_val) min_val = check_buf[i];
            if (check_buf[i] > max_val) max_val = check_buf[i];
        }
        // 麦克风正常工作：大部分数据非零，且有一定波动（不是恒定值）
        if (nonzero_count > total / 2 && max_val != min_val) {
            mic_detected = true;
        }
        ESP_LOGI(TAG, "Retry %d: total=%d, nonzero=%d, min=0x%08lX, max=0x%08lX",
                 retry, total, nonzero_count, (unsigned long)min_val, (unsigned long)max_val);
    }
    ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
    if (mic_detected) {
        ESP_LOGI(TAG, "INMP441 microphone detected");
    } else {
        ESP_LOGW(TAG, "INMP441 microphone NOT detected, check wiring!");
    }
}

Inmp441AudioCodec::~Inmp441AudioCodec() {
    if (rx_handle_ != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_disable(rx_handle_));
    }
}

void Inmp441AudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
    }
    AudioCodec::EnableInput(enable);
}

void Inmp441AudioCodec::EnableOutput(bool enable) {
    AudioCodec::EnableOutput(enable);
}

int Inmp441AudioCodec::Read(int16_t* dest, int samples) {
    size_t bytes_read = 0;
    constexpr TickType_t kReadTimeoutTicks = pdMS_TO_TICKS(200);

    std::vector<int32_t> bit32_buffer(samples);
    if (i2s_channel_read(rx_handle_, bit32_buffer.data(), samples * sizeof(int32_t), &bytes_read, kReadTimeoutTicks) != ESP_OK) {
        return 0;
    }

    samples = bytes_read / sizeof(int32_t);
    for (int i = 0; i < samples; ++i) {
        int32_t value = bit32_buffer[i] >> 12;
        dest[i] = (value > INT16_MAX) ? INT16_MAX : (value < -INT16_MAX) ? -INT16_MAX : static_cast<int16_t>(value);
    }
    return samples;
}

int Inmp441AudioCodec::Write(const int16_t* data, int samples) {
    (void)data;
    return samples;
}