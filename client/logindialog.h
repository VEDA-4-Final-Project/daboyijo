#ifndef LOGINDIALOG_H
#define LOGINDIALOG_H

#include <QDialog>
#include <QPixmap>

#include "auth.h"

class QCheckBox;
class QFrame;
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

protected:
    // 블러 배경은 QSS로 표현할 수 없다(Qt Widgets에는 backdrop-filter가 없다).
    // 창 전체를 직접 그리고, 그 위에 반투명 카드를 얹는 방식으로 낸다.
    void paintEvent(QPaintEvent* event) override;

private slots:
    void attemptLogin();
    void openSignup();     // "회원가입" — 가입 창을 열고, 성공하면 아이디를 채워준다

private:
    void buildUi();
    void showError(const QString& message);
    void clearError();

    // 현재 창 크기에 맞는 블러 배경을 만들어 background_에 캐싱한다.
    // 블러는 비싸므로 크기가 바뀔 때만 다시 만든다 — 매 paintEvent가 아니다.
    void ensureBackground();

    QLineEdit*   idEdit_       = nullptr;
    QLineEdit*   pwEdit_       = nullptr;
    QCheckBox*   rememberBox_  = nullptr;
    QPushButton* loginButton_  = nullptr;
    QLabel*      errorLabel_   = nullptr;
    QFrame*      card_         = nullptr;   // 그림자를 직접 그리려면 카드 위치가 필요하다

    QPixmap      background_;               // 블러가 끝난 배경 (창 크기 기준 캐시)

    Auth::SessionUser user_;
};

#endif  // LOGINDIALOG_H
