#ifndef VITALRULES_H
#define VITALRULES_H

// 바이탈 판정 규칙 — mainwindow_p.h 의 vitalLevel/vitalSeverity/severityGlyph 와 같은 값.
// ★ 0은 "값 없음"이지 "이상"이 아니다(웨어러블을 벗으면 심박·SpO2가 모두 0으로 온다).
#include <QString>

namespace VitalRules {
enum class Level { Normal, Warn, Critical };

inline bool worn(int spo2, int hr) { return spo2 > 0 || hr > 0; }

inline Level level(int spo2, int hr)
{
    const bool hasSpo2 = spo2 > 0, hasHr = hr > 0;
    if ((hasSpo2 && spo2 < 90) || (hasHr && (hr >= 110 || hr <= 45))) return Level::Critical;
    if ((hasSpo2 && spo2 < 95) || (hasHr && (hr >= 100 || hr < 55))) return Level::Warn;
    return Level::Normal;
}
inline QString severity(int spo2, int hr)
{
    switch (level(spo2, hr)) {
    case Level::Critical: return QStringLiteral("critical");
    case Level::Warn:     return QStringLiteral("medium");
    default:              return QStringLiteral("normal");
    }
}
inline QString statusLabel(int spo2, int hr)
{
    switch (level(spo2, hr)) {
    case Level::Critical: return QStringLiteral("위험");
    case Level::Warn:     return QStringLiteral("주의");
    default:              return QStringLiteral("정상");
    }
}
inline QString glyph(const QString& severity, bool stale = false)
{
    if (stale) return QStringLiteral("○");
    if (severity == QLatin1String("critical")) return QStringLiteral("✖");
    if (severity == QLatin1String("high"))     return QStringLiteral("▲");
    if (severity == QLatin1String("medium"))   return QStringLiteral("◆");
    if (severity == QLatin1String("info"))     return QStringLiteral("●");
    if (severity == QLatin1String("normal"))   return QStringLiteral("✓");
    return QStringLiteral("○");
}
constexpr qint64 kStaleMs = 30000;   // 이 시간 새 값이 없으면 "신호 끊김"
constexpr int kHistoryMax = 40;
}  // namespace VitalRules

#endif
