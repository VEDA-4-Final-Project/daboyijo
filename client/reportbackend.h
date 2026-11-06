#ifndef REPORTBACKEND_H
#define REPORTBACKEND_H

// 일일 리포트 — 위젯 mainwindow_report.cpp 의 집계 SQL·AI 요약·PDF 내보내기를 그대로 옮긴 것.
#include <QDate>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

class QNetworkAccessManager;

class ReportBackend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString date READ date WRITE setDate NOTIFY dateChanged)
    Q_PROPERTY(QString dateLabel READ dateLabel NOTIFY dateChanged)
    Q_PROPERTY(QVariantList residents READ residents NOTIFY residentsChanged)
    Q_PROPERTY(int residentId READ residentId WRITE setResidentId NOTIFY metricsChanged)
    Q_PROPERTY(QString meta READ meta NOTIFY metricsChanged)
    Q_PROPERTY(QVariantMap tiles READ tiles NOTIFY metricsChanged)
    Q_PROPERTY(QVariantList hours READ hours NOTIFY metricsChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY summaryChanged)
    Q_PROPERTY(bool summaryBusy READ summaryBusy NOTIFY summaryChanged)
    Q_PROPERTY(bool hasSummary READ hasSummary NOTIFY summaryChanged)
public:
    explicit ReportBackend(QObject* parent = nullptr);

    QString date() const { return date_.toString(Qt::ISODate); }
    void setDate(const QString& s);
    QString dateLabel() const;
    QVariantList residents() const { return residents_; }
    int residentId() const { return rid_; }
    void setResidentId(int id);
    QString meta() const { return m_.residentMeta; }
    QVariantMap tiles() const { return tiles_; }
    QVariantList hours() const { return hours_; }
    QString summary() const { return summary_; }
    bool summaryBusy() const { return busy_; }
    bool hasSummary() const { return hasSummary_; }

    Q_INVOKABLE void reloadResidents();
    Q_INVOKABLE void today();
    Q_INVOKABLE void requestSummary();
    // PDF 저장(경로는 file:/// URL 또는 로컬 경로). {ok, message}
    Q_INVOKABLE QVariantMap exportPdf(const QString& path);
    Q_INVOKABLE QString suggestedPdfName() const;
    Q_INVOKABLE QString maxDate() const { return QDate::currentDate().toString(Qt::ISODate); }

signals:
    void dateChanged();
    void residentsChanged();
    void metricsChanged();
    void summaryChanged();
    void notice(const QString& title, const QString& message);

private:
    struct Metrics {
        bool valid = false;
        QString residentName, residentMeta;
        int lyingSec = 0, lyingCount = 0, steps = 0, activeMin = 0, careSec = 0, careCount = 0, eventTotal = 0;
        QString careLast, eventDetail;
    };
    void update();
    void loadCachedSummary();
    void setSummary(const QString& t);
    static QString key();
    QImage renderChart() const;

    QNetworkAccessManager* net_;
    QDate date_;
    int rid_ = -1;
    QVariantList residents_;
    Metrics m_;
    QVariantMap tiles_;
    QVariantList hours_;
    QVector<int> stepsByHour_;
    QString summary_;
    bool hasSummary_ = false;
    bool busy_ = false;
};

#endif
