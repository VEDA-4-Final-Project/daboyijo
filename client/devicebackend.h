#ifndef DEVICEBACKEND_H
#define DEVICEBACKEND_H

// 장치 설정 — 위젯 mainwindow_camera.cpp / mainwindow_roi.cpp 의 로직.
//   카메라 연결/해제 · ONVIF 검색 · 영상 조정 · 초점 · 침대(ROI) 편집·입소자 연결 ·
//   호실 자리 · 알림 노드(밝기/음량/테스트) 를 담당한다. 화면은 QML.
#include <QHash>
#include <QObject>
#include <QPolygonF>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class CareDirectory;
class StreamClient;
class VitalsBackend;
class MqttQtManager;
class QUdpSocket;

class DeviceBackend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList channels READ channels NOTIFY channelsChanged)
    Q_PROPERTY(QVariantList found READ found NOTIFY foundChanged)
    Q_PROPERTY(QString searchStatus READ searchStatus NOTIFY foundChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY foundChanged)
    Q_PROPERTY(QString savedIp READ savedIp CONSTANT)
    Q_PROPERTY(QString savedUser READ savedUser CONSTANT)
    Q_PROPERTY(QStringList rooms READ rooms NOTIFY roomsChanged)
    Q_PROPERTY(int selectedRoom READ selectedRoom WRITE setSelectedRoom NOTIFY roomsChanged)
    Q_PROPERTY(QStringList alertNodes READ alertNodes CONSTANT)
    Q_PROPERTY(QString alertStatus READ alertStatus NOTIFY alertStatusChanged)   // "" / online / offline
    Q_PROPERTY(QString lastApplied READ lastApplied NOTIFY alertStatusChanged)
public:
    DeviceBackend(CareDirectory* dir, StreamClient* stream, VitalsBackend* vitals, QObject* parent = nullptr);

    QVariantList channels() const { return channels_; }
    QVariantList found() const { return found_; }
    QString searchStatus() const { return searchStatus_; }
    bool searching() const { return searching_; }
    QString savedIp() const;
    QString savedUser() const;
    QStringList rooms() const;
    int selectedRoom() const { return selectedRoom_; }
    void setSelectedRoom(int r);
    QStringList alertNodes() const { return {QStringLiteral("alarm_rpi_01")}; }
    QString alertStatus() const;
    QString lastApplied() const { return lastApplied_; }

    // 카메라 연결 ─────────────────────────────────────
    Q_INVOKABLE QVariantMap connectCameras(const QString& ip, const QString& user, const QString& password);
    Q_INVOKABLE bool anyCameraActive() const;
    Q_INVOKABLE QString clearCameras();
    Q_INVOKABLE void searchCameras(const QString& typedIp);
    // 영상 조정 ────────────────────────────────────────
    Q_INVOKABLE QVariantMap imageParams(int ch) const;
    Q_INVOKABLE QVariantMap applyImage(int ch, int b, int c, int s);
    Q_INVOKABLE QVariantMap focusAll(int ch);
    Q_INVOKABLE QVariantMap focusAt(int ch, double nx, double ny);
    // 침대(ROI) ────────────────────────────────────────
    Q_INVOKABLE QVariantList zones(int ch) const;                // VideoItem.zones 형식
    Q_INVOKABLE QVariantList beds(int ch) const;                 // [{id, residentId, color}]
    Q_INVOKABLE QVariantList residentChoices(int ch) const;      // [{id, name}]
    Q_INVOKABLE QVariantMap addZone(int ch, int roiId, const QVariantList& pts);
    Q_INVOKABLE QVariantMap removeBed(int ch, int roiId);        // roiId<0 = 전부
    Q_INVOKABLE QVariantMap bindBed(int ch, int roiId, int residentId);
    Q_INVOKABLE QString removeBedPrompt(int ch, int roiId) const; // 확인창 문구 ("" = 지울 게 없음)
    // 호실 ────────────────────────────────────────────
    Q_INVOKABLE QString suggestRoom() const;
    Q_INVOKABLE QString addRoom(const QString& name);            // 오류 문구 or ""
    Q_INVOKABLE void removeRoom(int idx);
    // 알림 노드 ─────────────────────────────────────────
    Q_INVOKABLE QVariantMap alertSettings(const QString& node) const;
    Q_INVOKABLE bool testAlert(const QString& node, int bright100, int vol);
    Q_INVOKABLE bool applyAlert(const QString& node, int bright100, int vol);
    Q_INVOKABLE QString alertTestText() const;

signals:
    void channelsChanged();
    void foundChanged();
    void roomsChanged();
    void alertStatusChanged();

private:
    void refreshChannels();
    void persistMask();
    void onDatagrams();
    void resendForServer(int idx);
    static QVariantMap res(bool ok, const QString& msg) { return {{"ok", ok}, {"message", msg}}; }
    static QString rtsp(const QString& ip, const QString& user, const QString& pw, int port, const QString& profile, int ch);
    MqttQtManager* mqtt() const;

    CareDirectory* dir_;
    StreamClient* stream_;
    VitalsBackend* vitals_;
    bool active_[4] = {};
    QString lastUrl_[4];
    QString ip_[4];
    QVariantList channels_;
    QVariantList found_;
    QString searchStatus_;
    bool searching_ = false;
    QUdpSocket* sock_ = nullptr;
    QSet<QString> seen_;
    QList<quint32> sweep_;
    QTimer sweepTimer_;
    QTimer poll_;
    int selectedRoom_ = 0;
    QHash<QString, bool> online_;
    QString lastApplied_ = QStringLiteral("마지막 적용 --:--:--");
};

#endif
