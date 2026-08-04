#include "auth.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QPasswordDigestor>   // Qt::Network 모듈 (CMake에 이미 링크돼 있음)
#include <QRandomGenerator>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace {

constexpr int kSaltBytes = 16;   // salt 길이 (hex 32자)
constexpr int kKeyBytes  = 32;   // 파생키 길이 (hex 64자, SHA-256 출력과 동일)

// 비밀번호 + salt → 파생키. 저장할 때도, 검증할 때도 반드시 이 함수를 거친다.
QByteArray derive(const QString& password, const QByteArray& salt, int iterations)
{
    return QPasswordDigestor::deriveKeyPbkdf2(
        QCryptographicHash::Sha256, password.toUtf8(), salt, iterations, kKeyBytes);
}

// 길이·내용을 항상 같은 시간에 비교한다.
// 일반 == 는 첫 불일치 바이트에서 즉시 빠져나가므로, 응답 시간 차이로
// "앞 몇 글자가 맞았는지"가 새어나갈 수 있다(타이밍 공격).
bool constantTimeEquals(const QByteArray& a, const QByteArray& b)
{
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}

}  // namespace

namespace Auth {

void makePassword(const QString& password, QString* outSaltHex,
                  QString* outHashHex, int* outIterations)
{
    // 암호학적 난수로 salt 생성. fillRange는 32비트 단위로 채우므로
    // quint32 배열에 받은 뒤 바이트열로 옮긴다.
    quint32 raw[kSaltBytes / sizeof(quint32)];
    QRandomGenerator::system()->fillRange(raw);
    const QByteArray salt(reinterpret_cast<const char*>(raw), kSaltBytes);

    const QByteArray key = derive(password, salt, kDefaultIterations);

    if (outSaltHex)    *outSaltHex = QString::fromLatin1(salt.toHex());
    if (outHashHex)    *outHashHex = QString::fromLatin1(key.toHex());
    if (outIterations) *outIterations = kDefaultIterations;
}

bool ensureSchema(QString* errorDetail)
{
    if (!QSqlDatabase::database().isOpen()) {
        if (errorDetail) *errorDetail = QStringLiteral("DB 연결이 열려 있지 않습니다.");
        return false;
    }

    // sql/users_schema.sql과 같은 정의를 유지할 것.
    QSqlQuery q;
    const bool created = q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS users ("
        "  user_id       INT AUTO_INCREMENT PRIMARY KEY,"
        "  login_id      VARCHAR(50)  NOT NULL UNIQUE,"
        "  pw_hash       CHAR(64)     NOT NULL,"
        "  salt          CHAR(32)     NOT NULL,"
        "  iterations    INT          NOT NULL DEFAULT 100000,"
        "  name          VARCHAR(50)  NOT NULL,"
        "  role          VARCHAR(20)  NOT NULL DEFAULT 'admin',"
        "  status        VARCHAR(10)  NOT NULL DEFAULT '재직',"
        "  caregiver_id  INT          NULL,"
        "  last_login_at DATETIME     NULL,"
        "  created_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci"));

    if (!created) {
        if (errorDetail) *errorDetail = q.lastError().text();
        qDebug() << "❌ users 테이블 준비 실패:" << q.lastError().text()
                 << "— sql/users_schema.sql을 수동으로 실행하세요.";
        return false;
    }

    // 계정이 하나도 없을 때만 초기 관리자를 넣는다.
    // (해시는 여기서 계산하므로 소스에 비밀번호 해시를 박아둘 필요가 없다)
    QSqlQuery countQuery;
    if (!countQuery.exec(QStringLiteral("SELECT COUNT(*) FROM users")) || !countQuery.next()) {
        if (errorDetail) *errorDetail = countQuery.lastError().text();
        return false;
    }
    if (countQuery.value(0).toInt() > 0)
        return true;   // 이미 계정이 있으면 건드리지 않는다

    QString saltHex, hashHex;
    int iterations = kDefaultIterations;
    makePassword(QStringLiteral("admin1234"), &saltHex, &hashHex, &iterations);

    QSqlQuery seed;
    seed.prepare(QStringLiteral(
        "INSERT INTO users (login_id, pw_hash, salt, iterations, name, role, status) "
        "VALUES ('admin', ?, ?, ?, '관리자', 'admin', '재직')"));
    seed.addBindValue(hashHex);
    seed.addBindValue(saltHex);
    seed.addBindValue(iterations);

    if (!seed.exec()) {
        if (errorDetail) *errorDetail = seed.lastError().text();
        qDebug() << "초기 관리자 계정 생성 실패:" << seed.lastError().text();
        return false;
    }

    qDebug() << "✅ 초기 관리자 계정 생성 — admin / admin1234 (반드시 변경할 것)";
    return true;
}

Result verify(const QString& loginId, const QString& password,
              SessionUser* outUser, QString* errorDetail)
{
    if (!QSqlDatabase::database().isOpen()) {
        if (errorDetail) *errorDetail = QStringLiteral("DB 연결이 열려 있지 않습니다.");
        return Result::DatabaseError;
    }

    QSqlQuery q;
    q.prepare(QStringLiteral(
        "SELECT user_id, pw_hash, salt, iterations, name, role, status "
        "FROM users WHERE login_id = ?"));
    q.addBindValue(loginId);

    if (!q.exec()) {
        if (errorDetail) *errorDetail = q.lastError().text();
        qDebug() << "로그인 쿼리 실패:" << q.lastError().text();
        return Result::DatabaseError;
    }
    if (!q.next())
        return Result::InvalidCredentials;   // 아이디가 없어도 "불일치"로만 응답

    const int     userId     = q.value(0).toInt();
    const QString storedHash = q.value(1).toString();
    const QString saltHex    = q.value(2).toString();
    const int     iterations = q.value(3).toInt();
    const QString name       = q.value(4).toString();
    const QString role       = q.value(5).toString();
    const QString status     = q.value(6).toString();

    // iterations가 0/NULL이면 계정 데이터가 잘못 들어간 것 — 기본값으로 검증 시도
    const int iters = iterations > 0 ? iterations : kDefaultIterations;

    // toHex()는 소문자를 내므로 DB 값도 소문자로 맞춰 비교한다
    // (대문자 hex로 넣어둔 계정이 섞여도 로그인되게).
    const QByteArray key = derive(password, QByteArray::fromHex(saltHex.toLatin1()), iters);
    if (!constantTimeEquals(key.toHex(), storedHash.toLatin1().toLower()))
        return Result::InvalidCredentials;

    // 비밀번호는 맞지만 퇴사 처리된 계정 — 통과시키지 않는다.
    if (status != QStringLiteral("재직"))
        return Result::AccountDisabled;

    if (outUser) {
        outUser->userId  = userId;
        outUser->loginId = loginId;
        outUser->name    = name;
        outUser->role    = role;
    }

    // 마지막 접속 시각 기록 — 실패해도 로그인 자체는 성공으로 둔다.
    QSqlQuery upd;
    upd.prepare(QStringLiteral("UPDATE users SET last_login_at = ? WHERE user_id = ?"));
    upd.addBindValue(QDateTime::currentDateTime());
    upd.addBindValue(userId);
    if (!upd.exec())
        qDebug() << "last_login_at 갱신 실패:" << upd.lastError().text();

    return Result::Ok;
}

// ═══════════════════════════════════════════════════════════
//  회원가입
// ═══════════════════════════════════════════════════════════

QString validateLoginId(const QString& loginId)
{
    if (loginId.isEmpty())
        return QStringLiteral("아이디를 입력하세요.");
    if (loginId.length() < kLoginIdMinLen || loginId.length() > kLoginIdMaxLen)
        return QStringLiteral("아이디는 %1~%2자로 입력하세요.")
            .arg(kLoginIdMinLen).arg(kLoginIdMaxLen);

    // 영문 소문자/숫자/밑줄만. 대소문자 구분 혼동과 공백·한글로 인한
    // 로그인 실패를 원천 차단한다(관제 PC를 여럿이 돌려 쓰기 때문).
    for (const QChar c : loginId) {
        const bool ok = (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
                        || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                        || c == QLatin1Char('_');
        if (!ok)
            return QStringLiteral("아이디는 영문 소문자, 숫자, _ 만 쓸 수 있습니다.");
    }
    if (loginId.at(0).isDigit())
        return QStringLiteral("아이디는 영문으로 시작해야 합니다.");

    return QString();
}

QString validateName(const QString& name)
{
    if (name.isEmpty())
        return QStringLiteral("이름을 입력하세요.");
    if (name.length() > kNameMaxLen)
        return QStringLiteral("이름은 %1자 이내로 입력하세요.").arg(kNameMaxLen);
    return QString();
}

QString validatePassword(const QString& password)
{
    if (password.isEmpty())
        return QStringLiteral("비밀번호를 입력하세요.");
    if (password.length() < kPasswordMinLen)
        return QStringLiteral("비밀번호는 %1자 이상이어야 합니다.").arg(kPasswordMinLen);
    if (password.length() > kPasswordMaxLen)
        return QStringLiteral("비밀번호는 %1자 이내로 입력하세요.").arg(kPasswordMaxLen);

    // 문자 종류 조합(영문+숫자) 요구는 두지 않는다 — 시연·개발 편의를 위해
    // "1234" 같은 단순 비밀번호도 허용한다. 조건을 되살리려면 여기에 추가하면 된다.
    return QString();
}

bool loginIdExists(const QString& loginId, bool* dbError)
{
    if (dbError) *dbError = false;

    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT 1 FROM users WHERE login_id = ?"));
    q.addBindValue(loginId);
    if (!q.exec()) {
        if (dbError) *dbError = true;
        qDebug() << "아이디 중복 확인 실패:" << q.lastError().text();
        return false;
    }
    return q.next();
}

SignupResult createUser(const QString& loginId, const QString& password,
                        const QString& name, QString* errorDetail)
{
    if (!QSqlDatabase::database().isOpen()) {
        if (errorDetail) *errorDetail = QStringLiteral("DB 연결이 열려 있지 않습니다.");
        return SignupResult::DatabaseError;
    }

    // UI에서 이미 걸렀더라도 저장 직전에 한 번 더 본다 — 검증은 저장하는 쪽 책임.
    if (!validateLoginId(loginId).isEmpty() || !validateName(name).isEmpty()
        || !validatePassword(password).isEmpty()) {
        if (errorDetail) *errorDetail = QStringLiteral("입력값이 형식에 맞지 않습니다.");
        return SignupResult::InvalidInput;
    }

    bool dbError = false;
    if (loginIdExists(loginId, &dbError))
        return SignupResult::DuplicateId;
    if (dbError) {
        if (errorDetail) *errorDetail = QStringLiteral("아이디 중복 확인에 실패했습니다.");
        return SignupResult::DatabaseError;
    }

    QString saltHex, hashHex;
    int iterations = kDefaultIterations;
    makePassword(password, &saltHex, &hashHex, &iterations);

    QSqlQuery q;
    q.prepare(QStringLiteral(
        "INSERT INTO users (login_id, pw_hash, salt, iterations, name, role, status) "
        "VALUES (?, ?, ?, ?, ?, 'admin', '재직')"));
    q.addBindValue(loginId);
    q.addBindValue(hashHex);
    q.addBindValue(saltHex);
    q.addBindValue(iterations);
    q.addBindValue(name);

    if (!q.exec()) {
        // 중복 확인과 INSERT 사이에 다른 PC에서 같은 아이디를 만들 수 있다.
        // UNIQUE 제약이 최종 방어선이므로 1062(중복 키)는 중복으로 되돌려준다.
        if (q.lastError().nativeErrorCode() == QLatin1String("1062"))
            return SignupResult::DuplicateId;

        if (errorDetail) *errorDetail = q.lastError().text();
        qDebug() << "계정 생성 실패:" << q.lastError().text();
        return SignupResult::DatabaseError;
    }

    qDebug() << "✅ 계정 생성:" << loginId << "/" << name;
    return SignupResult::Ok;
}

}  // namespace Auth
