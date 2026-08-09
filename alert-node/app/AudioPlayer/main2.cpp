#include "VedaAudioPlayer.hpp"
#include <iostream>
#include <thread>
#include <chrono>

int main(int argc, char *argv[])
{
    if (argc < 2) {
        std::cout << "사용법 : " << argv[0] << " <WAV 파일 경로>" << std::endl;
        return -1;
    }

    std::string wav_path = argv[1];

    // WM8960 Custom Soundcard 디바이스 오픈
    VedaAudioPlayer player;

    std::cout << "[Main] 오디오 재생 테스트 시작..." << std::endl;

    int ret = player.playWav(wav_path);
    if (ret < 0) {
        std::cerr << "[Main] 재생 시작 실패: " << ret << std::endl;
        return -2;
    }

    std::cout << "\n==========================================" << std::endl;
    std::cout << " [명령어 가이드]" << std::endl;
    std::cout << "  p : 일시정지 (Pause)" << std::endl;
    std::cout << "  r : 재생 재개 (Resume)" << std::endl;
    std::cout << "  s : 재생 멈춤 (Stop)" << std::endl;
    std::cout << "  + : 볼륨 10% 올리기" << std::endl;
    std::cout << "  - : 볼륨 10% 내리기" << std::endl;
    std::cout << "  q : 프로그램 종료 (Quit)" << std::endl;
    std::cout << "==========================================\n" << std::endl;

    char command;
    bool running = true;

    while (running && std::cin >> command) {
        switch (command) {
            case 'p': case 'P':
                player.pause();
                break;

            case 'r': case 'R':
                player.resume();
                break;

            case 's': case 'S':
                player.stop();
                break;

            case '+': {
                long current_vol = player.getVolume();
                player.setVolume(current_vol + 10);
                break;
            }

            case '-': {
                long current_vol = player.getVolume();
                player.setVolume(current_vol - 10);
                break;
            }

            case 'q': case 'Q':
                player.stop();
                running = false;
                break;

            default:
                std::cout << "알 수 없는 명령어입니다." << std::endl;
                break;
        }
    }

    std::cout << "[Main] 프로그램 종료" << std::endl;
    return 0;
}
