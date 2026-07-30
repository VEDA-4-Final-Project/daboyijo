#include "VedaAudioPlayer.hpp"
#include <iostream>

int main(int argc, char *argv[])
{
    if(argc < 2){
        std::cout << " 사용법 : " << argv[0] << " <WAV 파일 경로" << std::endl;
        return -1;
    }

    std::string wav_path = argv[1];

    VedaAudioPlayer player("plughw:2");

    std::cout << "[Main] 오디오 재생 테스트를 시작... " << std::endl;

    int ret = player.playWav(wav_path);
    if (ret < 0 ) {
        std::cerr << "[Main] 재생중 에러 발생: " << ret << std::endl;
        return -2;
    }

    std::cout << "[Main] 재생 완료" << std::endl;
    return 0;

}

