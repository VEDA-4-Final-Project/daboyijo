#ifndef CAREDIRECTORY_H
#define CAREDIRECTORY_H

// ═══════════════════════════════════════════════════════════
//  CareDirectory — "누가 어느 채널·침대에 있나" 의 단일 출처 (C++ 로직)
// ═══════════════════════════════════════════════════════════
// 위젯 앱에서는 MainWindow가 loadPatientsFromDb() / loadRoiZonesFromDb() /
// eventWhoLabel() / channelDisplayName() 으로 나눠 들고 있던 입소자·침대 정보를
// 한 클래스로 모았다. QML 화면(관제·바이탈·이벤트)은 이 객체가 만든 문구만 쓴다.

#include <QHash>
#include <QMap>
#include <QObject>
#include <QPolygonF>
#include <QString>
#include <QVector>

class CareDirectory : public QObject
{
    Q_OBJECT
public:
    static constexpr int kNumChannels = 4;
    static constexpr int kMaxRoiZones = 8;

    struct Resident {
        QString name;
        QString bed;       // "101호 · 채널 N"
        QString room;
        int     channel = -1;
    };
    struct Zone {
        int      id = 0;
        QPolygonF poly;    // 0~1 정규화
    };

    explicit CareDirectory(QObject* parent = nullptr);

    // DB → 메모리 (입소자·웨어러블 매핑·침대 ROI). DB가 없으면 비어 있는 상태 그대로.
    void reload();

    static QString roomName();                // QSettings room/name (기본 101호)
    QString channelName(int ch) const;        // "CH1 · 김복순" / "CH1"
    QString zoneResidentName(int ch, int roiId) const;
    QString whoLabel(int ch, int roiId) const;      // 배너용 "침대 2 김복순"
    QString placeLabel(int ch, int roiId) const;    // 이벤트 목록용

    const QVector<int>& residentsOn(int ch) const { return byChannel_[ch]; }
    Resident resident(int id) const { return residents_.value(id); }
    int residentForWearable(const QString& id) const { return wearable_.value(id, -1); }
    QVector<int> residentIds() const;               // 채널 순 정렬
    const QVector<Zone>& zones(int ch) const { return zones_[ch]; }
    QString zoneLabel(int ch, int roiId) const;     // "침대 1 · 김복순"

    // 침대 ↔ 입소자 지정 / 도형 갱신 (장치 설정 → ROI)
    void setZone(int ch, int roiId, const QPolygonF& poly);
    void removeZone(int ch, int roiId);
    void bindZone(int ch, int roiId, int residentId);
    int  zoneResident(int ch, int roiId) const { return zoneResident_[ch].value(roiId, 0); }

signals:
    void changed();        // 입소자/매핑이 다시 읽혔다
    void zonesChanged(int channel);

private:
    QHash<int, Resident> residents_;
    QVector<int> byChannel_[kNumChannels];
    QHash<QString, int> wearable_;
    QVector<Zone> zones_[kNumChannels];
    QMap<int, int> zoneResident_[kNumChannels];
};

#endif // CAREDIRECTORY_H
