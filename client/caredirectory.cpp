#include "caredirectory.h"

#include <QDebug>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <algorithm>

CareDirectory::CareDirectory(QObject* parent) : QObject(parent) {}

QString CareDirectory::roomName()
{
    return QSettings().value(QStringLiteral("room/name"), QStringLiteral("101호")).toString();
}

void CareDirectory::reload()
{
    residents_.clear();
    wearable_.clear();
    for (int ch = 0; ch < kNumChannels; ++ch) { byChannel_[ch].clear(); zones_[ch].clear(); zoneResident_[ch].clear(); }

    if (QSqlDatabase::database().isOpen()) {
        const QString room = roomName();
        QSqlQuery q;
        if (q.exec(QStringLiteral(
                "SELECT resident_id, camera_id, name, room, wearable_id FROM residents "
                "WHERE status='재원' AND camera_id BETWEEN 0 AND 3 "
                "ORDER BY camera_id, resident_id"))) {
            while (q.next()) {
                const int rid = q.value(0).toInt();
                const int ch = q.value(1).toInt();
                if (ch < 0 || ch >= kNumChannels) continue;
                Resident r;
                r.name = q.value(2).toString();
                r.room = q.value(3).toString();
                r.bed = QStringLiteral("%1 · 채널 %2").arg(room).arg(ch + 1);
                r.channel = ch;
                residents_.insert(rid, r);
                byChannel_[ch].append(rid);
                // 브로커는 기기 id로만 알려주므로 등록이 빠지면 그 값은 주인을 몰라 버려진다
                const QString w = q.value(4).toString().trimmed();
                if (!w.isEmpty()) wearable_.insert(w, rid);
            }
        } else {
            qDebug() << "채널 입소자 조회 실패:" << q.lastError().text();
        }

        QSqlQuery z;
        if (z.exec(QStringLiteral("SELECT camera_id, roi_id, resident_id, roi_points FROM roi_zones "
                                  "ORDER BY camera_id, roi_id"))) {
            while (z.next()) {
                const int ch = z.value(0).toInt();
                const int id = z.value(1).toInt();
                if (ch < 0 || ch >= kNumChannels || id < 0 || id >= kMaxRoiZones) continue;
                const int rid = z.value(2).isNull() ? 0 : z.value(2).toInt();
                if (rid > 0) zoneResident_[ch][id] = rid;
                // roi_points: 서버가 쓴 [[x,y],...] JSON — 숫자만 순서대로 긁어 둘씩 묶는다
                const QString json = z.value(3).toString();
                QVector<double> nums;
                for (int i = 0; i < json.size();) {
                    const QChar c = json[i];
                    if (c.isDigit() || c == QLatin1Char('-') || c == QLatin1Char('.')) {
                        int j = i;
                        while (j < json.size() && (json[j].isDigit() || json[j] == QLatin1Char('-')
                                                   || json[j] == QLatin1Char('.') || json[j] == QLatin1Char('e')))
                            ++j;
                        nums.push_back(json.mid(i, j - i).toDouble());
                        i = j;
                    } else {
                        ++i;
                    }
                }
                Zone zone;
                zone.id = id;
                for (int i = 0; i + 1 < nums.size(); i += 2) zone.poly << QPointF(nums[i], nums[i + 1]);
                if (zone.poly.size() >= 3) zones_[ch].push_back(zone);
            }
        } else {
            qDebug() << "침대 ROI 조회 실패:" << z.lastError().text();
        }
    }
    emit changed();
    for (int ch = 0; ch < kNumChannels; ++ch) emit zonesChanged(ch);
}

QString CareDirectory::channelName(int ch) const
{
    const QString tag = QStringLiteral("CH%1").arg(ch + 1);
    if (ch < 0 || ch >= kNumChannels) return tag;
    QStringList names;
    for (int rid : byChannel_[ch]) {
        const QString n = residents_.value(rid).name;
        if (!n.isEmpty()) names << n;
    }
    if (names.isEmpty()) return tag;
    if (names.size() <= 2) return QStringLiteral("%1 · %2").arg(tag, names.join(QStringLiteral(", ")));
    return QStringLiteral("%1 · %2 외 %3명").arg(tag, names.mid(0, 2).join(QStringLiteral(", "))).arg(names.size() - 2);
}

QString CareDirectory::zoneResidentName(int ch, int roiId) const
{
    if (ch < 0 || ch >= kNumChannels) return QStringLiteral("미지정");
    const int rid = zoneResident_[ch].value(roiId, 0);
    if (rid <= 0) return QStringLiteral("미지정");
    auto it = residents_.constFind(rid);
    return it == residents_.constEnd() || it->name.isEmpty() ? QStringLiteral("입소자 %1").arg(rid) : it->name;
}

QString CareDirectory::whoLabel(int ch, int roiId) const
{
    // 침대를 못 밝혔으면 알 수 있는 사실(채널)만 — 추정한 이름은 붙이지 않는다
    if (roiId < 0) return QStringLiteral("%1 · 채널 %2").arg(roomName()).arg(ch + 1);
    const QString name = zoneResidentName(ch, roiId);
    return name == QStringLiteral("미지정") ? QStringLiteral("침대 %1").arg(roiId + 1)
                                           : QStringLiteral("침대 %1 %2").arg(roiId + 1).arg(name);
}

QString CareDirectory::placeLabel(int ch, int roiId) const
{
    if (ch < 0 || ch >= kNumChannels) return {};
    const QString base = QStringLiteral("%1 · 채널 %2").arg(roomName()).arg(ch + 1);
    return roiId < 0 ? base : QStringLiteral("%1 · 침대 %2").arg(base).arg(roiId + 1);
}

QString CareDirectory::zoneLabel(int ch, int roiId) const
{
    return QStringLiteral("침대 %1 · %2").arg(roiId + 1).arg(zoneResidentName(ch, roiId));
}

QVector<int> CareDirectory::residentIds() const
{
    QVector<int> ids;
    for (int ch = 0; ch < kNumChannels; ++ch) ids += byChannel_[ch];
    return ids;
}

void CareDirectory::setZone(int ch, int roiId, const QPolygonF& poly)
{
    if (ch < 0 || ch >= kNumChannels) return;
    for (auto& z : zones_[ch])
        if (z.id == roiId) { z.poly = poly; emit zonesChanged(ch); return; }
    Zone z; z.id = roiId; z.poly = poly;
    zones_[ch].push_back(z);
    emit zonesChanged(ch);
}

void CareDirectory::removeZone(int ch, int roiId)
{
    if (ch < 0 || ch >= kNumChannels) return;
    zones_[ch].erase(std::remove_if(zones_[ch].begin(), zones_[ch].end(),
                                    [roiId](const Zone& z) { return z.id == roiId; }), zones_[ch].end());
    zoneResident_[ch].remove(roiId);
    emit zonesChanged(ch);
}

void CareDirectory::bindZone(int ch, int roiId, int residentId)
{
    if (ch < 0 || ch >= kNumChannels) return;
    if (residentId > 0) zoneResident_[ch][roiId] = residentId;
    else zoneResident_[ch].remove(roiId);
    emit zonesChanged(ch);
}
