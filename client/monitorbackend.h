#ifndef MONITORBACKEND_H
#define MONITORBACKEND_H

// ═══════════════════════════════════════════════════════════
//  MonitorBackend — QML 관제 화면이 보는 C++ 백엔드
// ═══════════════════════════════════════════════════════════
// QML 화면은 "무엇을 보여줄지"만 정하고, 데이터는 전부 이 객체에서 받는다.
//   · channels : 채널 4개의 상태(LIVE·경보·프레임 순번) — 2×2 영상 그리드가 Repeater로 그린다
//   · events   : 낙상·침상이탈·생체이상 이벤트 목록(최신순)
//   · 연결 상태 / 검색 / 경보 해제
//
// 통신은 StreamClient가 그대로 맡는다(위젯 앱과 같은 클래스). 즉 위젯 앱에서
// 통신을 떼어 낸 덕분에, QML 화면은 이 얇은 백엔드만 새로 만들면 됐다.

#include <QAbstractListModel>
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

class StreamClient;
class FrameProvider;
class CareDirectory;

// ── 채널 4개 상태 모델 ─────────────────────────────────────
class ChannelModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role { ChannelRole = Qt::UserRole + 1, LiveRole, FrameIdRole, AlarmRole, AlarmTextRole,
                AlertNxRole, AlertNyRole, DisplayNameRole, ZonesRole };

    struct Item {
        bool    live = false;
        int     frameId = 0;      // 프레임이 올 때마다 +1 → QML Image가 새로 읽는다
        bool    alarm = false;
        QString alarmText;        // 타일 경보 배지 문구 (위젯 앱 VideoView::setAlert와 같은 문구)
        qreal   nx = -1, ny = -1;  // 낙상 위치(0~1) — 십자선, 모르면 -1
        bool    fall = false, egress = false, vital = false;   // 토스트 요약용
        qint64  lastFrameMs = 0;
        QString displayName;      // "CH1 · 김복순"
        QVariantList zones;       // 침대 ROI (+이름표)
    };

    explicit ChannelModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Item& item(int ch) { return items_[ch]; }
    const Item& item(int ch) const { return items_[ch]; }
    void notify(int ch, const QVector<int>& roles);

private:
    QVector<Item> items_;
};

// ── 이벤트 목록 모델 (최신이 맨 위) ─────────────────────────
class EventModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role { TimeRole = Qt::UserRole + 1, ChannelRole, TypeRole, TypeLabelRole, WhoRole };

    struct Event {
        QDateTime when;
        int       channel = -1;
        QString   type;        // FALL / EGRESS / VITAL_ABNORMAL
        QString   who;         // "침대 2" / "채널 3"
    };

    explicit EventModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void prepend(const Event& e);

private:
    QVector<Event> events_;
};

// ── 백엔드 ────────────────────────────────────────────────
class MonitorBackend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(ChannelModel* channels READ channels CONSTANT)
    Q_PROPERTY(EventModel* events READ events CONSTANT)
    Q_PROPERTY(int connectedServers READ connectedServers NOTIFY connectionChanged)
    Q_PROPERTY(int totalServers READ totalServers CONSTANT)
    Q_PROPERTY(QString connectionText READ connectionText NOTIFY connectionChanged)
    Q_PROPERTY(bool anyAlarm READ anyAlarm NOTIFY alarmChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY searchingChanged)
    Q_PROPERTY(QString alarmSummary READ alarmSummary NOTIFY alarmChanged)
    Q_PROPERTY(QString roomName READ roomName CONSTANT)

public:
    MonitorBackend(StreamClient* stream, FrameProvider* frames, CareDirectory* dir,
                   QObject* parent = nullptr);
    // 웨어러블 낙상 등 외부에서 온 경보 (영상 서버 이벤트와 같은 경로)
    void raiseExternalAlarm(int ch, const QString& type, const QString& label);

    ChannelModel* channels() { return &channels_; }
    EventModel* events() { return &events_; }
    int connectedServers() const;
    int totalServers() const;
    QString connectionText() const;
    bool anyAlarm() const;
    bool searching() const { return searching_; }
    QString alarmSummary() const;   // 경보 토스트 문구
    QString roomName() const;       // QSettings room/name (기본 101호)

    // 경보 해제 — 화면 경보를 끄고 서버에 모자이크 복구(0x03)를 보낸다
    Q_INVOKABLE void clearAlarms();
    // 자연어 영상 검색 (channel -1 = 전체). 결과는 searchResult 시그널로
    Q_INVOKABLE bool search(int channel, const QString& query);
    // 서버 답변 → {hits:[{when,meta,url,context}], message}  (위젯 parseSearchReply 와 같은 규칙)
    Q_INVOKABLE QVariantMap parseSearch(const QString& text) const;
    // 화면 상태(네비·리소스 패널 접힘 등) — 위젯 앱과 같은 QSettings 키를 쓴다
    Q_INVOKABLE QVariant setting(const QString& key, const QVariant& fallback) const;
    Q_INVOKABLE void setSetting(const QString& key, const QVariant& value);
    // 타일 스냅샷 저장 경로 (홈/CHn_시각.png)
    Q_INVOKABLE QString snapshotPath(int channel) const;

signals:
    void connectionChanged();
    void alarmChanged();
    void searchingChanged();
    void searchResult(const QString& text);
    // 새 경보 — 화면은 이 채널을 스포트라이트로 키우고 타임라인에 표시한다
    void alarmRaised(int channel, qint64 atMs, const QString& markerColor);
    // 이벤트 로그 화면에 실시간 줄을 넣기 위한 신호 (type: FALL/EGRESS/VITAL…)
    void eventRaised(int channel, int roiId, double atMs, const QString& type);

private:
    void onFrame(int ch, const QImage& image, qint64 serverTs);
    void onConnection();
    void raiseAlarm(int ch, const QString& type, int roiId, quint64 ts, float nx, float ny);
    void checkStale();   // 소켓은 붙어 있는데 프레임이 끊긴 채널의 LIVE 끄기

    void refreshNames();
    void refreshZones(int ch);

    StreamClient* stream_;
    FrameProvider* frames_;
    CareDirectory* dir_;
    ChannelModel channels_;
    EventModel events_;
    QTimer staleTimer_;
    QTimer searchTimeout_;
    bool searching_ = false;
};

#endif // MONITORBACKEND_H
