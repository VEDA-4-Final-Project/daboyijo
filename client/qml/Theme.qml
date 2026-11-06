pragma Singleton
import QtQuick
import Daboyijo 1.0

// 위젯 앱 theme.h 의 kDark / kLight 와 같은 값. App.dark 가 바뀌면 전 화면 색이 같이 바뀐다.
// (글꼴은 main_qml.cpp 가 위젯 QSS와 같은 13px 기본 글꼴을 앱 전체에 깐다)
QtObject {
    readonly property bool dark: App.dark
    readonly property color bgDeep:    dark ? "#0E141B" : "#F4F7FA"
    readonly property color panel:     dark ? "#151D26" : "#FFFFFF"
    readonly property color card:      dark ? "#1C2733" : "#F0F4F8"
    readonly property color border:    dark ? "#2A3742" : "#DCE4EC"
    readonly property color text:      dark ? "#E6EDF3" : "#1E2A32"
    readonly property color sub:       dark ? "#8B98A5" : "#5C6B78"
    readonly property color accent:    dark ? "#17C7B6" : "#12B5A6"
    readonly property color accentInk: dark ? "#0E141B" : "#1E2A32"   // accent 위 글자색 (onXxx 이름은 QML이 시그널 핸들러로 읽어서 피한다)
    readonly property color accentHover: dark ? "#3AD4C4" : "#3AD1C3"
    readonly property color normal:    dark ? "#35B368" : "#257A4A"
    readonly property color warn:      dark ? "#E0A030" : "#8A6400"
    readonly property color high:      dark ? "#FF9F45" : "#A05000"
    readonly property color critical:  dark ? "#FF5A5F" : "#C23934"
    readonly property color info:      dark ? "#4DA3FF" : "#1D5FA8"
    readonly property color select:    dark ? "#F37321" : "#C25A10"   // "여러 개 중 지금 고른 하나"
    readonly property color selectInk: dark ? "#1A1204" : "#FFFFFF"
    readonly property color videoSurface: "#000000"                   // 테마와 무관하게 항상 검정
    readonly property string subHex: dark ? "8B98A5" : "5C6B78"        // image://icons 색 인자용
    readonly property string mono: "Consolas"
}
