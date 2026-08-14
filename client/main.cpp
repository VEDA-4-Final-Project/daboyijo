#include "auth.h"
#include "logindialog.h"
#include "mainwindow.h"

#include <QApplication>
#include <QLocale>
#include <QTranslator>
#include <QSqlDatabase>
#include <QSqlError>


int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // QSettings 저장 위치를 고정 — 카메라 연결 IP·계정 등 관제 PC 로컬 설정이
    // 기본 생성자 QSettings()로 일관되게 읽고 쓰이도록 조직/앱 이름을 지정한다.
    QApplication::setOrganizationName(QStringLiteral("daboyijo"));
    QApplication::setApplicationName(QStringLiteral("gvm-client"));

    QSqlDatabase db = QSqlDatabase::addDatabase("QMARIADB");
    db.setHostName("172.20.32.51");
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
