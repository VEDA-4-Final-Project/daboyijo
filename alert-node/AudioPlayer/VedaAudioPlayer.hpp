#ifndef VEDA_AUDIO_PLATER_HPP
#define VEDA_AUDIO_PLATER_HPP

#include <string>
#include <thread>
#include <atomic>

extern "C" {
    #include <alsa/asoundlib.h>
}

struct WavHeader {
    char riff[4];               // riff
    int32_t overall_size;       // 파일 전체 크기
    char wave[4];               // wave
    char fmt_chunk_marker[4];   // fmt 
    int32_t length_of_fmt;      // fmt 청크의 크기 
    int16_t format_type;        // 데이터 형태 (1이면 pcm)
    int16_t channels;           // 채널수 
    int32_t sample_rate;        // 샘플 레이트 
    int32_t byterate;           // 초당 바이트 수
    int16_t block_align;        // 데이터 블록 크기?
    int16_t bits_per_sample;    // 비트 폭 
    char data_chunk_header[4];  // data
    int32_t data_size;          // 순수 오디오 데이터 크기 
};


class VedaAudioPlayer {
private:
    snd_pcm_t *pcm_handle;
    std::string device_name;

    snd_mixer_t *volume_elem;
    std::string card_name;

    int initMixer(const std::string& mix_crtl_name = "Speaker");

    //백그라운드 재생 전용 스레드 객체 
    std::thread play_thread;
    //재생중 확인 플래그 
    std::atomic<bool> is_playing;
    std::atomic<bool> is_paused;

    int setHardwareParams(unsigned int rate, unsigned int channels, snd_pcm_format_t format);

    // 실제 파일 읽기와 alsa쓰기를 담당할 백그라운드 함수 
    void playbackWorker(const std::string& file_path);

public:
    VedaAudioPlayer(const std::string&dev_name = "plughw:CARD=WM8960CustomSou,DEV=0", const std::string& card_name = "hw:CARD=WM8960CustomSou");
    ~VedaAudioPlayer();

    int initPlayer(unsigned int rate, unsigned int cahnnels, snd_pcm_format_t format);

    // 비동기 재생 시작함수 (호출시 스레드를 뛰우고 즉식 리턴) 
    int playWav(const std::string& file_path);

    // 재생 제어 함수 
    int pause();
    int resume();
    int stop();
    // 볼륨 제어 함수 
    int setVolume(long volume_percent);
    long getVolume();
};


#endif // VEDA_AUDIO_PLATER_HPP
