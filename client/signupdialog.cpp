#include "signupdialog.h"

#include <QApplication>
#include <QFrame>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include "auth.h"
#include "theme.h"

namespace {
// 안내문 라벨 높이를 고정해 문구가 생겼다 사라질 때 아래 칸이 밀리지 않게 한다.
constexpr int kHintHeight = 16;
}  // namespace

SignupDialog::SignupDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("회원가입"));
    setFixedSize(440, 660);
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);

    buildUi();
    setStyleSheet(authDialogStyleSheet());   // 로그인 창과 같은 스타일

    idEdit_->setFocus();
    updateSubmitEnabled();
}

QLineEdit* SignupDialog::addField(QVBoxLayout* lay, const QString& caption,
                                  const QString& placeholder, QLabel** outHint)
{
    auto* cap = new QLabel(caption);
    cap->setObjectName("authCaption");
    lay->addWidget(cap);
    lay->addSpacing(6);

    auto* edit = new QLineEdit();
    edit->setObjectName("authEdit");
    edit->setPlaceholderText(placeholder);
    lay->addWidget(edit);

    auto* hint = new QLabel();
    hint->setObjectName("authHint");
    hint->setFixedHeight(kHintHeight);
    lay->addSpacing(3);
    lay->addWidget(hint);

    *outHint = hint;
    return edit;
}

void SignupDialog::setFieldState(QLineEdit* edit, QLabel* hint,
                                 const QString& message, bool isError)
{
    hint->setText(message);
    // 동적 속성 + unpolish/polish 조합이라야 QSS의 [invalid="true"] 규칙이 다시 먹는다.
    hint->setProperty("invalid", isError);
    hint->setProperty("ok", !isError && !message.isEmpty());
    hint->style()->unpolish(hint);
    hint->style()->polish(hint);

    edit->setProperty("invalid", isError);
    edit->style()->unpolish(edit);
    edit->style()->polish(edit);
}

void SignupDialog::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    auto* card = new QFrame();
    card->setObjectName("authCard");
    root->addWidget(card);

    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(38, 30, 38, 26);
    lay->setSpacing(0);

    // ── 제목 ──
    auto* title = new QLabel(QStringLiteral("회원가입"));
    title->setObjectName("authTitle");
    title->setAlignment(Qt::AlignCenter);
    lay->addWidget(title);

    lay->addSpacing(6);
    auto* subtitle = new QLabel(QStringLiteral("관제 시스템에서 사용할 계정을 만듭니다"));
    subtitle->setObjectName("authSubtitle");
    subtitle->setAlignment(Qt::AlignCenter);
    lay->addWidget(subtitle);

    lay->addSpacing(24);

    // ── 입력 칸 ──
    idEdit_ = addField(lay, QStringLiteral("아이디"),
                       QStringLiteral("영문 소문자·숫자 %1~%2자")
                           .arg(Auth::kLoginIdMinLen).arg(Auth::kLoginIdMaxLen),
                       &idHint_);
    idEdit_->setMaxLength(Auth::kLoginIdMaxLen);

    nameEdit_ = addField(lay, QStringLiteral("이름"),
                         QStringLiteral("화면에 표시될 이름"), &nameHint_);
    nameEdit_->setMaxLength(Auth::kNameMaxLen);

    pwEdit_ = addField(lay, QStringLiteral("비밀번호"),
                       QStringLiteral("%1자 이상").arg(Auth::kPasswordMinLen),
                       &pwHint_);
    pwEdit_->setEchoMode(QLineEdit::Password);
    pwEdit_->setMaxLength(Auth::kPasswordMaxLen);

    pwConfirm_ = addField(lay, QStringLiteral("비밀번호 확인"),
                          QStringLiteral("비밀번호를 다시 입력하세요"), &pwConfirmHint_);
    pwConfirm_->setEchoMode(QLineEdit::Password);
    pwConfirm_->setMaxLength(Auth::kPasswordMaxLen);

    // ── 저장 실패 등 전체 오류 ──
    errorLabel_ = new QLabel();
    errorLabel_->setObjectName("authError");
    errorLabel_->setAlignment(Qt::AlignCenter);
    errorLabel_->setWordWrap(true);
    errorLabel_->setFixedHeight(32);
    lay->addSpacing(4);
    lay->addWidget(errorLabel_);

    // ── 버튼 ──
    submitButton_ = new QPushButton(QStringLiteral("가입하기"));
    submitButton_->setObjectName("authPrimary");
    submitButton_->setCursor(Qt::PointingHandCursor);
    submitButton_->setDefault(true);
    submitButton_->setFixedHeight(44);
    lay->addWidget(submitButton_);

    lay->addSpacing(4);
    auto* cancelButton = new QPushButton(QStringLiteral("취소"));
    cancelButton->setObjectName("authLink");
    cancelButton->setCursor(Qt::PointingHandCursor);
    cancelButton->setAutoDefault(false);
    lay->addWidget(cancelButton);

    lay->addStretch();

    connect(submitButton_, &QPushButton::clicked, this, &SignupDialog::attemptSignup);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

    // ── 실시간 검증 배선 ──
    // 아이디는 글자마다 DB를 때리면 안 되므로 형식만 즉시 보고,
    // 중복 조회는 포커스가 빠질 때(editingFinished) 한 번만 한다.
    connect(idEdit_, &QLineEdit::textEdited, this, [this] {
        idChecked_ = false;
        errorLabel_->clear();
        const QString err = Auth::validateLoginId(idEdit_->text().trimmed());
        setFieldState(idEdit_, idHint_, err, !err.isEmpty());
        updateSubmitEnabled();
    });
    connect(idEdit_, &QLineEdit::editingFinished, this, &SignupDialog::checkLoginId);

    connect(nameEdit_, &QLineEdit::textEdited, this, [this] {
        errorLabel_->clear();
        const QString err = Auth::validateName(nameEdit_->text().trimmed());
        setFieldState(nameEdit_, nameHint_, err, !err.isEmpty());
        updateSubmitEnabled();
    });

    connect(pwEdit_, &QLineEdit::textEdited, this, [this] {
        errorLabel_->clear();
        checkPassword();
        // 비밀번호가 바뀌면 확인란과의 일치 여부도 다시 본다
        if (!pwConfirm_->text().isEmpty())
            checkPasswordConfirm();
        updateSubmitEnabled();
    });
    connect(pwConfirm_, &QLineEdit::textEdited, this, [this] {
        errorLabel_->clear();
        checkPasswordConfirm();
        updateSubmitEnabled();
    });

    // Enter로 다음 칸 이동 → 마지막 칸에서 제출
    connect(idEdit_, &QLineEdit::returnPressed, this, [this] { nameEdit_->setFocus(); });
    connect(nameEdit_, &QLineEdit::returnPressed, this, [this] { pwEdit_->setFocus(); });
    connect(pwEdit_, &QLineEdit::returnPressed, this, [this] { pwConfirm_->setFocus(); });
    connect(pwConfirm_, &QLineEdit::returnPressed, this, &SignupDialog::attemptSignup);
}

void SignupDialog::checkLoginId()
{
    const QString id = idEdit_->text().trimmed();
    if (id.isEmpty()) {          // 빈 칸은 아직 안 쓴 것 — 다그치지 않는다
        setFieldState(idEdit_, idHint_, QString(), false);
        idChecked_ = false;
        updateSubmitEnabled();
        return;
    }

    const QString formatError = Auth::validateLoginId(id);
    if (!formatError.isEmpty()) {
        setFieldState(idEdit_, idHint_, formatError, true);
        idChecked_ = false;
        updateSubmitEnabled();
        return;
    }

    bool dbError = false;
    const bool exists = Auth::loginIdExists(id, &dbError);
    if (dbError) {
        setFieldState(idEdit_, idHint_,
                      QStringLiteral("중복 확인 실패 — DB 연결을 확인하세요"), true);
        idChecked_ = false;
    } else if (exists) {
        setFieldState(idEdit_, idHint_, QStringLiteral("이미 사용 중인 아이디입니다"), true);
        idChecked_ = false;
    } else {
        setFieldState(idEdit_, idHint_, QStringLiteral("사용 가능한 아이디입니다"), false);
        idChecked_ = true;
    }
    updateSubmitEnabled();
}

void SignupDialog::checkPassword()
{
    const QString err = Auth::validatePassword(pwEdit_->text());
    setFieldState(pwEdit_, pwHint_, err, !err.isEmpty());
}

void SignupDialog::checkPasswordConfirm()
{
    if (pwConfirm_->text().isEmpty()) {
        setFieldState(pwConfirm_, pwConfirmHint_, QString(), false);
        return;
    }
    if (pwConfirm_->text() != pwEdit_->text()) {
        setFieldState(pwConfirm_, pwConfirmHint_,
                      QStringLiteral("비밀번호가 일치하지 않습니다"), true);
        return;
    }
    setFieldState(pwConfirm_, pwConfirmHint_, QStringLiteral("비밀번호가 일치합니다"), false);
}

void SignupDialog::updateSubmitEnabled()
{
    const bool ok =
        Auth::validateLoginId(idEdit_->text().trimmed()).isEmpty()
        && Auth::validateName(nameEdit_->text().trimmed()).isEmpty()
        && Auth::validatePassword(pwEdit_->text()).isEmpty()
        && pwConfirm_->text() == pwEdit_->text()
        && !pwConfirm_->text().isEmpty();
    submitButton_->setEnabled(ok);
}

void SignupDialog::attemptSignup()
{
    const QString id   = idEdit_->text().trimmed();
    const QString name = nameEdit_->text().trimmed();
    const QString pw   = pwEdit_->text();

    // 버튼이 열려 있어도 마지막으로 한 번 더 확인한다(Enter 제출 경로 대비).
    struct { QLineEdit* edit; QLabel* hint; QString error; } checks[] = {
        { idEdit_,    idHint_,   Auth::validateLoginId(id) },
        { nameEdit_,  nameHint_, Auth::validateName(name) },
        { pwEdit_,    pwHint_,   Auth::validatePassword(pw) },
    };
    for (auto& c : checks) {
        if (!c.error.isEmpty()) {
            setFieldState(c.edit, c.hint, c.error, true);
            c.edit->setFocus();
            return;
        }
    }
    if (pwConfirm_->text() != pw) {
        setFieldState(pwConfirm_, pwConfirmHint_,
                      QStringLiteral("비밀번호가 일치하지 않습니다"), true);
        pwConfirm_->setFocus();
        return;
    }

    // 아직 중복 확인을 못 했으면(입력 후 바로 Enter) 지금 한다.
    if (!idChecked_) {
        checkLoginId();
        if (!idChecked_) {
            idEdit_->setFocus();
            return;
        }
    }

    errorLabel_->clear();
    submitButton_->setEnabled(false);
    submitButton_->setText(QStringLiteral("가입 중…"));
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    QApplication::processEvents();

    QString detail;
    const Auth::SignupResult result = Auth::createUser(id, pw, name, &detail);

    QGuiApplication::restoreOverrideCursor();
    submitButton_->setText(QStringLiteral("가입하기"));
    submitButton_->setEnabled(true);

    switch (result) {
    case Auth::SignupResult::Ok:
        createdLoginId_ = id;
        QMessageBox::information(
            this, QStringLiteral("가입 완료"),
            QStringLiteral("%1 님의 계정이 생성되었습니다.\n방금 만든 아이디로 로그인하세요.")
                .arg(name));
        accept();
        return;

    case Auth::SignupResult::DuplicateId:
        // 검사 이후 다른 PC에서 선점한 경우 — 여기서 잡힌다
        setFieldState(idEdit_, idHint_, QStringLiteral("이미 사용 중인 아이디입니다"), true);
        idChecked_ = false;
        idEdit_->setFocus();
        return;

    case Auth::SignupResult::InvalidInput:
        errorLabel_->setText(QStringLiteral("입력값을 다시 확인해 주세요."));
        return;

    case Auth::SignupResult::DatabaseError:
        errorLabel_->setText(QStringLiteral("계정 저장에 실패했습니다.\n%1").arg(detail));
        return;
    }
}
