#ifndef EVENTLOGBACKEND_H
#define EVENTLOGBACKEND_H

// 이벤트 기록 — 위젯 mainwindow_eventlog.cpp(원장 조회·확인 처리·클립·NVR 이어보기)의 로직.
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QDate>

class CareDirectory;
class AppBackend;
class QNetworkAccessManager;

class EventLogBackend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    Q_PROPERTY(QString countText READ countText NOTIFY rowsChanged)
    Q_PROPERTY(bool hasUnconfirmed READ hasUnconfirmed NOTIFY rowsChanged)
    Q_PROPERTY(bool truncated READ truncated NOTIFY rowsChanged)
    Q_PROPERTY(bool dbOpen READ dbOpen NOTIFY rowsChanged)
    Q_PROPERTY(QString dateFrom READ dateFrom WRITE setDateFrom NOTIFY filterChanged)
    Q_PROPERTY(QString dateTo READ dateTo WRITE setDateTo NOTIFY filterChanged)
    Q_PROPERTY(int channel READ channel WRITE setChannel NOTIFY filterChanged)        // -1 전체
    Q_PROPERTY(QString eventType READ eventType WRITE setEventType NOTIFY filterChanged)
    Q_PROPERTY(QString confirmed READ confirmed WRITE setConfirmed NOTIFY filterChanged)
    Q_PROPERTY(QString roomName READ roomName CONSTANT)
public:
    EventLogBackend(CareDirectory* dir, AppBackend* app, QObject* parent = nullptr);

    QVariantList rows() const { return rows_; }
    QString countText() const { return countText_; }
    bool hasUnconfirmed() const { return unconfirmed_ > 0; }
    bool truncated() const { return truncated_; }
    bool dbOpen() const;
    QString dateFrom() const { return from_.toString(Qt::ISODate); }
    QString dateTo() const { return to_.toString(Qt::ISODate); }
    void setDateFrom(const QString& s);
    void setDateTo(const QString& s);
    int channel() const { return channel_; }
    void setChannel(int c) { if (c == channel_) return; channel_ = c; emit filterChanged(); reload(); }
    QString eventType() const { return type_; }
    void setEventType(const QString& t) { if (t == type_) return; type_ = t; emit filterChanged(); reload(); }
    QString confirmed() const { return conf_; }
    void setConfirmed(const QString& t) { if (t == conf_) return; conf_ = t; emit filterChanged(); reload(); }
    static QString roomName();

    Q_INVOKABLE void reload();
    Q_INVOKABLE void preset(int days);     // 0=오늘, 6=7일, 29=30일
    Q_INVOKABLE void resetFilters();
    Q_INVOKABLE void refreshNvr();         // NVR 세그먼트 목록 수집
    // 행을 눌렀을 때: 확인 처리(클립이 있을 때만) 후 재생 정보를 돌려준다
    Q_INVOKABLE QVariantMap activate(int row);
    // NVR 이어보기: {ok, url, seekMs, message}
    Q_INVOKABLE QVariantMap nvrContext(int channel, double timestampMs) const;
    // 관제 타임라인용 — 채널의 녹화 구간 [[s,e],...](연속은 합침) / 이벤트 마커 / 그 시각을 담은 세그먼트
    Q_INVOKABLE QVariantList spans(int channel) const;
    Q_INVOKABLE QVariantList markers(int channel) const;
    Q_INVOKABLE QVariantMap segmentAt(int channel, double ms) const;
    // 클립 저장(비동기). path는 로컬 파일 경로 — 끝나면 downloadFinished
    Q_INVOKABLE void download(const QString& url, const QString& path);
    Q_INVOKABLE QString suggestedName(const QString& url) const;
    // 실시간으로 들어온 이벤트(서버 알람)를 목록 맨 위에 끼워 넣는다
    Q_INVOKABLE void appendLive(int channel, int roiId, double occurredMs, const QString& typeCode, const QString& source);

signals:
    void rowsChanged();
    void nvrChanged();
    void filterChanged();
    void downloadFinished(bool ok, const QString& message);

private:
    struct NvrSeg { int channel; qint64 startMs; QString url; };
    void recount();
    static QString typeLabel(const QString& code);
    static QString sourceLabel(const QString& code);

    CareDirectory* dir_;
    AppBackend* app_;
    QNetworkAccessManager* net_;
    QVariantList rows_;
    QString countText_;
    int unconfirmed_ = 0;
    bool truncated_ = false;
    QDate from_, to_;
    int channel_ = -1;
    QString type_ = QStringLiteral("전체 이벤트");
    QString conf_ = QStringLiteral("전체");
    QVector<NvrSeg> nvr_;
};

#endif
