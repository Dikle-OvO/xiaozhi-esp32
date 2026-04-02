#ifndef _INMP441_AUDIO_CODEC_H
#define _INMP441_AUDIO_CODEC_H

#include "audio_codec.h"

#include <mutex>

class Inmp441AudioCodec : public AudioCodec {
private:
    std::mutex data_if_mutex_;

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

public:
    Inmp441AudioCodec(int input_sample_rate, int output_sample_rate,
        gpio_num_t bclk, gpio_num_t ws, gpio_num_t din, i2s_std_slot_mask_t mic_slot_mask = I2S_STD_SLOT_LEFT);
    virtual ~Inmp441AudioCodec();

    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
};

#endif // _INMP441_AUDIO_CODEC_H