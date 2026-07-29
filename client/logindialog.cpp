#include "logindialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSqlDatabase>
#include <QVBoxLayout>

#include "signupdialog.h"
#include "theme.h"

namespace {
// "아이디 저장" 체크 시 마지막 로그인 아이디를 남겨둘 위치
constexpr char kOrgName[]    = "daboyijo";
constexpr char kAppName[]    = "control";
constexpr char kKeySavedId[] = "login/savedId";
}  // namespace

LoginDialog::LoginDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("다보이조 · 로그인"));
    setFixedSize(420, 540);
    // 로그인 창에는 최대화/도움말 버튼이 의미 없다 — 닫기만 남긴다.
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);

    buildUi();
    setStyleSheet(authDialogStyleSheet());   // 회원가입 창과 같은 스타일

    // 중괄호 초기화 — 괄호로 쓰면 컴파일러가 함수 선언으로 읽는다(most vexing parse)
    QSettings settings{QLatin1String(kOrgName), QLatin1String(kAppName)};
    const QString savedId = settings.value(QLatin1String(kKeySavedId)).toString();
    if (!savedId.isEmpty()) {
        idEdit_->setText(savedId);
        rememberBox_->setChecked(true);
        pwEdit_->setFocus();   // 아이디가 이미 있으면 비밀번호 칸부터
    } else {
        idEdit_->setFocus();
    }
}

void LoginDialog::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    auto* card = new QFrame();
    card->setObjectName("authCard");
    root->addWidget(card);

    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(38, 40, 38, 26);
    lay->setSpacing(0);

    // ── 로고 / 타이틀 ──
    auto* logo = new QLabel(QStringLiteral("다보이조"));
    logo->setObjectName("authLogo");
    logo->setAlignment(Qt::AlignCenter);
    lay->addWidget(logo);

    lay->addSpacing(6);
    auto* subtitle = new QLabel(QStringLiteral("요양원 통합 관제 시스템"));
    subtitle->setObjectName("authSubtitle");
    subtitle->setAlignment(Qt::AlignCenter);
    lay->addWidget(subtitle);

    lay->addSpacing(32);

    // ── 입력 필드 ──
    auto* idCaption = new QLabel(QStringLiteral("아이디"));
    idCaption->setObjectName("authCaption");
    lay->addWidget(idCaption);
    lay->addSpacing(6);

    idEdit_ = new QLineEdit();
    idEdit_->setObjectName("authEdit");
    idEdit_->setPlaceholderText(QStringLiteral("아이디를 입력하세요"));
    idEdit_->setMaxLength(50);
    lay->addWidget(idEdit_);

    lay->addSpacing(16);

    auto* pwCaption = new QLabel(QStringLiteral("비밀번호"));
    pwCaption->setObjectName("authCaption");
    lay->addWidget(pwCaption);
    lay->addSpacing(6);

    pwEdit_ = new QLineEdit();
    pwEdit_->setObjectName("authEdit");
    pwEdit_->setPlaceholderText(QStringLiteral("비밀번호를 입력하세요"));
    pwEdit_->setEchoMode(QLineEdit::Password);
    pwEdit_->setMaxLength(128);
    lay->addWidget(pwEdit_);

    lay->addSpacing(12);

    rememberBox_ = new QCheckBox(QStringLiteral("아이디 저장"));
    rememberBox_->setObjectName("authCheck");
    rememberBox_->setCursor(Qt::PointingHandCursor);
    lay->addWidget(rememberBox_);

    // ── 오류 메시지 ──
    // 자리를 미리 잡아둬야 오류가 떴다 사라질 때 아래 버튼이 튀지 않는다.
    errorLabel_ = new QLabel();
    errorLabel_->setObjectName("authError");
    errorLabel_->setAlignment(Qt::AlignCenter);
    errorLabel_->setWordWrap(true);
    errorLabel_->setFixedHeight(34);
    lay->addSpacing(8);
    lay->addWidget(errorLabel_);

    // ── 로그인 버튼 ──
    loginButton_ = new QPushButton(QStringLiteral("로그인"));
    loginButton_->setObjectName("authPrimary");
    loginButton_->setCursor(Qt::PointingHandCursor);
    loginButton_->setDefault(true);       // Enter로 제출
    loginButton_->setAutoDefault(true);
    loginButton_->setFixedHeight(44);
    lay->addWidget(loginButton_);

    // ── 회원가입 안내 ──
    lay->addSpacing(10);
    auto* signupRow = new QHBoxLayout();
    signupRow->setSpacing(2);
    signupRow->addStretch();

    auto* signupNote = new QLabel(QStringLiteral("계정이 없으신가요?"));
    signupNote->setObjectName("authFooter");
    signupRow->addWidget(signupNote);

    auto* signupButton = new QPushButton(QStringLiteral("회원가입"));
    signupButton->setObjectName("authLink");
    signupButton->setCursor(Qt::PointingHandCursor);
    signupButton->setAutoDefault(false);   // Enter는 로그인 버튼이 받아야 한다
    signupRow->addWidget(signupButton);

    signupRow->addStretch();
    lay->addLayout(signupRow);

    lay->addStretch();

    // DB가 안 열려 있으면 눌러보기 전에 미리 알려준다(시연 중 원인 파악 시간 절약).
    auto* dbNote = new QLabel();
    dbNote->setObjectName("authFooter");
    dbNote->setAlignment(Qt::AlignCenter);
    if (!QSqlDatabase::database().isOpen())
        dbNote->setText(QStringLiteral("⚠ 데이터베이스에 연결되어 있지 않습니다"));
    else
        dbNote->setText(QStringLiteral("v1.0 · 관제 클라이언트"));
    lay->addWidget(dbNote);

    connect(loginButton_, &QPushButton::clicked, this, &LoginDialog::attemptLogin);
    connect(signupButton, &QPushButton::clicked, this, &LoginDialog::openSignup);
    // 입력을 고치기 시작하면 이전 오류 메시지는 지운다
    connect(idEdit_, &QLineEdit::textEdited, this, &LoginDialog::clearError);
    connect(pwEdit_, &QLineEdit::textEdited, this, &LoginDialog::clearError);
    // 아이디 칸에서 Enter → 비밀번호로, 비밀번호에서 Enter → 로그인
    connect(idEdit_, &QLineEdit::returnPressed, this, [this] { pwEdit_->setFocus(); });
    connect(pwEdit_, &QLineEdit::returnPressed, this, &LoginDialog::attemptLogin);
}

void LoginDialog::openSignup()
{
    SignupDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    // 방금 만든 아이디를 채워두고 비밀번호 칸으로 — 곧바로 로그인할 수 있게.
    clearError();
    idEdit_->setText(dlg.createdLoginId());
    pwEdit_->clear();
    pwEdit_->setFocus();
}

void LoginDialog::showError(const QString& message)
{
    errorLabel_->setText(message);
}

void LoginDialog::clearError()
{
    if (!errorLabel_->text().isEmpty())
        errorLabel_->clear();
}

void LoginDialog::attemptLogin()
{
    const QString id = idEdit_->text().trimmed();
    const QString pw = pwEdit_->text();

    if (id.isEmpty()) {
        showError(QStringLiteral("아이디를 입력하세요."));
        idEdit_->setFocus();
        return;
    }
    if (pw.isEmpty()) {
        showError(QStringLiteral("비밀번호를 입력하세요."));
        pwEdit_->setFocus();
        return;
    }

    // PBKDF2 10만 회는 수십 ms 걸린다 — 그동안 버튼을 잠가 중복 제출을 막는다.
    clearError();
    loginButton_->setEnabled(false);
    loginButton_->setText(QStringLiteral("확인 중…"));
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    QApplication::processEvents();   // 잠긴 버튼 상태를 즉시 그려준다

    QString detail;
    Auth::SessionUser found;
    const Auth::Result result = Auth::verify(id, pw, &found, &detail);

    QGuiApplication::restoreOverrideCursor();
    loginButton_->setEnabled(true);
    loginButton_->setText(QStringLiteral("로그인"));

    switch (result) {
    case Auth::Result::Ok: {
        user_ = found;

        // 중괄호 초기화 — 괄호로 쓰면 컴파일러가 함수 선언으로 읽는다(most vexing parse)
    QSettings settings{QLatin1String(kOrgName), QLatin1String(kAppName)};
        if (rememberBox_->isChecked())
            settings.setValue(QLatin1String(kKeySavedId), id);
        else
            settings.remove(QLatin1String(kKeySavedId));

        accept();
        return;
    }
    case Auth::Result::InvalidCredentials:
        // 어느 쪽이 틀렸는지 알려주지 않는다 — 아이디 존재 여부가 새어나가지 않게.
        showError(QStringLiteral("아이디 또는 비밀번호가 올바르지 않습니다."));
        pwEdit_->clear();
        pwEdit_->setFocus();
        return;
    case Auth::Result::AccountDisabled:
        showError(QStringLiteral("사용이 중지된 계정입니다. 관리자에게 문의하세요."));
        pwEdit_->clear();
        return;
    case Auth::Result::DatabaseError:
        showError(QStringLiteral("데이터베이스에 연결할 수 없습니다.\n%1").arg(detail));
        return;
    }
}
