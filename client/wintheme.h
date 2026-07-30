#ifndef WINTHEME_H
#define WINTHEME_H

class QWidget;

// 창의 네이티브 타이틀바를 다크로 칠한다 (Windows 10 1809+/11).
// Windows가 아니면 아무것도 하지 않는다(no-op).
void enableDarkTitleBar(QWidget* w);

#endif  // WINTHEME_H
