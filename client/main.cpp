#include "mainwindow.h"

#include <QApplication>
#include <QLocale>
#include <QTranslator>
#include <QSqlDatabase>
#include <QSqlError>


int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    QSqlDatabase db = QSqlDatabase::addDatabase("QMARIADB");
    db.setHostName("172.20.35.112");
    db.setPort(3306);
    db.setDatabaseName("daboijo");
    db.setUserName("daboijo");
    db.setPassword("1234");

    if (db.open()) {
        qDebug() << "✅ DB 연결 성공!";
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
    MainWindow w;
    w.show();
    return QCoreApplication::exec();
}
