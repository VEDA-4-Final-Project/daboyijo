#include "thememanager.h"

#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QTextStream>

void ThemeManager::applyStylesheet(const Palette& p, bool darkMode)
{
    qApp->setStyleSheet(substitute(loadQss(":/style/base.qss"), p, darkMode));
}

QString ThemeManager::loadQss(const QString& resourcePath)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCritical() << "[Theme] QSS 리소스를 열 수 없습니다:" << resourcePath;
        return QString();
    }
    QTextStream stream(&file);
    return stream.readAll();
}

QString ThemeManager::substitute(QString qss, const Palette& p, bool darkMode)
{
    return qss
        .replace("%(bgDeep)", p.bgDeep)
        .replace("%(panel)", p.panel)
        .replace("%(card)", p.card)
        .replace("%(border)", p.border)
        .replace("%(text)", p.text)
        .replace("%(sub)", p.sub)
        .replace("%(normal)", p.normal)
        .replace("%(warn)", p.warn)
        // accentHover는 accent의 부분문자열이라 반드시 accent보다 먼저 치환.
        .replace("%(accentHover)", darkMode ? "#3AD4C4" : "#3AD1C3")
        .replace("%(accent)", p.accent)
        .replace("%(critical)", p.critical);
}

QString ThemeManager::dialogStyleSheet()
{
    // dialogs.qss는 base.qss와 토큰 집합이 다르다(%(card), %(warn) 없음).
    // substitute()에 억지로 합치면 두 시트의 토큰 차이가 흐려지므로 별도 치환 체인을 쓴다.
    // 값은 전부 kDark 고정 — 현재 팔레트(darkMode 인자)를 받지 않는다.
    return loadQss(":/style/dialogs.qss")
        .replace("%(bgDeep)", kDark.bgDeep)
        .replace("%(panel)", kDark.panel)
        .replace("%(border)", kDark.border)
        .replace("%(text)", kDark.text)
        .replace("%(sub)", kDark.sub)
        .replace("%(normal)", kDark.normal)
        // accentHover는 accent의 부분문자열이라 반드시 accent보다 먼저 치환.
        .replace("%(accentHover)", "#3AD4C4")   // accent(#17C7B6)보다 한 단계 밝은 톤
        .replace("%(accent)", kDark.accent)
        .replace("%(critical)", kDark.critical);
}
