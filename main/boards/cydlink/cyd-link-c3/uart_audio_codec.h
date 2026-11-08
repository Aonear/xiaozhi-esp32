#ifndef UART_AUDIO_CODEC_H
#define UART_AUDIO_CODEC_H

#include "audio_codec.h"

#include <driver/gpio.h>
#include <mutex>

// Mic INMP441 (I2S) sebagai input. Output (suara AI) dikirim lewat UART ke CYD.
class UartAudioCodec : public AudioCodec {
public:
    UartAudioCodec(int input_sample_rate, int output_sample_rate,
                   gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din);
    virtual ~UartAudioCodec();

    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;

protected:
    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

private:
    std::mutex mutex_;
};

#endif
