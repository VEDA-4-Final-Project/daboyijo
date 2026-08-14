#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include <QString>

#include "theme.h"   // Palette 타입이 필요하다

// 앱 전역 QSS 한 벌을 리소스(:/style/base.qss)에서 읽어 팔레트 토큰을 치환한다.
//
// [커밋 1 상태] 아직 qApp에 적용하는 진입점(applyStylesheet)은 없다.
// MainWindow::applyTheme()가 아래 두 헬퍼를 직접 조합해 지금까지처럼
// this->setStyleSheet(...)로 적용한다 — 구조만 파일 밖으로 옮긴 상태.
// (다음 커밋에서 이 조합을 applyStylesheet()로 감싸고 적용 주체를 qApp으로 올린다.)
class ThemeManager {
public:
    // 리소스 경로(:/style/... 형태)의 QSS 텍스트를 UTF-8로 읽어 돌려준다.
    // 리소스가 없거나 열 수 없으면 qCritical()로 [Theme] 접두 로그를 남기고
    // 빈 문자열을 돌려준다 — 조용한 저하가 아니라 눈에 띄는 실패가 되게 한다.
    static QString loadQss(const QString& resourcePath);

    // qss 안의 %(token) 자리를 팔레트 p의 값으로 치환한다.
    // %(accentHover)는 %(accent)의 부분 문자열이라 반드시 %(accent)보다 먼저
    // 치환해야 한다 — 순서를 바꾸면 정확한 accentHover 자리까지 accent로 덮인다.
    static QString substitute(QString qss, const Palette& p, bool darkMode);
};

#endif  // THEMEMANAGER_H
