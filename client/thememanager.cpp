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
