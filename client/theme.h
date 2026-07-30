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

// 로그인·회원가입 창 공용 스타일시트.
// 앱 전체를 어두운 톤으로 통일하므로 이 두 창도 다크 팔레트로 고정한다.
// (관제 화면은 MainWindow::applyTheme가 현재 팔레트로 따로 만든다)
inline QString authDialogStyleSheet()
{
    return QString(R"(
        QDialog { background: %(bgDeep); }
        #authCard { background: %(panel); }
        QLabel { color: %(text); font-family: "Segoe UI", "맑은 고딕", sans-serif; }

        #authLogo { color: %(accent); font-size: 34px; font-weight: 800; letter-spacing: 3px; }
        #authTitle { color: %(text); font-size: 20px; font-weight: 800; }
        #authSubtitle { color: %(sub); font-size: 13px; letter-spacing: 0.5px; }
        #authCaption { color: %(sub); font-size: 12px; font-weight: 700; }

        #authEdit { background: %(bgDeep); color: %(text); border: 1px solid %(border);
                    border-radius: 8px; padding: 10px 12px; font-size: 14px; }
        #authEdit:focus { border: 1px solid %(accent); }
        /* 검증 실패한 칸은 테두리를 빨갛게 — 어디가 틀렸는지 바로 보이게 */
        #authEdit[invalid="true"] { border: 1px solid %(critical); }

        #authCheck { color: %(sub); font-size: 12px; }
        #authCheck::indicator { width: 14px; height: 14px; border: 1px solid %(border);
                                border-radius: 3px; background: %(bgDeep); }
        #authCheck::indicator:checked { background: %(accent); border-color: %(accent); }

        #authHint { color: %(sub); font-size: 11px; }
        #authHint[invalid="true"] { color: %(critical); font-weight: 600; }
        #authHint[ok="true"] { color: %(normal); font-weight: 600; }
        #authError { color: %(critical); font-size: 12px; font-weight: 600; }

        #authPrimary { background: %(accent); color: #fff; border: none; border-radius: 8px;
                       font-size: 15px; font-weight: 700; letter-spacing: 1px; }
        #authPrimary:hover { background: %(accentHover); }
        #authPrimary:disabled { background: %(border); color: %(sub); }

        /* 링크처럼 보이는 보조 버튼 (회원가입 / 취소) */
        #authLink { background: transparent; color: %(accent); border: none;
                    font-size: 13px; font-weight: 700; padding: 6px; }
        #authLink:hover { color: %(accentHover); text-decoration: underline; }

        #authSep { color: %(border); }
        #authFooter { color: %(sub); font-size: 11px; }
    )")
        .replace("%(bgDeep)", kDark.bgDeep)
        .replace("%(panel)", kDark.panel)
        .replace("%(border)", kDark.border)
        .replace("%(text)", kDark.text)
        .replace("%(sub)", kDark.sub)
        .replace("%(normal)", kDark.normal)
        .replace("%(accentHover)", "#3AD4C4")   // accent(#17C7B6)보다 한 단계 밝은 톤
        .replace("%(accent)", kDark.accent)
        .replace("%(critical)", kDark.critical);
}

#endif  // THEME_H
