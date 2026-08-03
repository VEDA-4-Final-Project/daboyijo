#include "VedaAudioPlayer.hpp"
#include <iostream>
#include <fstream>
#include <vector>

VedaAudioPlayer::VedaAudioPlayer(const std::string& dev_name, const std::string& card_name)
    : pcm_handle(nullptr), device_name(dev_name), is_playing(false), is_paused(false),
    card_name(card_name),mixer_handle(nullptr),volume_elem(nullptr){
        // 멤버 변수 초기화
        
    initMixer("Speaker");
}


VedaAudioPlayer::~VedaAudioPlayer() {
    stop();
    //소멸자
    is_playing = false; 

    
    if(play_thread.joinable()) {
        play_thread.join();
    }
    if(mixer_handle) {
        snd_mixer_close(mixer_handle);
        mixer_handle = nullptr;
    }

    if(pcm_handle != nullptr){

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

    snd_pcm_uframes_t period_size = 1024;
    snd_pcm_uframes_t buffer_size = 4096;

    snd_pcm_hw_params_set_period_size_near(pcm_handle, params, &period_size, 0);
    snd_pcm_hw_params_set_buffer_size_near(pcm_handle, params, &buffer_size);


   
    // 최종 전달 
    ret = snd_pcm_hw_params(pcm_handle, params);
    if(ret < 0){
        return ret;
    }

    // snd_pcm_pause Test
    int can_pause = snd_pcm_hw_params_can_pause(params);
    printf("-> WM8960 HW Pause Support: %s (%d)\n", can_pause ? "YES" : "NO", can_pause);

    return 0;
}

int VedaAudioPlayer::playWav(const std::string& file_path)
{
    is_playing = false;
    if(play_thread.joinable()) {
        play_thread.join();
    }

    is_playing = true;

    play_thread = std::thread(&VedaAudioPlayer::playbackWorker, this, file_path);

    return 0;
}




void VedaAudioPlayer::playbackWorker(const std::string& file_path)
{
    std::ifstream file(file_path, std::ios::binary);
    if(!file.is_open()) {
        std::cerr<< " wav 파일을 열수 없음 " << file_path << std::endl;
        is_playing = false;
        return ;
    }


    WavHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(WavHeader));

    if(std::string(header.riff, 4) != "RIFF" || std::string(header.wave, 4) != "WAVE") {
        std::cerr<< "올바른 WAV파일 형식이 아닙니다. " << std::endl;
        is_playing = false;
        return ;
    }

    snd_pcm_format_t alsa_format;
    if(header.bits_per_sample == 16) {
        alsa_format = SND_PCM_FORMAT_S16_LE; 
    } else{
        std::cerr<< " 지원하지 않는 비트폭입니다. (?)" << header.bits_per_sample << std::endl;
        is_playing = false;
        return ;
    }

    int ret = initPlayer(header.sample_rate, header.channels, alsa_format);
    if(ret<0) {
        is_playing = false;
        return ;
    }

    const size_t PERIOD_SIZE = 1024;
    size_t bytes_per_frame = (header.bits_per_sample/8) * header.channels;
    std::vector<char> buffer(PERIOD_SIZE * bytes_per_frame);

    std::cout << "[Playback] 비동기 재생시작 : " <<header.sample_rate << "Hz, " << header.channels <<"ch" << std::endl;

    //재생 루프 

    while(is_playing && (file.read(buffer.data(), buffer.size()) || file.gcount() > 0)) {

        //일시정지 확인 
        while(is_paused && is_playing) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        //일시정지 중에 멈춤 발생시 
        if(!is_playing) break;


        size_t read_bytes = file.gcount();
        snd_pcm_uframes_t frames_to_write = file.gcount() / bytes_per_frame;
        char* ptr = buffer.data();

        while(frames_to_write >0) {

            ret = snd_pcm_writei(pcm_handle, ptr, frames_to_write);

            if(ret == -EPIPE) {
                std::cerr << "[XRUN] 오디오 언더런 발생, 복구시도 "<< std::endl;
                snd_pcm_prepare(pcm_handle);
                continue;
            }else if( ret < 0 ) {
                int err = snd_pcm_recover(pcm_handle, ret, 0);
                if(err<0) {
                    std::cerr << "오디오 전송 실패 : " << snd_strerror(ret) << std::endl;
                    break;
                }
                continue;

            }else {
                frames_to_write -= ret;
                ptr += (ret * bytes_per_frame);
            }

        }
        if(ret<0) break;
    }
    std::cout << "[Playback] 재생종료 "<< std::endl;
    
    is_playing = false;
    return;
}




int VedaAudioPlayer::initMixer(const std::string& mix_ctrl_name) {
    int err;

    if((err = snd_mixer_open(&mixer_handle, 0)) <0) return err;
    if((err = snd_mixer_attach(mixer_handle, card_name.c_str())) <0) return err;
    if((err = snd_mixer_selem_register(mixer_handle,NULL,NULL)) <0) return err;
    if((err = snd_mixer_load(mixer_handle)) <0) return err;

    snd_mixer_selem_id_t *sid;
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid,0);
    snd_mixer_selem_id_set_name(sid, mix_ctrl_name.c_str());


    volume_elem = snd_mixer_find_selem(mixer_handle, sid);
    if(!volume_elem) {
        std::cerr << "[Mixer Warning] Mixer Control Element를 찾을수 없음 : " << mix_ctrl_name << std::endl;
    }
    return -1;

    std::cout << "[Mixer] Volume Control 연동 성공: " << mix_ctrl_name << std::endl;

    return 0;
}





int VedaAudioPlayer::pause() {
    if(pcm_handle && is_playing && !is_paused) {
        int err = snd_pcm_pause(pcm_handle, 1);
        if(err == 0) {
            is_paused = true;
            std::cout << "[Playback] 일시정지" << std::endl;
        }
        return err;
    }
    return 0;
}

int VedaAudioPlayer::resume() {
    if(pcm_handle && is_paused) {
        int err = snd_pcm_pause(pcm_handle, 0);
        if(err == 0) {
            is_paused = false;
            std::cout << "[Playback] 재생 재게" << std::endl;
        }
        return err;
    }
    return 0;
}

int VedaAudioPlayer::stop() {
    if(!is_playing){
        return 0;
    }

    is_playing = false;
    is_paused = false;

    if(pcm_handle) {
        snd_pcm_drop(pcm_handle);
        snd_pcm_prepare(pcm_handle);
    }

    if(play_thread.joinable()){
        play_thread.join();
    }

    std::cout << "[Playback] 재생 정지 완료" << std::endl;
    return 0;
}


int VedaAudioPlayer::setVolume(long volume_percent) {
    if(!volume_elem) return -1;

    if(volume_percent<0) volume_percent = 0;
    if(volume_percent>100) volume_percent = 100;

    long min_vol, max_vol;
    snd_mixer_selem_get_playback_volume_range(volume_elem, &min_vol,&max_vol);

    long target_vol = min_vol+ (max_vol - min_vol) * volume_percent / 100;

    int err = snd_mixer_selem_set_playback_volume_all(volume_elem, target_vol);
    if(err == 0) {
        std::cout << "[Volume] 설정된 볼륨 : " << volume_percent << "% (HW Raw: "<< target_vol << ")" << std::endl;
    }

    return err;
}


long VedaAudioPlayer::getVolume() {

    if(!volume_elem) return -1;

    long min_vol, max_vol, raw_vol;
    snd_mixer_selem_get_playback_volume_range(volume_elem,&min_vol, &max_vol);
    snd_mixer_selem_get_playback_volume(volume_elem, SND_MIXER_SCHN_FRONT_LEFT, &raw_vol);

    if(max_vol == min_vol) return 0;

    return (raw_vol - min_vol) * 100 / (max_vol - min_vol);
}

