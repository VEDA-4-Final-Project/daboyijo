#include "logindialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QFrame>
#include <QGraphicsBlurEffect>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QRadialGradient>
#include <QSettings>
#include <QSqlDatabase>
#include <QVBoxLayout>

#include "signupdialog.h"
#include "theme.h"
#include "thememanager.h"
#include "wintheme.h"

namespace {
// "아이디 저장" 체크 시 마지막 로그인 아이디를 남겨둘 위치
constexpr char kOrgName[]    = "daboyijo";
constexpr char kAppName[]    = "control";
constexpr char kKeySavedId[] = "login/savedId";

// 배경 사진을 넣을 자리. 리소스에 이 파일이 있으면 사진을 블러해서 쓰고,
// 없으면 makePlaceholder()가 만든 추상 배경을 쓴다. 즉 사진을 넣는 작업은
// CMakeLists의 qt_add_resources에 파일 한 줄을 더하는 것으로 끝난다 —
// 이 파일은 건드릴 필요가 없다.
constexpr char kBackgroundImage[] = ":/images/login-bg.jpg";

// 카드 너비. 창이 넓어져도 입력 폼 자체는 읽기 좋은 폭을 유지해야 한다.
constexpr int kCardWidth = 400;

// src를 가우시안 블러한 새 픽스맵을 돌려준다.
// QGraphicsBlurEffect는 위젯이 아니라 QGraphicsItem에만 걸 수 있어서
// 일회용 씬에 아이템을 하나 넣고 렌더하는 우회로를 쓴다 — Qt의 표준 관용구다.
QPixmap blurPixmap(const QPixmap& src, qreal radius)
{
    QGraphicsScene scene;
    // 스택이 아니라 힙에 만든다 — addItem()이 소유권을 가져가므로
    // 스택 객체를 넣으면 씬 소멸자가 두 번 지운다.
    auto* item = new QGraphicsPixmapItem(src);
    auto* blur = new QGraphicsBlurEffect;
    blur->setBlurRadius(radius);
    blur->setBlurHints(QGraphicsBlurEffect::QualityHint);
    item->setGraphicsEffect(blur);   // 여기서도 item이 effect의 소유자가 된다
    scene.addItem(item);

    QPixmap out(src.size());
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    scene.render(&painter, QRectF(out.rect()), QRectF(src.rect()));
    return out;
}

// 사진이 아직 없을 때 쓰는 대체 배경.
// 큰 색 덩어리 몇 개를 겹쳐 "심하게 블러된 사진"처럼 보이게 한다.
// 색은 팔레트에서 그대로 가져와 나중에 사진으로 바꿔도 톤이 튀지 않게 했다.
QPixmap makePlaceholder(const QSize& size)
{
    QPixmap pm(size);
    pm.fill(QColor(QLatin1String(kDark.bgDeep)));

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);

    // cx/cy/r은 크기 대비 비율이라 창 크기가 바뀌어도 구도가 유지된다.
    struct Blob { qreal cx, cy, r; const char* color; int alpha; };
    const Blob blobs[] = {
        {0.14, 0.18, 0.62, kDark.accent,  165},   // 좌상단 청록 — 브랜드 색
        {0.88, 0.12, 0.50, kDark.info,    150},   // 우상단 파랑
        {0.80, 0.86, 0.55, kDark.select,  130},   // 우하단 주황 — 온기 한 점
        {0.30, 0.92, 0.45, kDark.bgDeep,  120},   // 좌하단은 눌러 깊이감
    };
    for (const Blob& b : blobs) {
        const QPointF center(size.width() * b.cx, size.height() * b.cy);
        const qreal   radius = size.width() * b.r;

        QColor color(QLatin1String(b.color));
        QRadialGradient gradient(center, radius);
        color.setAlpha(b.alpha);
        gradient.setColorAt(0.0, color);
        color.setAlpha(0);            // 가장자리는 완전히 투명 — 경계선이 안 보이게
        gradient.setColorAt(1.0, color);

        p.setBrush(gradient);
        p.drawEllipse(center, radius, radius);
    }
    return pm;
}
}  // namespace

LoginDialog::LoginDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Carenet · 로그인"));
    // 배경 사진이 "분위기"로 읽히려면 카드 바깥에 여백이 있어야 한다 —
    // 420폭 그대로 두면 카드가 창을 다 덮어 블러가 거의 안 보인다.
    setFixedSize(940, 600);
    // 로그인 창에는 최대화/도움말 버튼이 의미 없다 — 닫기만 남긴다.
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);

    buildUi();
    setStyleSheet(ThemeManager::dialogStyleSheet());   // 회원가입 창과 같은 스타일
    enableDarkTitleBar(this);                // 네이티브 타이틀바도 다크로

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
    // 카드를 창 한가운데 띄운다. 위아래/좌우 stretch가 남긴 여백이
    // 곧 블러 배경이 보이는 자리다.
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addStretch();

    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->addStretch();

    auto* card = new QFrame();
    // authCard(회원가입 창과 공용)가 아니라 전용 이름을 쓴다 — 반투명 유리
    // 스타일이 불투명 배경 위에 뜨는 회원가입 창까지 번지면 거기선 탁해 보인다.
    card->setObjectName("authGlassCard");
    card->setFixedWidth(kCardWidth);
    card_ = card;
    row->addWidget(card);

    row->addStretch();
    root->addLayout(row);
    root->addStretch();

    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(38, 38, 38, 24);
    lay->setSpacing(0);

    // ── 로고 / 타이틀 ──
    auto* logo = new QLabel(QStringLiteral("Carenet"));
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

    // 카드가 내용 높이에 딱 맞아야 가운데 정렬이 성립한다 — 여기에
    // addStretch()를 넣으면 카드가 창 높이만큼 늘어나 여백이 사라진다.
    lay->addSpacing(14);

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

void LoginDialog::ensureBackground()
{
    if (!background_.isNull() && background_.size() == size())
        return;

    // 블러는 픽셀 수에 비례해 비싸다. 1/4로 줄여 블러한 뒤 다시 키우면
    // 축소·확대 자체가 이미 부드럽게 뭉개주므로 반경도 그만큼 작아도 된다.
    const QSize target = size();
    const QSize small  = target / 4;

    // 중괄호 초기화 — 괄호로 쓰면 컴파일러가 함수 선언으로 읽는다(most vexing parse)
    QPixmap source{QLatin1String(kBackgroundImage)};   // 리소스에 없으면 null
    if (source.isNull()) {
        source = makePlaceholder(small);
    } else {
        // Expanding으로 채운 뒤 가운데를 잘라낸다 — 사진 비율이 창과 달라도
        // 여백(레터박스) 없이 꽉 찬다.
        source = source.scaled(small, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        const int x = (source.width()  - small.width())  / 2;
        const int y = (source.height() - small.height()) / 2;
        source = source.copy(x, y, small.width(), small.height());
    }

    background_ = blurPixmap(source, 14).scaled(target, Qt::IgnoreAspectRatio,
                                                Qt::SmoothTransformation);
}

void LoginDialog::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    ensureBackground();

    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);

    // 블러는 가장자리 픽셀을 투명 쪽으로 번지게 만든다. 창보다 살짝 크게 그려
    // 그 옅어진 테두리를 화면 밖으로 밀어낸다.
    p.fillRect(rect(), QColor(QLatin1String(kDark.bgDeep)));
    p.drawPixmap(rect().adjusted(-16, -16, 16, 16), background_);

    // 배경을 한 겹 눌러 카드가 앞으로 나와 보이게 한다. 다만 너무 짙게 덮으면
    // 블러 배경이 그냥 검은 화면이 되어 버린다 — 색이 남을 만큼만 덮는다.
    // 카드 안 글자의 대비는 이 장막이 아니라 카드 자체의 반투명 배경이 책임진다.
    QLinearGradient veil(0, 0, 0, height());
    veil.setColorAt(0.0, QColor(10, 15, 21, 55));
    veil.setColorAt(1.0, QColor(10, 15, 21, 135));
    p.fillRect(rect(), veil);

    // 카드 그림자. QGraphicsDropShadowEffect를 카드에 걸면 카드와 그 자식
    // 전체가 오프스크린으로 렌더돼 QLineEdit 커서 깜빡임이 어긋난다 —
    // 그래서 효과 대신 여기서 직접 그린다.
    if (card_) {
        const QRect g = card_->geometry();
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 12));
        for (int i = 1; i <= 10; ++i)
            p.drawRoundedRect(g.adjusted(-i, -i + 2, i, i + 5), 18 + i, 18 + i);
    }
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
