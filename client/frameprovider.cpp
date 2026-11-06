#include "frameprovider.h"

#include <QMutexLocker>

void FrameProvider::setFrame(int ch, const QImage& img)
{
    if (ch < 0 || ch >= kNumChannels) return;
    QMutexLocker lock(&mutex_);
    frames_[ch] = img;   // QImage는 암시적 공유라 복사 비용이 거의 없다
}

void FrameProvider::clear(int ch)
{
    if (ch < 0 || ch >= kNumChannels) return;
    QMutexLocker lock(&mutex_);
    frames_[ch] = QImage();
}

QImage FrameProvider::frame(int ch)
{
    if (ch < 0 || ch >= kNumChannels) return {};
    QMutexLocker lock(&mutex_);
    return frames_[ch];
}

QImage FrameProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize)
{
    // id = "<채널>/<순번>"
    const int ch = id.section(QLatin1Char('/'), 0, 0).toInt();
    QImage img;
    {
        QMutexLocker lock(&mutex_);
        if (ch >= 0 && ch < kNumChannels) img = frames_[ch];
    }
    if (img.isNull()) {
        img = QImage(16, 9, QImage::Format_RGB32);
        img.fill(Qt::black);
    }
    if (size) *size = img.size();
    if (requestedSize.isValid() && !requestedSize.isEmpty())
        return img.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return img;
}
