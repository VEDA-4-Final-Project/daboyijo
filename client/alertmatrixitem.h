#ifndef ALERTMATRIXITEM_H
#define ALERTMATRIXITEM_H

// LED 전광판(64×32) 미리보기 — alertmatrixpreview.cpp 를 QQuickPaintedItem 으로 옮긴 것.
// 알림 노드와 같은 글꼴(font16.h)로 그려서 실제 전광판과 같은 모양이 나온다.
#include <QColor>
#include <QQuickPaintedItem>
#include <QTimer>

class AlertMatrixItem : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(int brightness READ brightness WRITE setBrightness NOTIFY changed)   // 0~255
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY changed)
public:
    explicit AlertMatrixItem(QQuickItem* parent = nullptr);
    int brightness() const { return brightness_; }
    void setBrightness(int v);
    QString text() const { return text_; }
    void setText(const QString& t);
    void paint(QPainter* p) override;
signals:
    void changed();
private:
    void advance();
    int measureText(const QString& s) const;
    QTimer timer_;
    QString text_;
    QColor accent_{255, 140, 0};
    int brightness_ = 180, offset_ = 0, span_ = 0;
};

#endif
