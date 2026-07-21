#include "VedaAudioPlayer.hpp"
#include <iostream>
#include <fstream>
#include <vector>

VedaAudioPlayer::VedaAudioPlayer(const std::string& dev_name)
    : pcm_handle(nullptr), device_name(dev_name) {
        // 멤버 변수 초기화
}


VedaAudioPlayer::~VedaAudioPlayer() {
    //소멸자
    if(pcm_handle != nullptr){
        snd_pcm_drain(pcm_handle);

        snd_pcm_close(pcm_handle);

        pcm_handle = nullptr;
        std::cout<< "[VedaAudioPlayer] 장치가 안전하게 닫혔습니다.\n" << std::endl;
    }
}

int VedaAudioPlayer::initPlayer(unsigned int rate, unsigned int channels, snd_pcm_format_t format)
{
    int ret;
    //ALSA PCM 장치 열기 

    ret = snd_pcm_open(&pcm_handle, device_name.c_str(), SND_PCM_STREAM_PLAYBACK, 0);

    if(ret<0) {
        std::cerr<< "PCM 장치 오픈 실패:"<< snd_strerror(ret) << std::endl;
        return ret;
    }

    ret = setHardwareParams(rate,channels, format);
    if(ret<0) {
        snd_pcm_close(pcm_handle);
        pcm_handle = nullptr;
        return ret;
    }

    std::cout << "[VedaAudioPlayer] 장치오픈 완료 " << std::endl;
    return 0;
}

int VedaAudioPlayer::setHardwareParams(unsigned int rate, unsigned int channels, snd_pcm_format_t format){
    int ret;
    snd_pcm_hw_params_t *params = nullptr;
    //1. 메모리 준비
    snd_pcm_hw_params_alloca(&params);

    //2. 사운드카드 기본사양 범위 
    ret = snd_pcm_hw_params_any(pcm_handle, params);
    if(ret < 0){
        return ret;
    }

    //3.엑세스 모드 설정 
    ret = snd_pcm_hw_params_set_access(pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if(ret < 0){
        return ret;
    }

    //4. 데이터 포맷 설정 
    ret = snd_pcm_hw_params_set_format(pcm_handle, params, format);
    if(ret < 0){
        return ret;
    }

    //5. 채널 수 설정
    ret = snd_pcm_hw_params_set_channels(pcm_handle, params, channels);
    if(ret < 0){
        return ret;
    }

    //6. 샘플레이트 설정 
    ret = snd_pcm_hw_params_set_rate_near(pcm_handle, params, &rate, nullptr);
    if(ret < 0){
        return ret;
    }
    // 최종 전달 
    ret = snd_pcm_hw_params(pcm_handle, params);
    if(ret < 0){
        return ret;
    }


    return 0;
}

struct WavHeader {
    char riff[4];               // riff??
    int32_t overall_size;       // 파일 전체 크기
    char wave[4];               // wave?
    char fmt_chunk_marker[4];   // fmt ??
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


int VedaAudioPlayer::playWav(const std::string& file_path)
{
    std::ifstream file(file_path, std::ios::binary);
    if(!file.is_open()) {
        std::cerr<< " wav 파일을 열수 없음 " << file_path << std::endl;
        return -1;
    }


    WavHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(WavHeader));

    if(std::string(header.riff, 4) != "RIFF" || std::string(header.wave, 4) != "WAVE") {
        std::cerr<< "올바른 WAV파일 형식이 아닙니다. " << std::endl;
        return -2;
    }

    snd_pcm_format_t alsa_format;
    if(header.bits_per_sample == 16) {
        alsa_format = SND_PCM_FORMAT_S16_LE; 
    } else{
        std::cerr<< " 지원하지 않는 비트폭입니다. (?)" << header.bits_per_sample << std::endl;
        return -3;
    }

    int ret = initPlayer(header.sample_rate, header.channels, alsa_format);
    if(ret<0) {
        return ret;
    }

    const size_t PERIOD_SIZE = 1024;
    size_t bytes_per_frame = (header.bits_per_sample/8) * header.channels;
    std::vector<char> buffer(PERIOD_SIZE * bytes_per_frame);

    std::cout << "[Playback] 재생시작 : " <<header.sample_rate << "Hz, " << header.channels <<"ch" << std::endl;

    //재생 루프 

    while(file.read(buffer.data(), buffer.size()) || file.gcount() > 0) {
        snd_pcm_uframes_t frames = file.gcount() / bytes_per_frame;

        ret = snd_pcm_writei(pcm_handle, buffer.data(), frames);

        if(ret == -EPIPE) {
            std::cerr << "[XRUN] 오디오 언더런 발생, 복구시도 "<< std::endl;
            snd_pcm_prepare(pcm_handle);
        }else if( ret < 0 ) {
            std::cerr << "오디오 전송 실패 : " << snd_strerror(ret) << std::endl;
            break;
        }else if( ret < frames) {

        }

    }
    return 0;
}

