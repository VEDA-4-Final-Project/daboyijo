#include "residentbackend.h"
#include "appbackend.h"
#include "caredirectory.h"
#include "streamclient.h"
#include "streamprotocol.h"

#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>

namespace {
struct LoggedField { const char* label; const char* column; };
const LoggedField kLoggedFields[] = {
    {"이름", "name"},           {"호실", "room"},
    {"카메라 채널", "camera_id"},
    {"웨어러블 ID", "wearable_id"}, {"위험도", "risk_level"},
    {"입원일", "admitted_at"},  {"퇴원 예정일", "discharge_due"},
    {"상태", "status"},         {"특이사항", "notes"},
};

QString severityOf(const QString& risk) {
    return risk == QStringLiteral("상") ? "critical" : risk == QStringLiteral("중") ? "medium" : "normal";
}
}  // namespace

ResidentBackend::ResidentBackend(CareDirectory* dir, StreamClient* stream, AppBackend* app, QObject* parent)
    : QObject(parent), dir_(dir), stream_(stream), app_(app) {}

void ResidentBackend::setFilter(const QString& f)
{
    if (f == filter_) return;
    filter_ = f;
    refresh();
}

void ResidentBackend::setSearch(const QString& s)
{
    if (s == search_) return;
    search_ = s;
    refresh();
}

void ResidentBackend::refresh()
{
    const QString trimmed = search_.trimmed();
    QString sql = QStringLiteral(
        "SELECT resident_id, name, room, bed, camera_id, wearable_id, risk_level, status "
        "FROM residents WHERE 1=1");
    if (filter_ == QStringLiteral("재원"))      sql += QStringLiteral(" AND status='재원'");
    else if (filter_ == QStringLiteral("퇴원")) sql += QStringLiteral(" AND status='퇴원'");
    if (!trimmed.isEmpty()) sql += QStringLiteral(" AND name LIKE ?");
    sql += QStringLiteral(" ORDER BY status DESC, camera_id, resident_id");

    residents_.clear();
    QSqlQuery q;
    q.prepare(sql);
    if (!trimmed.isEmpty()) q.addBindValue(QStringLiteral("%%1%").arg(trimmed));
    if (!q.exec()) {
        qDebug() << "입소자 목록 조회 실패:" << q.lastError().text();
    } else {
        while (q.next()) {
            const QString name = q.value(1).toString();
            const QString risk = q.value(6).toString();
            const bool active = q.value(7).toString() == QStringLiteral("재원");
            const QVariant cam = q.value(4);
            QVariantMap m;
            m["id"] = q.value(0).toInt();
            m["name"] = name.isEmpty() ? QStringLiteral("(이름 없음)") : name;
            m["meta"] = cam.isNull() ? QStringLiteral("채널 미지정")
                                     : QStringLiteral("채널 %1").arg(cam.toInt() + 1);
            m["severity"] = severityOf(risk);
            m["active"] = active;
            m["chip"] = active ? QStringLiteral("위험 %1").arg(risk.isEmpty() ? QStringLiteral("-") : risk)
                               : QStringLiteral("퇴원");
            residents_ << m;
        }
    }
    const QString scope = trimmed.isEmpty() ? QStringLiteral("%1 목록").arg(filter_)
                                            : QStringLiteral("검색 결과");
    countText_ = QStringLiteral("%1 · %2명").arg(scope).arg(residents_.size());

    int active = 0, high = 0, mid = 0, low = 0, cams = 0;
    QSqlQuery s;
    if (s.exec(QStringLiteral(
            "SELECT COUNT(*), SUM(risk_level='상'), SUM(risk_level='중'), SUM(risk_level='하'), "
            " COUNT(DISTINCT CASE WHEN camera_id BETWEEN 0 AND 3 THEN camera_id END) "
            "FROM residents WHERE status='재원'")) && s.next()) {
        active = s.value(0).toInt(); high = s.value(1).toInt();
        mid = s.value(2).toInt();    low = s.value(3).toInt(); cams = s.value(4).toInt();
    }
    summary_ = {{"active", QStringLiteral("%1명").arg(active)}, {"high", high}, {"mid", mid},
                {"low", low}, {"cams", QStringLiteral("%1/4").arg(cams)}};
    emit listChanged();
}

QVariantMap ResidentBackend::loadForm(int id) const
{
    QVariantMap f;
    QSqlQuery q;
    q.prepare(QStringLiteral(
        "SELECT name, room, camera_id, wearable_id, risk_level, admitted_at, discharge_due, status, notes "
        "FROM residents WHERE resident_id = ?"));
    q.addBindValue(id);
    if (!q.exec() || !q.next()) return f;
    f["name"] = q.value(0).toString();
    f["room"] = q.value(1).toString();
    f["camera"] = q.value(2).isNull() ? QString() : QString::number(q.value(2).toInt() + 1);
    f["wearable"] = q.value(3).isNull() ? QString() : q.value(3).toString();
    f["risk"] = q.value(4).toString();
    const QDate in = q.value(5).toDate(), due = q.value(6).toDate();
    f["admittedAt"] = (in.isValid() ? in : QDate::currentDate()).toString(Qt::ISODate);
    f["dischargeDue"] = (due.isValid() ? due : QDate::currentDate().addMonths(1)).toString(Qt::ISODate);
    f["status"] = q.value(7).toString();
    f["notes"] = q.value(8).toString();
    return f;
}

void ResidentBackend::openResident(int id)
{
    if (id < 0) {
        selected_ = -1;
        form_ = {{"name", ""}, {"room", ""}, {"camera", ""}, {"wearable", ""}, {"risk", "중"},
                 {"admittedAt", QDate::currentDate().toString(Qt::ISODate)},
                 {"dischargeDue", QDate::currentDate().addMonths(1).toString(Qt::ISODate)},
                 {"status", "재원"}, {"notes", ""}};
    } else {
        form_ = loadForm(id);
        if (form_.isEmpty()) return;
        selected_ = id;
    }
    loadAdmissions();
    emit selectionChanged();
}

void ResidentBackend::closeEditor()
{
    selected_ = 0;
    form_.clear();
    admissions_.clear();
    emit selectionChanged();
    emit admissionsChanged();
}

void ResidentBackend::loadAdmissions()
{
    admissions_.clear();
    if (selected_ > 0) {
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "SELECT a.admission_id, a.admitted_at, a.discharged_at, a.status, "
            " (SELECT COUNT(*) FROM resident_changes c WHERE c.admission_id = a.admission_id) AS chg "
            "FROM admissions a WHERE a.resident_id=? "
            "ORDER BY a.admitted_at DESC, a.admission_id DESC"));
        q.addBindValue(selected_);
        if (q.exec()) {
            while (q.next()) {
                QVariantMap m;
                m["id"] = q.value(0).toInt();
                m["in"] = q.value(1).toDate().toString("yyyy-MM-dd");
                m["out"] = q.value(2).isNull() ? QStringLiteral("—") : q.value(2).toDate().toString("yyyy-MM-dd");
                m["status"] = q.value(3).toString();
                m["changes"] = QStringLiteral("%1건").arg(q.value(4).toInt());
                admissions_ << m;
            }
        } else qDebug() << "입원 이력 조회 실패:" << q.lastError().text();
    }
    emit admissionsChanged();
}

QVariantList ResidentBackend::changeLog(int admissionId) const
{
    QVariantList out;
    QSqlQuery q;
    q.prepare(QStringLiteral(
        "SELECT changed_at, change_type, field_label, old_value, new_value, changed_by "
        "FROM resident_changes WHERE admission_id=? ORDER BY changed_at DESC, change_id DESC"));
    q.addBindValue(admissionId);
    if (!q.exec()) return out;
    auto shown = [](const QString& s) { return s.isEmpty() ? QStringLiteral("(없음)") : s; };
    while (q.next()) {
        QVariantMap m;
        m["at"] = q.value(0).toDateTime().toString("yyyy-MM-dd HH:mm:ss");
        m["type"] = q.value(1).toString();
        m["field"] = q.value(2).toString();
        m["change"] = QStringLiteral("[수정 전] %1   →   [수정 후] %2")
                          .arg(shown(q.value(3).toString()), shown(q.value(4).toString()));
        m["by"] = q.value(5).toString();
        out << m;
    }
    return out;
}

QMap<QString, QString> ResidentBackend::snapshot(int id) const
{
    QMap<QString, QString> m;
    QStringList cols;
    for (const auto& f : kLoggedFields) cols << QLatin1String(f.column);
    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT %1 FROM residents WHERE resident_id=?").arg(cols.join(',')));
    q.addBindValue(id);
    if (!q.exec() || !q.next()) return m;
    int i = 0;
    for (const auto& f : kLoggedFields) {
        const QVariant v = q.value(i++);
        QString t = v.isNull() ? QString() : v.toString();
        if (!v.isNull() && QLatin1String(f.column) == QLatin1String("camera_id"))
            t = QString::number(v.toInt() + 1);
        m.insert(QString::fromUtf8(f.label), t);
    }
    return m;
}

QMap<QString, QString> ResidentBackend::formSnapshot(const QVariantMap& f)
{
    QMap<QString, QString> m;
    m["이름"] = f["name"].toString().trimmed();
    m["호실"] = f["room"].toString().trimmed();
    m["카메라 채널"] = f["camera"].toString().trimmed();
    m["웨어러블 ID"] = f["wearable"].toString().trimmed();
    m["위험도"] = f["risk"].toString();
    m["입원일"] = f["admittedAt"].toString();
    m["퇴원 예정일"] = f["dischargeDue"].toString();
    m["상태"] = f["status"].toString();
    m["특이사항"] = f["notes"].toString();
    return m;
}

int ResidentBackend::currentAdmissionId(int residentId) const
{
    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT admission_id FROM admissions WHERE resident_id=? "
                             "ORDER BY admitted_at DESC, admission_id DESC LIMIT 1"));
    q.addBindValue(residentId);
    return (q.exec() && q.next()) ? q.value(0).toInt() : -1;
}

void ResidentBackend::logChanges(int residentId, int admissionId, const QMap<QString, QString>& before,
                                 const QMap<QString, QString>& after, const QString& type)
{
    QSqlQuery q;
    q.prepare(QStringLiteral(
        "INSERT INTO resident_changes (resident_id, admission_id, field_label, old_value, new_value, "
        " change_type, changed_by) VALUES (?,?,?,?,?,?,?)"));
    for (auto it = after.constBegin(); it != after.constEnd(); ++it) {
        const QString oldV = before.value(it.key());
        if (oldV == it.value()) continue;
        q.bindValue(0, residentId);
        q.bindValue(1, admissionId > 0 ? QVariant(admissionId) : QVariant());
        q.bindValue(2, it.key());
        q.bindValue(3, oldV);
        q.bindValue(4, it.value());
        q.bindValue(5, type);
        q.bindValue(6, app_ ? app_->userName() : QString());
        if (!q.exec()) qDebug() << "변경 로그 기록 실패:" << q.lastError().text();
    }
}

void ResidentBackend::afterChange()
{
    refresh();
    form_ = loadForm(selected_);
    loadAdmissions();
    emit selectionChanged();
    dir_->reload();      // 채널↔입소자 매핑을 DB 기준으로 다시 읽어 영상/바이탈 이름 갱신
}

QVariantMap ResidentBackend::save(const QVariantMap& f)
{
    if (f["name"].toString().trimmed().isEmpty()) return result(false, QStringLiteral("이름을 입력해주세요."));
    const QString camText = f["camera"].toString().trimmed();
    if (!camText.isEmpty()) {
        bool ok = false;
        const int n = camText.toInt(&ok);
        if (!ok || n < 1 || n > 4) return result(false, QStringLiteral("카메라 채널은 1~4 사이로 입력해주세요."));
    }
    auto orNull = [](const QString& s) { const QString t = s.trimmed(); return t.isEmpty() ? QVariant() : QVariant(t); };

    const bool isNew = selected_ < 0;
    const auto before = isNew ? QMap<QString, QString>() : snapshot(selected_);
    const QDate admitted = QDate::fromString(f["admittedAt"].toString(), Qt::ISODate);
    const QDate due = QDate::fromString(f["dischargeDue"].toString(), Qt::ISODate);

    QSqlQuery q;
    if (isNew)
        q.prepare(QStringLiteral("INSERT INTO residents (name, room, bed, camera_id, wearable_id, risk_level, "
                                 "admitted_at, discharge_due, status, notes) VALUES (?,?,?,?,?,?,?,?,?,?)"));
    else
        q.prepare(QStringLiteral("UPDATE residents SET name=?, room=?, bed=?, camera_id=?, wearable_id=?, "
                                 "risk_level=?, admitted_at=?, discharge_due=?, status=?, notes=? WHERE resident_id=?"));
    q.addBindValue(f["name"].toString().trimmed());
    q.addBindValue(f["room"].toString().trimmed());
    q.addBindValue(QStringLiteral(""));
    q.addBindValue(camText.isEmpty() ? QVariant() : QVariant(camText.toInt() - 1));
    q.addBindValue(orNull(f["wearable"].toString()));
    q.addBindValue(f["risk"].toString());
    q.addBindValue(admitted);
    q.addBindValue(due);
    q.addBindValue(f["status"].toString());
    q.addBindValue(f["notes"].toString());
    if (!isNew) q.addBindValue(selected_);
    if (!q.exec()) return result(false, q.lastError().text());

    if (isNew) {
        selected_ = q.lastInsertId().toInt();
        QSqlQuery a;
        a.prepare(QStringLiteral("INSERT INTO admissions (resident_id, admitted_at, discharge_due, status, room, bed) "
                                 "VALUES (?,?,?,'재원','','')"));
        a.addBindValue(selected_);
        a.addBindValue(admitted);
        a.addBindValue(due);
        if (!a.exec()) qDebug() << "입원 에피소드 생성 실패:" << a.lastError().text();
    }
    logChanges(selected_, currentAdmissionId(selected_), before, formSnapshot(f),
               isNew ? QStringLiteral("등록") : QStringLiteral("수정"));

    // 서버에 위험도 전달 (하:1 중:2 상:3) — 이 입소자에게 배정된 침대 단위, 없으면 채널 일괄
    const int cam = camText.isEmpty() ? -1 : camText.toInt() - 1;
    if (stream_ && cam >= 0 && cam < 4 && stream_->isChannelConnected(cam)) {
        const QString risk = f["risk"].toString();
        const int level = risk == QStringLiteral("상") ? 3 : risk == QStringLiteral("중") ? 2 : 1;
        QVector<int> beds;
        for (const auto& z : dir_->zones(cam))
            if (dir_->zoneResident(cam, z.id) == selected_) beds << z.id;
        if (beds.isEmpty()) beds << kRoiIdAll;
        for (int b : beds) stream_->sendRiskLevel(cam, b, level);
    }
    afterChange();
    return result(true, isNew ? QStringLiteral("신규 입소자가 등록되었습니다.")
                              : QStringLiteral("수정 내용이 저장되었습니다."));
}

QVariantMap ResidentBackend::discharge()
{
    if (selected_ <= 0) return result(false, QStringLiteral("퇴원 처리할 입소자를 먼저 선택해주세요."));
    QSqlQuery q;
    q.prepare(QStringLiteral("UPDATE residents SET status='퇴원' WHERE resident_id=?"));
    q.addBindValue(selected_);
    if (!q.exec()) return result(false, q.lastError().text());
    const int adm = currentAdmissionId(selected_);
    QSqlQuery a;
    a.prepare(QStringLiteral("UPDATE admissions SET discharged_at=CURDATE(), status='퇴원' WHERE admission_id=?"));
    a.addBindValue(adm);
    a.exec();
    logChanges(selected_, adm, {{"상태", "재원"}}, {{"상태", "퇴원"}}, QStringLiteral("퇴원"));
    afterChange();
    return result(true, QStringLiteral("퇴원 처리되었습니다."));
}

QVariantMap ResidentBackend::readmit()
{
    if (selected_ <= 0) return result(false, QStringLiteral("재입원 처리할 입소자를 먼저 선택해주세요."));
    QSqlQuery a;
    a.prepare(QStringLiteral("INSERT INTO admissions (resident_id, admitted_at, status, room, bed) "
                             "VALUES (?, CURDATE(), '재원', '', '')"));
    a.addBindValue(selected_);
    if (!a.exec()) return result(false, a.lastError().text());
    QSqlQuery r;
    r.prepare(QStringLiteral("UPDATE residents SET status='재원', admitted_at=CURDATE() WHERE resident_id=?"));
    r.addBindValue(selected_);
    r.exec();
    logChanges(selected_, currentAdmissionId(selected_), {{"상태", "퇴원"}}, {{"상태", "재원"}}, QStringLiteral("재입원"));
    afterChange();
    return result(true, QStringLiteral("재입원 처리되었습니다."));
}
