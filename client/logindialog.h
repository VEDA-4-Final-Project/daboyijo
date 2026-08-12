#ifndef LOGINDIALOG_H
#define LOGINDIALOG_H

#include <QDialog>

#include "auth.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

// 앱 진입 시 가장 먼저 뜨는 로그인 창.
// accept()되면 user()로 로그인한 사용자 정보를 꺼내 MainWindow에 넘긴다.
class LoginDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LoginDialog(QWidget* parent = nullptr);

    Auth::SessionUser user() const { return user_; }

private slots:
    void attemptLogin();
    void openSignup();     // "회원가입" — 가입 창을 열고, 성공하면 아이디를 채워준다

private:
    void buildUi();
    void showError(const QString& message);
    void clearError();

    QLineEdit*   idEdit_       = nullptr;
    QLineEdit*   pwEdit_       = nullptr;
    QCheckBox*   rememberBox_  = nullptr;
    QPushButton* loginButton_  = nullptr;
    QLabel*      errorLabel_   = nullptr;

    Auth::SessionUser user_;
};

#endif  // LOGINDIALOG_H
