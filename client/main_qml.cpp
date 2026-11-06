// 다보이조 QML 관제 앱 진입점.
// 위젯 앱(main.cpp + MainWindow)과 별도 실행 파일이다 — 시연 중인 위젯 앱을 건드리지
// 않고, 화면을 한 페이지씩 QML로 옮겨 오기 위해서다. 통신(StreamClient)은 같은 코드를 쓴다.

#include <QFont>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>

#include "alertmatrixitem.h"
#include "appbackend.h"
#include "caredirectory.h"
#include "devicebackend.h"
#include "eventlogbackend.h"
#include "frameprovider.h"
#include "monitorbackend.h"
#include "mqttqtmanager.h"
#include "qmlitems.h"
#include "reportbackend.h"
#include "residentbackend.h"
#include "streamclient.h"
#include "theme.h"
#include "vitalsbackend.h"

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    // 위젯 앱과 같은 QSettings(서버 주소 등)를 읽도록 이름을 맞춘다
    QCoreApplication::setOrganizationName(QStringLiteral("daboyijo"));
    QCoreApplication::setApplicationName(QStringLiteral("gvm-client"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // 위젯 앱과 같은 모양: 다크 관제 팔레트 + QSS의 기본 글꼴(13px)
    applyPalette(kDark);
    QFont f;
    f.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("맑은 고딕"),
                   QStringLiteral("Malgun Gothic"), QStringLiteral("Apple SD Gothic Neo"),
                   QStringLiteral("Noto Sans CJK KR"), QStringLiteral("Noto Sans KR")});
    f.setPixelSize(13);
    QGuiApplication::setFont(f);

    // 위젯 앱의 커스텀 그리기 위젯을 옮긴 QML 아이템들
    qmlRegisterType<VideoItem>("Daboyijo", 1, 0, "VideoItem");
    qmlRegisterType<TimelineItem>("Daboyijo", 1, 0, "TimelineItem");
    qmlRegisterType<SparklineItem>("Daboyijo", 1, 0, "SparklineItem");
    qmlRegisterType<AlarmOverlayItem>("Daboyijo", 1, 0, "AlarmOverlayItem");
    qmlRegisterType<BackdropItem>("Daboyijo", 1, 0, "BackdropItem");
    qmlRegisterType<ActivityChartItem>("Daboyijo", 1, 0, "ActivityChartItem");
    qmlRegisterType<AlertMatrixItem>("Daboyijo", 1, 0, "AlertMatrixItem");

    // ── 백엔드(C++ 로직) ─────────────────────────────────
    AppBackend appBackend;
    appBackend.openDatabase();                 // 위젯 main.cpp 와 같은 DB 접속

    StreamClient stream;
    auto* frames = new FrameProvider;          // 엔진이 소유권을 가져간다
    CareDirectory dir;
    MonitorBackend backend(&stream, frames, &dir);
    VitalsBackend vitals(&dir, &backend);
    appBackend.setMqtt(vitals.mqtt());
    ResidentBackend residents(&dir, &stream, &appBackend);
    EventLogBackend events(&dir, &appBackend);
    ReportBackend report;
    DeviceBackend device(&dir, &stream, &vitals);
    VideoItem::setFrameSource(frames);

    // 실시간 경보 → 이벤트 로그 맨 위에 줄 추가
    QObject::connect(&backend, &MonitorBackend::eventRaised, &events,
                     [&events](int ch, int roi, double ms, const QString& type) {
                         events.appendLive(ch, roi, ms, type, QStringLiteral("CAMERA"));
                     });
    // 입소자·침대 매핑이 바뀌면 리포트 탭 목록도 새로
    QObject::connect(&dir, &CareDirectory::changed, &report, &ReportBackend::reloadResidents);

    // 로그인 성공 후에야 데이터·통신을 시작한다 (위젯 앱: LoginDialog → MainWindow)
    bool started = false;
    QObject::connect(&appBackend, &AppBackend::sessionChanged, &app, [&] {
        if (!appBackend.loggedIn() || started) return;
        started = true;
        dir.reload();
        report.reloadResidents();
        events.reload();
        stream.start();
        vitals.start();
        events.refreshNvr();
    });
    QObject::connect(&stream, &StreamClient::serverConnected, &events, [&events] { events.refreshNvr(); });

    qmlRegisterSingletonInstance("Daboyijo", 1, 0, "App", &appBackend);

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("frames"), frames);
    engine.addImageProvider(QStringLiteral("icons"), new IconProvider);
    auto* ctx = engine.rootContext();
    ctx->setContextProperty(QStringLiteral("backend"), &backend);
    ctx->setContextProperty(QStringLiteral("vitals"), &vitals);
    ctx->setContextProperty(QStringLiteral("residents"), &residents);
    ctx->setContextProperty(QStringLiteral("events"), &events);
    ctx->setContextProperty(QStringLiteral("report"), &report);
    ctx->setContextProperty(QStringLiteral("device"), &device);
    // qml/ 폴더(Theme 싱글턴·컴포넌트)를 모듈 없이 바로 쓰도록 import 경로에 둔다
    engine.addImportPath(QStringLiteral("qrc:/qml"));
    const QUrl url(qEnvironmentVariableIsSet("DABOYIJO_QML_DIR")
                       ? QUrl::fromLocalFile(qEnvironmentVariable("DABOYIJO_QML_DIR")
                                             + QStringLiteral("/Main.qml"))
                       : QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.load(url);

    const int rc = app.exec();
    stream.stop();
    return rc;
}
