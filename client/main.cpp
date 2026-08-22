#include "auth.h"
#include "logindialog.h"
#include "mainwindow.h"
#include "thememanager.h"

#include <QApplication>
#include <QLocale>
#include <QStyle>
#include <QStyleFactory>
#include <QTranslator>
#include <QSqlDatabase>
#include <QSqlError>


int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // Windows/macOS/Linux가 각자 다른 기본 위젯 스타일(플랫폼 스타일)을 쓰면
    // 같은 QSS를 걸어도 스크롤바·콤보박스·체크박스·버튼 테두리 모양이 달라진다.
    // Qt 내장 Fusion 스타일로 고정해 플랫폼 기본 스타일에 대한 의존을 끊는다(FOUND-02).
    // 어떤 위젯도 만들어지기 전, 그리고 아래 ThemeManager::applyStylesheet() 호출보다
    // 먼저여야 한다 — 늦으면 로그인 창이 플랫폼 기본 스타일로 잠깐 떴다가 바뀐다.
    if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        QApplication::setStyle(fusion);
    } else {
        // 표준 Qt 배포에서는 일어나지 않지만, 조용히 넘어가면 "특정 환경에서만
        // 다르게 보임"이 원인 불명으로 남는다.
        qWarning() << "[Style] Fusion 스타일을 로드할 수 없어 플랫폼 기본 스타일을 유지합니다.";
    }

    // QSettings 저장 위치를 고정 — 카메라 연결 IP·계정 등 관제 PC 로컬 설정이
    // 기본 생성자 QSettings()로 일관되게 읽고 쓰이도록 조직/앱 이름을 지정한다.
    QApplication::setOrganizationName(QStringLiteral("daboyijo"));
    QApplication::setApplicationName(QStringLiteral("gvm-client"));

    // 앱 전역 스타일시트를 첫 창이 뜨기 전에 한 번 건다.
    // MainWindow의 darkMode 기본값이 true이므로 여기서도 kDark로 맞춘다 —
    // 두 값이 어긋나면 로그인에서 관제로 넘어가는 순간 화면이 한 번 튄다.
    //
    // applyPalette()는 여기서 부르지 않는다. 팔레트 전역(kBgDeep 등)은
    // MainWindow 생성자가 buildUi() 뒤에 세팅하는 기존 순서를 그대로 둔다.
    ThemeManager::applyStylesheet(kDark, /*darkMode=*/true);

    QSqlDatabase db = QSqlDatabase::addDatabase("QMARIADB");
    db.setHostName("172.20.32.79");
    db.setPort(3306);
    db.setDatabaseName("daboijo");
    db.setUserName("daboijo");
    db.setPassword("1234");
    // 서버(172.20.31.17, MariaDB 10.5)는 TLS 없이 뜨는데, 클라이언트가 링크하는
    // MariaDB Connector/C 3.4는 기본값이 "TLS 사용 + 서버 인증서 검증"이라
    // "SSL is required, but the server does not support it"로 접속이 끊긴다.
    // 이 커넥터에는 MYSQL_OPT_SSL_MODE가 없고 MYSQL_OPT_SSL_ENFORCE=0도 효과가 없으며,
    // 아래 옵션만이 TLS 요구를 실제로 해제한다(사내망 평문 접속).
    db.setConnectOptions("MYSQL_OPT_SSL_VERIFY_SERVER_CERT=0");

    if (db.open()) {
        qDebug() << "✅ DB 연결 성공!";
        // users 테이블이 없으면 만들고 초기 관리자 계정을 넣는다(최초 1회만 실제로 동작).
        QString schemaError;
        if (!Auth::ensureSchema(&schemaError))
            qDebug() << "⚠️ 로그인 테이블 준비 실패 —" << schemaError;
    } else {
        qDebug() << "❌ DB 연결 실패:" << db.lastError().text();
    }

    qDebug() << "사용 가능한 DB 드라이버:" << QSqlDatabase::drivers();

    QTranslator translator;
    const QStringList uiLanguages = QLocale::system().uiLanguages();
    for (const QString &locale : uiLanguages) {
        const QString baseName = "daboyijo_" + QLocale(locale).name();
        if (translator.load(":/i18n/" + baseName)) {
            a.installTranslator(&translator);
            break;
        }
    }

    // 로그인 → 관제 → (로그아웃 시) 다시 로그인.
    // 로그아웃할 때마다 MainWindow를 통째로 새로 만들어, 이전 사용자의
    // 화면 상태(경보, 선택된 입소자, 그리던 ROI 등)가 남지 않게 한다.
    while (true) {
        // 재로그인 경로 보정: 직전 세션에서 라이트로 토글하고 로그아웃했다면
        // qApp에는 라이트 시트가 남아 있다. 두 번째 로그인 창이 첫 번째와
        // 다르게 보이지 않도록 여기서 다시 다크로 되돌린다.
        ThemeManager::applyStylesheet(kDark, /*darkMode=*/true);

        LoginDialog login;
        if (login.exec() != QDialog::Accepted)
            break;              // 로그인 창을 닫음 → 앱 종료

        MainWindow w(login.user());
        w.show();
        a.exec();               // 관제 화면이 닫힐 때까지

        if (!w.logoutRequested())
            break;              // 그냥 종료한 것
    }

    return 0;
}
