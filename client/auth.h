#ifndef AUTH_H
#define AUTH_H

#include <QByteArray>
#include <QString>

// 로그인 인증 — 비밀번호 해싱과 DB 조회를 담당한다(UI 없음).
//
// 저장 방식: PBKDF2-HMAC-SHA256.
//   users.salt       = 16바이트 랜덤 salt를 hex로 (32자)
//   users.pw_hash    = 파생키 32바이트를 hex로 (64자)
//   users.iterations = 반복 횟수 (계정마다 다를 수 있어 컬럼으로 보관 —
//                      나중에 횟수를 올려도 기존 계정이 그대로 로그인된다)
//
// 평문 저장을 쓰지 않는 이유: DB가 한 번 유출되면 요양사 계정이 그대로 노출되고,
// 같은 비밀번호를 쓰는 다른 서비스까지 번진다. salt는 계정마다 달라서
// 같은 비밀번호라도 해시가 달라지고, 미리 만들어둔 레인보우 테이블이 무력화된다.

namespace Auth {

// 현재 로그인한 사용자 (세션 정보). MainWindow가 이 값을 들고 다닌다.
struct SessionUser {
    int     userId = -1;
    QString loginId;
    QString name;
    QString role;      // 지금은 전원 'admin' — 역할 분리 시 여기로 분기
    bool isValid() const { return userId >= 0; }
};

// 인증 결과 — 실패 사유를 UI에 구분해서 보여주기 위해 나눠둔다.
enum class Result {
    Ok,
    InvalidCredentials,   // 아이디 없음 또는 비밀번호 불일치 (구분해서 알려주지 않음)
    AccountDisabled,      // status가 '재직'이 아님 (퇴사자 차단)
    DatabaseError,        // 연결 끊김 / 쿼리 실패 — 사용자 잘못이 아님
};

// users 테이블이 없으면 만들고, 계정이 하나도 없으면 초기 관리자(admin/admin1234)를
// 넣는다. main()에서 DB 연결 직후 1회 호출한다.
//
// 앱이 DDL을 직접 실행하는 게 일반적인 구성은 아니지만, 이 프로젝트는 관제 PC를
// 새로 세팅할 때마다 별도 DB 도구 없이 바로 뜨는 편이 낫다고 보고 넣었다.
// CREATE 권한이 없으면 false를 돌려주고, 그때는 sql/users_schema.sql을 수동 실행하면 된다.
// (returns) 테이블이 사용 가능한 상태면 true
bool ensureSchema(QString* errorDetail = nullptr);

// 아이디/비밀번호 검증. 성공하면 outUser가 채워지고 last_login_at이 갱신된다.
Result verify(const QString& loginId, const QString& password,
              SessionUser* outUser, QString* errorDetail = nullptr);

// ── 회원가입 ──────────────────────────────────────────────
enum class SignupResult {
    Ok,
    DuplicateId,     // 이미 쓰는 아이디
    InvalidInput,    // 형식 위반 (아래 validate* 가 걸러낸 경우)
    DatabaseError,
};

// 새 계정을 users에 저장한다. 비밀번호는 해시로만 들어간다.
SignupResult createUser(const QString& loginId, const QString& password,
                        const QString& name, QString* errorDetail = nullptr);

// 아이디 중복 확인. dbError가 true로 나오면 결과값을 신뢰하면 안 된다.
bool loginIdExists(const QString& loginId, bool* dbError = nullptr);

// 입력 형식 검사 — 통과하면 빈 문자열, 아니면 사용자에게 보여줄 사유를 돌려준다.
// (UI가 입력 도중 실시간 안내에도 쓰고, 저장 직전 최종 검사에도 쓴다)
QString validateLoginId(const QString& loginId);
QString validateName(const QString& name);
QString validatePassword(const QString& password);

// 아이디/비밀번호 길이 제한 — UI의 setMaxLength와 DB 컬럼 크기를 여기에 맞춘다.
constexpr int kLoginIdMinLen  = 4;
constexpr int kLoginIdMaxLen  = 20;
constexpr int kPasswordMinLen = 4;    // "1234" 같은 단순 비밀번호도 허용 (시연 편의)
constexpr int kPasswordMaxLen = 128;
constexpr int kNameMaxLen     = 50;

// 새 비밀번호용 salt/해시 생성 (계정 추가·비밀번호 변경 화면에서 사용).
// outSaltHex(32자) / outHashHex(64자) / iterations를 그대로 users에 넣으면 된다.
void makePassword(const QString& password, QString* outSaltHex,
                  QString* outHashHex, int* outIterations);

// 기본 반복 횟수. 올리려면 여기만 바꾸면 되고, 기존 계정은 자기 iterations로 검증된다.
constexpr int kDefaultIterations = 100000;

}  // namespace Auth

#endif  // AUTH_H
