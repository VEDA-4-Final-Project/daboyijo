#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include <QString>

#include "theme.h"   // Palette 타입이 필요하다

// 앱 전역 QSS 한 벌을 리소스(:/style/base.qss)에서 읽어 팔레트 토큰을 치환하고
// qApp 한 곳에 적용한다. 이 클래스가 스타일시트를 적용하는 유일한 통로다.
class ThemeManager {
public:
    // base.qss를 읽어 팔레트 p로 치환한 뒤 qApp에 적용한다.
    //
    // 적용 주체가 위젯이 아니라 qApp이라, 이 한 번의 호출로 로그인 창을 포함한
    // 모든 최상위 창이 같은 시트를 받는다. 호출 지점은 두 곳뿐이다:
    //   1) main.cpp — QApplication 생성 직후, 첫 LoginDialog보다 먼저
    //   2) MainWindow::applyTheme() — 생성자와 테마 토글에서
    // 매 프레임이나 상태 변경마다 부르면 안 된다. 전체 스타일시트 재계산은
    // 비싸고, 4채널 실시간 영상과 CPU를 나눠 쓴다.
    static void applyStylesheet(const Palette& p, bool darkMode);

    // 리소스 경로(:/style/... 형태)의 QSS 텍스트를 UTF-8로 읽어 돌려준다.
    // 리소스가 없거나 열 수 없으면 qCritical()로 [Theme] 접두 로그를 남기고
    // 빈 문자열을 돌려준다 — 조용한 저하가 아니라 눈에 띄는 실패가 되게 한다.
    static QString loadQss(const QString& resourcePath);

    // qss 안의 %(token) 자리를 팔레트 p의 값으로 치환한다.
    // %(accentHover)는 %(accent)의 부분 문자열이라 반드시 %(accent)보다 먼저
    // 치환해야 한다 — 순서를 바꾸면 정확한 accentHover 자리까지 accent로 덮인다.
    static QString substitute(QString qss, const Palette& p, bool darkMode);

    // 로그인·회원가입 창 전용 시트. 앱 테마와 무관하게 항상 다크 팔레트로 굳는다.
    // qApp이 아니라 각 다이얼로그가 자기 자신에게 직접 적용한다 (dialogs.qss 상단 주석 참조).
    static QString dialogStyleSheet();
};

#endif  // THEMEMANAGER_H
