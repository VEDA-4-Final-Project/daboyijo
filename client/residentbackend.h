#ifndef RESIDENTBACKEND_H
#define RESIDENTBACKEND_H

// 입소자 관리 로직 — 위젯 앱 mainwindow_residents.cpp의 DB 처리를 그대로 옮긴 것.
// 화면(QML)은 목록/폼을 그리기만 하고, 조회·저장·퇴원·재입원·변경이력은 전부 여기서.

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QMap>

class CareDirectory;
class StreamClient;
class AppBackend;

class ResidentBackend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY listChanged)   // 재원/전체/퇴원
    Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY listChanged)
    Q_PROPERTY(QVariantList residents READ residents NOTIFY listChanged)
    Q_PROPERTY(QString countText READ countText NOTIFY listChanged)
    Q_PROPERTY(QVariantMap summary READ summary NOTIFY listChanged)
    Q_PROPERTY(int selectedId READ selectedId NOTIFY selectionChanged)         // -1 신규, 0 선택없음
    Q_PROPERTY(QVariantMap form READ form NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList admissions READ admissions NOTIFY admissionsChanged)
public:
    ResidentBackend(CareDirectory* dir, StreamClient* stream, AppBackend* app, QObject* parent = nullptr);

    QString filter() const { return filter_; }
    void setFilter(const QString& f);
    QString search() const { return search_; }
    void setSearch(const QString& s);
    QVariantList residents() const { return residents_; }
    QString countText() const { return countText_; }
    QVariantMap summary() const { return summary_; }
    int selectedId() const { return selected_; }
    QVariantMap form() const { return form_; }
    QVariantList admissions() const { return admissions_; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void openResident(int id);      // -1 = 신규 등록
    Q_INVOKABLE void closeEditor();
    // 폼 값(name, room, camera, wearable, risk, admittedAt, dischargeDue, status, notes) 저장
    Q_INVOKABLE QVariantMap save(const QVariantMap& f);
    Q_INVOKABLE QVariantMap discharge();
    Q_INVOKABLE QVariantMap readmit();
    Q_INVOKABLE QVariantList changeLog(int admissionId) const;

signals:
    void listChanged();
    void selectionChanged();
    void admissionsChanged();

private:
    QVariantMap loadForm(int id) const;
    QMap<QString, QString> snapshot(int id) const;
    static QMap<QString, QString> formSnapshot(const QVariantMap& f);
    int currentAdmissionId(int residentId) const;
    void logChanges(int residentId, int admissionId, const QMap<QString, QString>& before,
                    const QMap<QString, QString>& after, const QString& type);
    void loadAdmissions();
    void afterChange();
    static QVariantMap result(bool ok, const QString& msg) { return {{"ok", ok}, {"message", msg}}; }

    CareDirectory* dir_;
    StreamClient* stream_;
    AppBackend* app_;
    QString filter_ = QStringLiteral("재원");
    QString search_;
    QVariantList residents_;
    QString countText_;
    QVariantMap summary_;
    int selected_ = 0;
    QVariantMap form_;
    QVariantList admissions_;
};

#endif
