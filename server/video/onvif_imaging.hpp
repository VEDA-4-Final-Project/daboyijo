#pragma once

#include <string>

// ONVIF Imaging 서비스로 카메라 화질(밝기/대비/채도)을 원격 조절한다.
//
// Qt 관제 → StreamServer IMAGE_SET 제어메시지 → 이 모듈 → 카메라(ONVIF SOAP).
// 동작은 server/tools/onvif_imaging_test.py 로 실증한 흐름을 그대로 C++로 옮긴 것:
//   ⓪ GetSystemDateAndTime  (무인증) — 카메라 시각으로 WS-Security 서명(시계 오차 회피)
//   ① GetCapabilities       — Media/Imaging 서비스 주소(XAddr) 탐색
//   ② GetVideoSources       — VideoSourceToken 획득
//   ③ GetOptions            — 항목별 실제 범위(Min~Max) 확인
//   ④ 0~100 정규화 → 카메라 범위로 환산
//   ⑤ SetImagingSettings    — 적용
//
// 인증은 WS-Security UsernameToken(digest) — SHA1/Base64는 OpenSSL(libcrypto) 사용.
// HTTP 전송은 libcurl. 카메라 IP·계정은 RTSP URL에서 파싱한다
// (Qt가 CAMERA_SET으로 보내는 rtsp://user:pw@ip:554/... 형식).

// 슬라이더 값(0~100). 음수(-1)는 "이 항목은 건드리지 않음".
// (노출은 ONVIF에서 수동모드 전환이 필요해 야간감지에 위험 → 대상에서 제외)
struct OnvifImageParams {
    int brightness = -1;
    int contrast = -1;
    int saturation = -1;
};

// rtsp_url에서 host/계정을 파싱해 ONVIF Imaging으로 p를 적용한다.
// 성공하면 true. 실패 시 false를 반환하고 err(널 아님)에 사유를 채운다.
// 블로킹 HTTP 호출들을 수행하므로 수신 스레드가 아닌 별도 스레드에서 부를 것.
bool applyOnvifImaging(const std::string& rtsp_url, const OnvifImageParams& p,
                       std::string* err);
