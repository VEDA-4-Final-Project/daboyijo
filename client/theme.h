#ifndef THEME_H
#define THEME_H

#include <QString>

// 앱 전역 디자인 토큰 (라이트/다크 두 팔레트, 런타임 전환).
//
// 원래 mainwindow.cpp 내부에 있었으나 로그인 화면에서도 같은 색을 써야 해서
// 헤더로 분리했다. 값을 바꾸려면 여기 한 곳만 고치면 전 화면에 반영된다.
// (C++17 inline 변수 — 여러 .cpp에서 include해도 실체는 하나)

struct Palette {
    const char *bgDeep, *panel, *card, *border,
               *text, *sub, *accent, *normal, *warn, *critical;
};

// 밝은 의료 톤 (요양원 주간 관제 환경)
inline const Palette kLight {
    "#F4F7FA", "#FFFFFF", "#F0F4F8", "#DCE4EC",
    "#1E2A32", "#5C6B78", "#12B5A6", "#2E9E5B", "#C77A11", "#E5484D"
};
// 다크 관제실 톤 (야간·통합 관제 환경, 강조색은 어두운 배경용으로 살짝 밝게)
inline const Palette kDark {
    "#0E141B", "#151D26", "#1C2733", "#2A3742",
    "#E6EDF3", "#8B98A5", "#17C7B6", "#35B368", "#E0A030", "#FF5A5F"
};

// 현재 적용 중인 색 (전환 시 applyPalette로 재대입)
inline const char* kBgDeep   = kLight.bgDeep;
inline const char* kPanel    = kLight.panel;
inline const char* kCard     = kLight.card;
inline const char* kBorder   = kLight.border;
inline const char* kTextMain = kLight.text;
inline const char* kTextSub  = kLight.sub;
inline const char* kAccent   = kLight.accent;
inline const char* kNormal   = kLight.normal;
inline const char* kWarn     = kLight.warn;
inline const char* kCritical = kLight.critical;

inline void applyPalette(const Palette& p) {
    kBgDeep = p.bgDeep;   kPanel = p.panel;   kCard = p.card;   kBorder = p.border;
    kTextMain = p.text;   kTextSub = p.sub;   kAccent = p.accent;
    kNormal = p.normal;   kWarn = p.warn;     kCritical = p.critical;
}

#endif  // THEME_H
