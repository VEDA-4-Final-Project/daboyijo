#ifndef THEME_H
#define THEME_H

#include <QString>

// 앱 전역 디자인 토큰 (라이트/다크 두 팔레트, 런타임 전환).
//
// 원래 mainwindow.cpp 내부에 있었으나 로그인 화면에서도 같은 색을 써야 해서
// 헤더로 분리했다. 값을 바꾸려면 여기 한 곳만 고치면 전 화면에 반영된다.
// (C++17 inline 변수 — 여러 .cpp에서 include해도 실체는 하나)

// 필드 선언 순서와 kLight/kDark 리터럴의 값 순서는 반드시 일치해야 한다.
// C++17 위치 기반 집합 초기화라, 순서가 어긋나도 컴파일은 통과하고
// 색만 조용히 뒤바뀐다. 필드를 추가/재배치할 때는 구조체·kLight·kDark·
// applyPalette() 네 블록을 같은 편집에서 함께 고칠 것.
struct Palette {
    const char *bgDeep, *panel, *card, *border,
               *text, *sub, *accent, *onAccent,
               *normal, *warn, *high, *critical, *info;
};

// 밝은 의료 톤 (요양원 주간 관제 환경)
// normal/warn/critical 세 값은 AA 재계산에 따라 기존 값에서 교체됐다
// (#2E9E5B→#257A4A, #C77A11→#8A6400, #E5484D→#C23934).
inline const Palette kLight {
    "#F4F7FA", "#FFFFFF", "#F0F4F8", "#DCE4EC",
    "#1E2A32", "#5C6B78", "#12B5A6", "#1E2A32",
    "#257A4A", "#8A6400", "#A05000", "#C23934", "#1D5FA8"
};
// 다크 관제실 톤 (상용 VMS 기준으로 재조정, v2)
//
// v1은 파랑기 도는 중간 회색이었다(#0E141B/#151D26/#1C2733). 상용 VMS
// (한화 웹뷰어·Milestone·Genetec)는 그보다 훨씬 어둡고 채도가 낮다 —
// 영상이 화면의 주역이므로 UI 크롬이 영상보다 밝으면 눈이 크롬으로 간다.
// 그래서 배경 3단을 거의 무채색 검정 쪽으로 내리고, 남는 채도는 전부
// 경보 색에만 준다. 경보 5색(normal~info)은 어두운 배경 위 가독성이
// 이미 검증된 값이라 손대지 않는다.
inline const Palette kDark {
    "#07090C", "#0D1116", "#131920", "#212A33",
    "#DDE4EA", "#78858F", "#17C7B6", "#07090C",
    "#35B368", "#E0A030", "#FF9F45", "#FF5A5F", "#4DA3FF"
};

// onAccent — accent 채우기 위에 얹는 전경색 전용 토큰(UI-SPEC 정정 1).
// %(text)를 accent 위에 얹으면 다크에서 1.80:1로 AA 미달(D-08 위반)이 되므로
// 별도 토큰을 둔다. 라이트 #1E2A32 on #12B5A6 = 5.72:1, 다크 #0E141B on
// #17C7B6 = 8.71:1 — 두 팔레트 모두 AA(4.5:1)를 통과하는 유일한 방식.

// 현재 적용 중인 색 (전환 시 applyPalette로 재대입)
inline const char* kBgDeep   = kLight.bgDeep;
inline const char* kPanel    = kLight.panel;
inline const char* kCard     = kLight.card;
inline const char* kBorder   = kLight.border;
inline const char* kTextMain = kLight.text;
inline const char* kTextSub  = kLight.sub;
inline const char* kAccent   = kLight.accent;
inline const char* kOnAccent = kLight.onAccent;
inline const char* kNormal   = kLight.normal;
inline const char* kWarn     = kLight.warn;
inline const char* kHigh     = kLight.high;
inline const char* kCritical = kLight.critical;
inline const char* kInfo     = kLight.info;

// 영상 캔버스 전용 고정색 — 테마 토글과 무관하게 항상 어둡게 유지한다(D-07/IA-02).
// kVideoSurface를 Palette 구조체 "밖"에 독립 상수로 둔 이유: 구조체 안에 넣으면
// applyPalette()가 팔레트 전환 시 자동으로 값을 갈아치워, 라이트 테마에서
// 영상 캔버스까지 밝아져 IA-02가 깨진다. applyPalette() 본문에 kVideoSurface를
// 대입하는 줄을 절대 만들지 말 것.
inline const char* kVideoSurface = "#000000";

inline void applyPalette(const Palette& p) {
    kBgDeep = p.bgDeep;   kPanel = p.panel;   kCard = p.card;   kBorder = p.border;
    kTextMain = p.text;   kTextSub = p.sub;   kAccent = p.accent; kOnAccent = p.onAccent;
    kNormal = p.normal;   kWarn = p.warn;     kHigh = p.high;
    kCritical = p.critical; kInfo = p.info;
}

#endif  // THEME_H
