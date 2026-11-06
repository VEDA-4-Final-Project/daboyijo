#ifndef APPBACKEND_H
#define APPBACKEND_H

// ═══════════════════════════════════════════════════════════
//  AppBackend — 앱 전체(DB 연결 · 로그인 · 세션 · 테마 · 방송) C++ 로직
// ═══════════════════════════════════════════════════════════
// 위젯 앱의 main.cpp(DB 연결) · LoginDialog · SignupDialog · MainWindow(테마·방송·로그아웃)
// 에 흩어져 있던 로직을 QML이 부를 수 있게 모았다. 인증 자체는 위젯 앱과 같은 Auth 모듈.

#include <QObject>
#include <QString>
#include <QVariantMap>

#include "auth.h"

class AudioTransmitter;
class MqttQtManager;

class AppBackend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool dbConnected READ dbConnected NOTIFY dbChanged)
    Q_PROPERTY(bool loggedIn READ loggedIn NOTIFY sessionChanged)
    Q_PROPERTY(QString userName READ userName NOTIFY sessionChanged)
    Q_PROPERTY(QString savedId READ savedId CONSTANT)
    Q_PROPERTY(bool dark READ dark NOTIFY darkChanged)
    Q_PROPERTY(bool broadcasting READ broadcasting NOTIFY broadcastChanged)
    Q_PROPERTY(QString alarmNode READ alarmNode WRITE setAlarmNode NOTIFY alarmNodeChanged)

public:
    explicit AppBackend(QObject* parent = nullptr);

    bool openDatabase();           // 위젯 main.cpp 와 같은 접속 정보
    bool dbConnected() const;
    bool loggedIn() const { return user_.isValid(); }
    QString userName() const { return user_.name; }
    QString savedId() const;
    bool dark() const { return dark_; }
    bool broadcasting() const;
    QString alarmNode() const { return alarmNode_; }
    void setAlarmNode(const QString& n) { alarmNode_ = n; emit alarmNodeChanged(); }
    void setMqtt(MqttQtManager* m) { mqtt_ = m; }
    const Auth::SessionUser& user() const { return user_; }

    // ── 로그인 ── {ok, error}
    Q_INVOKABLE QVariantMap login(const QString& id, const QString& password, bool remember);
    Q_INVOKABLE void logout();
    // ── 회원가입 (입력 도중 실시간 안내 · 가입) ──
    Q_INVOKABLE QString validateLoginId(const QString& id) const;
    Q_INVOKABLE QString validateName(const QString& name) const;
    Q_INVOKABLE QString validatePassword(const QString& pw) const;
    Q_INVOKABLE QVariantMap checkLoginId(const QString& id) const;   // {ok, message}
    Q_INVOKABLE QVariantMap signup(const QString& id, const QString& name, const QString& pw);
    // ── 테마 / 방송 ──
    Q_INVOKABLE void toggleTheme();
    Q_INVOKABLE void setBroadcast(bool on);
    Q_INVOKABLE QVariant setting(const QString& key, const QVariant& fallback) const;
    Q_INVOKABLE void setSetting(const QString& key, const QVariant& value);
    Q_INVOKABLE void quit();

signals:
    void dbChanged();
    void sessionChanged();
    void darkChanged();
    void broadcastChanged();
    void alarmNodeChanged();
    void loggedOut();
    void broadcastFailed();

private:
    Auth::SessionUser user_;
    bool dark_ = true;
    AudioTransmitter* tx_ = nullptr;
    MqttQtManager* mqtt_ = nullptr;
    QString alarmNode_ = QStringLiteral("alarm_rpi_01");
    bool broadcasting_ = false;
};

#endif
