import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import Daboyijo 1.0

// Carenet QML 관제 화면 — 위젯 앱(MainWindow)과 같은 배치·색·크기.
// 헤더 / 좌측 네비 레일 / 페이지 스택, 위에 경보 비네트와 경보 토스트.
ApplicationWindow {
    id: win
    width: 1680; height: 960
    minimumWidth: 1420; minimumHeight: 780
    visible: true
    title: "Carenet · 요양원 통합 모니터링"
    color: Theme.bgDeep

    property int page: 0
    property bool navCollapsed: String(backend.setting("ui/nav_collapsed", false)) === "true"

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        visible: App.loggedIn

        // ── 헤더 (#header) ───────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 68
            gradient: Gradient {
                GradientStop { position: 0; color: Theme.panel }
                GradientStop { position: 1; color: Theme.card }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 22; anchors.rightMargin: 18
                spacing: 10

                Text {
                    text: "Carenet"
                    color: Theme.text
                    font.pixelSize: 23; font.weight: Font.ExtraBold; font.letterSpacing: 1.2
                }
                Item { Layout.fillWidth: true }

                // 방송(인터콤) 토글
                FlatButton {
                    id: mic
                    readonly property bool active: App.broadcasting
                    text: active ? "🔴 방송 중" : "🎤 방송"
                    padH: 15; padV: 7
                    bg: Theme.card; fg: Theme.text
                    hoverFg: Theme.text; hoverBd: Theme.accent
                    checked: active
                    checkedBg: Theme.critical; checkedFg: "#FFFFFF"; checkedBd: Theme.critical
                    onClicked: App.setBroadcast(!active)
                }

                Text {
                    id: clock
                    leftPadding: 6; rightPadding: 6
                    color: Theme.text
                    font.pixelSize: 16; font.weight: Font.ExtraBold; font.letterSpacing: 0.5
                    Timer {
                        interval: 1000; running: true; repeat: true; triggeredOnStart: true
                        onTriggered: clock.text = Qt.formatDateTime(new Date(), "yyyy-MM-dd  HH:mm:ss")
                    }
                }

                // 도움말 — 원형 ? + 텍스트
                Rectangle {
                    implicitWidth: helpRow.implicitWidth + 20
                    implicitHeight: helpRow.implicitHeight + 8
                    radius: 4
                    color: helpMa.containsMouse ? Theme.card : "transparent"
                    Row {
                        id: helpRow
                        x: 8; anchors.verticalCenter: parent.verticalCenter
                        spacing: 4
                        Rectangle {
                            width: 18; height: 18; radius: 9
                            anchors.verticalCenter: parent.verticalCenter
                            color: "transparent"
                            border.color: "#8B98A5"; border.width: 1.4
                            Text { anchors.centerIn: parent; text: "?"; color: "#8B98A5"; font.pixelSize: 13; font.bold: true }
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "도움말"
                            color: helpMa.containsMouse ? Theme.text : Theme.sub
                            font.pixelSize: 13; font.weight: Font.DemiBold
                        }
                    }
                    MouseArea { id: helpMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: { help.show(); help.raise(); help.requestActivate() } }
                }

                // 테마 토글 (원형)
                Rectangle {
                    implicitWidth: 32; implicitHeight: 32; radius: 16
                    color: themeMa.containsMouse ? Theme.card : "transparent"
                    border.color: themeMa.containsMouse ? Theme.accent : Theme.border
                    Text { anchors.centerIn: parent; text: App.dark ? "☀" : "🌙"; color: Theme.text; font.pixelSize: 15 }
                    MouseArea { id: themeMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: App.toggleTheme() }
                }

                // 계정 칩
                Text {
                    visible: text.length > 0
                    text: App.userName
                    color: Theme.text
                    font.pixelSize: 13; font.weight: Font.Bold
                }
                FlatButton {
                    text: "로그아웃"
                    radius: 4
                    bd: "transparent"; hoverBd: "transparent"
                    hoverBg: Theme.critical; hoverFg: "#FFFFFF"
                    onClicked: App.logout()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // ── 좌측 네비 레일 (#navRail) ─────────────────────
            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: win.navCollapsed ? 62 : 208
                color: Theme.panel
                Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }

                Column {
                    anchors.fill: parent
                    anchors.leftMargin: 10; anchors.rightMargin: 11
                    anchors.topMargin: 12; anchors.bottomMargin: 12
                    spacing: 4

                    Rectangle {
                        width: 34; height: 32; radius: 3
                        color: navTgMa.containsMouse ? Theme.card : "transparent"
                        Text {
                            anchors.centerIn: parent
                            text: "☰"
                            color: navTgMa.containsMouse ? Theme.text : Theme.sub
                            font.pixelSize: 15
                        }
                        MouseArea {
                            id: navTgMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                win.navCollapsed = !win.navCollapsed
                                backend.setSetting("ui/nav_collapsed", win.navCollapsed)
                            }
                        }
                    }
                    Item { width: 1; height: 6 }   // 위젯 addSpacing(10)과 같은 간격

                    Repeater {
                        model: ["실시간 관제", "이벤트 기록", "영상 검색", "일일 리포트", "입소자 관리", "장치 설정"]
                        Rectangle {
                            required property string modelData
                            required property int index
                            readonly property bool on: win.page === index
                            width: parent.width; height: 40; radius: 3
                            color: on ? Theme.accent : navMa.containsMouse ? Theme.card : "transparent"
                            Row {
                                anchors.verticalCenter: parent.verticalCenter
                                x: win.navCollapsed ? (parent.width - 20) / 2 : 12
                                spacing: 6
                                Image {
                                    anchors.verticalCenter: parent.verticalCenter
                                    source: "image://icons/nav/" + index + "/" + (on ? "FFFFFF" : Theme.subHex)
                                    sourceSize: Qt.size(20, 20)
                                }
                                Text {
                                    visible: !win.navCollapsed
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: modelData
                                    color: on ? Theme.accentInk : navMa.containsMouse ? Theme.text : Theme.sub
                                    font.pixelSize: 13
                                    font.weight: on ? Font.ExtraBold : Font.Bold
                                    font.letterSpacing: 0.2
                                }
                            }
                            MouseArea {
                                id: navMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: win.page = index
                            }
                            ToolTip.visible: win.navCollapsed && navMa.containsMouse
                            ToolTip.text: modelData
                        }
                    }
                }
            }

            // ── 페이지 ───────────────────────────────────────
            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: win.page

                MonitorPage {}
                Item {
                    EventLogPage { anchors.fill: parent; anchors.margins: 12 }
                }
                Item {
                    SearchPanel { anchors.fill: parent; anchors.margins: 12 }
                }
                ReportPage {}
                ResidentPage {}
                DevicePage {}
            }
        }
    }

    HelpDialog { id: help }

    // ── 로그인 (로그아웃하면 다시 이 화면) ─────────────────
    LoginPage { anchors.fill: parent; visible: !App.loggedIn }

    // ── 경보 비네트 (창 가장자리 빨강 호흡) ─────────────────
    AlarmOverlayItem {
        anchors.fill: parent
        active: backend.anyAlarm
    }

    // ── 경보 토스트 (위에서 내려옴) ─────────────────────────
    Rectangle {
        id: toast
        readonly property bool shown: backend.anyAlarm
        width: toastRow.implicitWidth + 18 + 14 + 3
        height: toastRow.implicitHeight + 24
        x: Math.max(12, (win.width - width) / 2)
        y: shown ? 78 : -height - 24
        Behavior on y { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }
        color: Theme.card
        border.color: Theme.border
        radius: 4
        Rectangle {   // 좌측 4px 빨강 띠
            width: 4; height: parent.height; radius: 2
            color: Theme.critical
        }
        RowLayout {
            id: toastRow
            x: 18 + 3
            anchors.verticalCenter: parent.verticalCenter
            spacing: 12
            Rectangle { width: 9; height: 9; radius: 4; color: Theme.critical }
            Text {
                text: backend.alarmSummary
                color: Theme.text
                font.pixelSize: 14; font.weight: Font.ExtraBold; font.letterSpacing: 0.2
                Layout.rightMargin: 8
            }
            FlatButton {
                text: "경보 해제"
                px: 13; weight: Font.ExtraBold
                padH: 16; padV: 7
                bg: Theme.critical; fg: "#FFFFFF"; bd: Theme.critical
                hoverBg: "#ff6b62"; hoverFg: "#FFFFFF"; hoverBd: "#ff6b62"
                onClicked: backend.clearAlarms()
            }
        }
    }
}
