#ifndef VITALSBACKEND_H
#define VITALSBACKEND_H

// ═══════════════════════════════════════════════════════════
//  VitalsBackend — MQTT 웨어러블 수신 + 바이탈 카드 모델 (C++ 로직)
// ═══════════════════════════════════════════════════════════
// 위젯 앱 mainwindow_vitals.cpp(onWearableData/updateVitals/rebuildVitalCards)의 로직을
// 그대로 옮겼다. QML은 모델이 주는 값(숫자·등급·배지 문구)만 그린다.
//   · 브로커 접속·재접속은 MqttQtManager 가 맡는다 (위젯 앱과 같은 클래스)
//   · 기기 id → 입소자 매핑은 CareDirectory(residents.wearable_id)
//   · 웨어러블 낙상 → MonitorBackend 경보 (영상 서버 이벤트와 같은 경로)

#include <QAbstractListModel>
#include <QHash>
#include <QTimer>
#include <QVariantList>
#include <QVector>

#include "veda_messages.hpp"

class MqttQtManager;
class CareDirectory;
class MonitorBackend;

class VitalModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role { NameRole = Qt::UserRole + 1, BedRole, Spo2Role, HrRole, SeverityRole, BadgeRole,
                StaleRole, HistoryRole, ColorRole };
    struct Card {
        int key = 0;            // residentId, 없으면 -(채널+1)
        QString name, bed;
        QString spo2 = QStringLiteral("--"), hr = QStringLiteral("--");
        QString severity;       // "" = 무신호
        QString badge = QStringLiteral("○ 대기");
        bool stale = true;
        QVariantList history;
        QString color;          // 스파크라인 색
    };
    explicit VitalModel(QObject* p = nullptr) : QAbstractListModel(p) {}
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : cards_.size(); }
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    QVector<Card>& cards() { return cards_; }
    void reset(const QVector<Card>& c) { beginResetModel(); cards_ = c; endResetModel(); }
    void touch(int row) { const auto i = index(row); emit dataChanged(i, i); }
private:
    QVector<Card> cards_;
};

class VitalsBackend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(VitalModel* cards READ cards CONSTANT)
    Q_PROPERTY(bool brokerConnected READ brokerConnected NOTIFY brokerChanged)
public:
    VitalsBackend(CareDirectory* dir, MonitorBackend* monitor, QObject* parent = nullptr);

    VitalModel* cards() { return &model_; }
    bool brokerConnected() const;
    MqttQtManager* mqtt() const { return mqtt_; }

    void start();   // 브로커 접속 시작

signals:
    void brokerChanged();
    void nodeOnlineChanged(const QString& node, bool online);

private:
    struct Sample { bool received = false; int hr = 0, spo2 = 0; qint64 at = 0; };
    void rebuildCards();
    void refreshCards();
    void onWearable(const WearableData& d);

    CareDirectory* dir_;
    MonitorBackend* monitor_;
    MqttQtManager* mqtt_;
    VitalModel model_;
    QHash<int, Sample> samples_;
    QHash<int, QVector<double>> history_;
    QTimer tick_;
};

#endif
