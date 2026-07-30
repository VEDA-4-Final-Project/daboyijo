#ifndef VEDA_AUDIO_PLATER_HPP
#define VEDA_AUDIO_PLATER_HPP

#include <string>

extern "C" {
    #include <alsa/asoundlib.h>
}

class VedaAudioPlayer {
private:
    snd_pcm_t *pcm_handle;
    std::string device_name;

    int setHardwareParams(unsigned int rate, unsigned int channels, snd_pcm_format_t format);

public:
    VedaAudioPlayer(const std::string&dev_name = "plughw:2");

    ~VedaAudioPlayer();

    int initPlayer(unsigned int rate, unsigned int cahnnels, snd_pcm_format_t format);

    int playWav(const std::string& file_path);
};


#endif // VEDA_AUDIO_PLATER_HPP
