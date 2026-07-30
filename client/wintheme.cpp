#include "wintheme.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <dwmapi.h>
#  include <QWidget>

void enableDarkTitleBar(QWidget* w) {
    if (!w) return;
    HWND hwnd = reinterpret_cast<HWND>(w->winId());  // winId()가 네이티브 창을 생성
    BOOL dark = TRUE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE = 20 (Win10 1903+/11), 이전(1809)엔 19.
    // 두 값을 모두 시도해 버전 차이를 흡수한다.
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
}

#else  // 비-Windows: no-op
void enableDarkTitleBar(QWidget*) {}
#endif
