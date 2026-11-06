#ifndef FRAMEPROVIDER_H
#define FRAMEPROVIDER_H

// QML <Image source: "image://frames/<채널>/<순번>"> 이 요청하면 그 채널의 최신
// 프레임을 돌려주는 이미지 공급자. 순번은 QML이 캐시된 옛 이미지를 다시 쓰지 않게
// 하려고 붙이는 값일 뿐, 공급자는 항상 "지금 최신" 1장을 준다.
//
// requestImage()는 QML 렌더 스레드에서 불릴 수 있어 mutex로 보호한다.

#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>

class FrameProvider : public QQuickImageProvider
{
public:
    static constexpr int kNumChannels = 4;

    FrameProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    void setFrame(int ch, const QImage& img);
    void clear(int ch);
    QImage frame(int ch);   // 영상 타일(VideoItem)이 직접 그릴 때

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

private:
    QMutex mutex_;
    QImage frames_[kNumChannels];
};

#endif // FRAMEPROVIDER_H
