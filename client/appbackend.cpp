#include "appbackend.h"

#include <QCoreApplication>
#include <QDebug>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>

#include "audiotransmitter.h"
#include "mqttqtmanager.h"
#include "theme.h"

namespace {
// 위젯 로그인 창과 같은 저장 위치 (LoginDialog: org daboyijo / app control)
QSettings loginSettings() { return QSettings(QStringLiteral("daboyijo"), QStringLiteral("control")); }
constexpr char kKeySavedId[] = "login/savedId";
// 인터콤 수신 노드 (mainwindow_monitor.cpp onMicToggled 와 같은 값)
constexpr char kAlertIp[] = "172.20.35.40";
constexpr quint16 kAlertPort = 5000;
}  // namespace

AppBackend::AppBackend(QObject* parent) : QObject(parent), tx_(new AudioTransmitter(this))
{
    connect(tx_, &AudioTransmitter::errorOccurred, this, [this](const QString& m) {
        qWarning() << "[방송]" << m;
    });
}

bool AppBackend::openDatabase()
{
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QMARIADB"));
    db.setHostName(QStringLiteral("172.20.32.79"));
    db.setPort(3306);
    db.setDatabaseName(QStringLiteral("daboijo"));
    db.setUserName(QStringLiteral("daboijo"));
    db.setPassword(QStringLiteral("1234"));
    db.setConnectOptions(QStringLiteral("MYSQL_OPT_SSL_VERIFY_SERVER_CERT=0"));
    if (db.open()) {
        qDebug() << "✅ DB 연결 성공!";
        QString schemaError;
        if (!Auth::ensureSchema(&schemaError)) qDebug() << "⚠️ 로그인 테이블 준비 실패 —" << schemaError;
    } else {
        qDebug() << "❌ DB 연결 실패:" << db.lastError().text();
    }
    emit dbChanged();
    return db.isOpen();
}

bool AppBackend::dbConnected() const { return QSqlDatabase::database().isOpen(); }
QString AppBackend::savedId() const { return loginSettings().value(QLatin1String(kKeySavedId)).toString(); }
bool AppBackend::broadcasting() const { return broadcasting_; }

QVariantMap AppBackend::login(const QString& id, const QString& password, bool remember)
{
    if (id.trimmed().isEmpty()) return {{"ok", false}, {"error", QStringLiteral("아이디를 입력하세요.")}, {"field", "id"}};
    if (password.isEmpty()) return {{"ok", false}, {"error", QStringLiteral("비밀번호를 입력하세요.")}, {"field", "pw"}};

    QString detail;
    Auth::SessionUser found;
    switch (Auth::verify(id.trimmed(), password, &found, &detail)) {
    case Auth::Result::Ok: {
        user_ = found;
        QSettings s = loginSettings();
        if (remember) s.setValue(QLatin1String(kKeySavedId), id.trimmed());
        else s.remove(QLatin1String(kKeySavedId));
        emit sessionChanged();
        return {{"ok", true}};
    }
    case Auth::Result::InvalidCredentials:
        return {{"ok", false}, {"error", QStringLiteral("아이디 또는 비밀번호가 올바르지 않습니다.")}, {"field", "pw"}};
    case Auth::Result::AccountDisabled:
        return {{"ok", false}, {"error", QStringLiteral("사용이 중지된 계정입니다. 관리자에게 문의하세요.")}, {"field", "pw"}};
    case Auth::Result::DatabaseError:
        return {{"ok", false}, {"error", QStringLiteral("데이터베이스에 연결할 수 없습니다.\n%1").arg(detail)}};
    }
    return {{"ok", false}};
}

void AppBackend::logout()
{
    setBroadcast(false);
    user_ = Auth::SessionUser();
    emit sessionChanged();
    emit loggedOut();
}

QString AppBackend::validateLoginId(const QString& id) const { return Auth::validateLoginId(id.trimmed()); }
QString AppBackend::validateName(const QString& n) const { return Auth::validateName(n.trimmed()); }
QString AppBackend::validatePassword(const QString& p) const { return Auth::validatePassword(p); }

QVariantMap AppBackend::checkLoginId(const QString& idRaw) const
{
    const QString id = idRaw.trimmed();
    if (id.isEmpty()) return {{"ok", false}, {"message", QString()}, {"error", false}};
    const QString fmt = Auth::validateLoginId(id);
    if (!fmt.isEmpty()) return {{"ok", false}, {"message", fmt}, {"error", true}};
    bool dbError = false;
    const bool exists = Auth::loginIdExists(id, &dbError);
    if (dbError) return {{"ok", false}, {"message", QStringLiteral("중복 확인 실패 — DB 연결을 확인하세요")}, {"error", true}};
    if (exists) return {{"ok", false}, {"message", QStringLiteral("이미 사용 중인 아이디입니다")}, {"error", true}};
    return {{"ok", true}, {"message", QStringLiteral("사용 가능한 아이디입니다")}, {"error", false}};
}

QVariantMap AppBackend::signup(const QString& idRaw, const QString& nameRaw, const QString& pw)
{
    const QString id = idRaw.trimmed(), name = nameRaw.trimmed();
    QString detail;
    switch (Auth::createUser(id, pw, name, &detail)) {
    case Auth::SignupResult::Ok:
        return {{"ok", true}, {"loginId", id}, {"name", name}};
    case Auth::SignupResult::DuplicateId:
        return {{"ok", false}, {"field", "id"}, {"error", QStringLiteral("이미 사용 중인 아이디입니다")}};
    case Auth::SignupResult::InvalidInput:
        return {{"ok", false}, {"error", QStringLiteral("입력값을 다시 확인해 주세요.")}};
    case Auth::SignupResult::DatabaseError:
        return {{"ok", false}, {"error", QStringLiteral("계정 저장에 실패했습니다.\n%1").arg(detail)}};
    }
    return {{"ok", false}};
}

void AppBackend::toggleTheme()
{
    dark_ = !dark_;
    applyPalette(dark_ ? kDark : kLight);
    emit darkChanged();
}

void AppBackend::setBroadcast(bool on)
{
    if (on == broadcasting_) return;
    // 위젯 onMicToggled 와 같은 순서: 알림 노드에 알린 뒤 오디오 송출
    if (on) {
        if (mqtt_) mqtt_->sendBroadcastToggle(alarmNode_, true);
        if (!tx_->startBroadcast(QLatin1String(kAlertIp), kAlertPort)) {
            qWarning() << "방송 시작 실패";
            if (mqtt_) mqtt_->sendBroadcastToggle(alarmNode_, false);
            emit broadcastFailed();
            return;
        }
        qDebug() << "인터콤 방송 시작";
    } else {
        tx_->stopBroadcast();
        if (mqtt_) mqtt_->sendBroadcastToggle(alarmNode_, false);
        qDebug() << "인터콤 방송 종료";
    }
    broadcasting_ = on;
    emit broadcastChanged();
}

QVariant AppBackend::setting(const QString& key, const QVariant& fallback) const
{
    return QSettings().value(key, fallback);
}
void AppBackend::setSetting(const QString& key, const QVariant& value) { QSettings().setValue(key, value); }
void AppBackend::quit() { QCoreApplication::quit(); }
