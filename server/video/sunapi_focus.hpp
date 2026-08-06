#pragma once

#include <string>

// 한화 SUNAPI SimpleFocus로 카메라 초점을 맞춘다.
//
// Qt 관제 → StreamServer FOCUS_SET 제어메시지 → 이 모듈 → 카메라(SUNAPI HTTP).
// 웹뷰어(172.20.32.6/wmf .../simpleFocus)가 실제로 쏘는 요청을 그대로 재현:
//   GET /stw-cgi/image.cgi?msubmenu=focus&action=control&Channel=<ch>&Mode=SimpleFocus
//       [&FocusAreaCoordinate=x1,y1,x2,y2] &SunapiSeqId=<n>
//
// 인증은 HTTP Digest(MD5) — libcurl이 자동 처리(CURLAUTH_DIGEST). ONVIF의
// WS-Security와 달리 서명을 손으로 만들 필요가 없다. 카메라 IP·계정은 RTSP URL에서
// 파싱한다(Qt가 CAMERA_SET으로 보내는 rtsp://user:pw@ip:554/... 형식).

// rtsp_url에서 host/계정을 파싱해 SimpleFocus를 적용한다.
//  - channel : SUNAPI Channel 번호(0 = CH1)
//  - area    : false=전체 자동초점, true=클릭 지점 영역 초점
//  - nx, ny  : area일 때 클릭 중심 정규화 좌표(0~1). 전체 초점이면 무시.
// 성공 시 true. 실패 시 false + err(널 아님)에 사유. 블로킹 HTTP라 별도 스레드에서 부를 것.
bool sunapiFocus(const std::string& rtsp_url, int channel, bool area,
                 float nx, float ny, std::string* err);
