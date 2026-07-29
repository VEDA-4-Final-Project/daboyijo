#ifndef SIGNUPDIALOG_H
#define SIGNUPDIALOG_H

#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

// 로그인 창의 "회원가입"에서 열리는 계정 생성 창.
// accept()되면 createdLoginId()로 방금 만든 아이디를 꺼내 로그인 칸을 채운다.
class SignupDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SignupDialog(QWidget* parent = nullptr);

    QString createdLoginId() const { return createdLoginId_; }

private slots:
    void attemptSignup();

private:
    void buildUi();

    // 캡션 + 입력칸 + 안내문 한 묶음을 만들어 레이아웃에 넣는다.
    // outHint에 안내문 라벨이 담겨 나오고, 검증 결과를 여기에 표시한다.
    QLineEdit* addField(QVBoxLayout* lay, const QString& caption,
                        const QString& placeholder, QLabel** outHint);

    // 칸/안내문의 상태를 바꾼다. message가 비면 안내문을 지우고 테두리도 원래대로.
    void setFieldState(QLineEdit* edit, QLabel* hint, const QString& message, bool isError);

    // 각 칸 실시간 검사 — 입력 도중 바로 알려주기 위한 것들
    void checkLoginId();      // 형식 + DB 중복 (포커스가 빠질 때)
    void checkPassword();
    void checkPasswordConfirm();

    // 네 칸이 모두 유효할 때만 [가입하기]를 활성화한다.
    void updateSubmitEnabled();

    QLineEdit* idEdit_      = nullptr;
    QLineEdit* nameEdit_    = nullptr;
    QLineEdit* pwEdit_      = nullptr;
    QLineEdit* pwConfirm_   = nullptr;

    QLabel* idHint_         = nullptr;
    QLabel* nameHint_       = nullptr;
    QLabel* pwHint_         = nullptr;
    QLabel* pwConfirmHint_  = nullptr;

    QLabel*      errorLabel_   = nullptr;
    QPushButton* submitButton_ = nullptr;

    // 중복 확인까지 끝난 아이디만 true. 입력이 바뀌면 다시 false가 된다.
    bool idChecked_ = false;

    QString createdLoginId_;
};

#endif  // SIGNUPDIALOG_H
