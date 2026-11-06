import QtQuick

// 위젯 QSS 버튼(#layoutTab, #modeBtn, #resChip …)과 같은 모양의 평면 버튼.
// checked는 바깥에서 묶는다(단일 선택 그룹은 부모가 상태를 들고 있다).
Rectangle {
    id: b
    property alias text: label.text
    property bool checked: false
    property int px: 12
    property int weight: Font.DemiBold
    property int padH: 12
    property int padV: 4
    property real spacingX: 0

    property color bg: "transparent"
    property color fg: Theme.sub
    property color bd: Theme.border
    property color hoverBg: bg
    property color hoverFg: Theme.text
    property color hoverBd: Theme.select
    property color checkedBg: Theme.select
    property color checkedFg: Theme.selectInk
    property color checkedBd: Theme.select
    readonly property bool hovered: ma.containsMouse

    signal clicked()

    implicitWidth: Math.ceil(label.implicitWidth) + 2 * padH + 2
    implicitHeight: Math.ceil(label.implicitHeight) + 2 * padV + 2
    radius: 3
    color: checked ? checkedBg : (hovered ? hoverBg : bg)
    border.width: 1
    border.color: checked ? checkedBd : (hovered ? hoverBd : bd)

    Text {
        id: label
        anchors.centerIn: parent
        color: b.checked ? b.checkedFg : (b.hovered ? b.hoverFg : b.fg)
        font.pixelSize: b.px
        font.weight: b.weight
        font.letterSpacing: b.spacingX
    }
    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: b.clicked()
    }
}
