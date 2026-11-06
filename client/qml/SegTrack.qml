import QtQuick

// #camSeg 트랙 + #camSegBtn 알약 버튼 묶음. model: 문자열 배열, current: 선택 인덱스
Rectangle {
    id: t
    property var model: []
    property int current: 0
    property int minBtnWidth: 96
    property bool addButton: false
    signal picked(int index)
    signal addClicked()
    signal contextRequested(int index)

    color: Theme.bgDeep; border.color: Theme.border; radius: 4
    implicitHeight: 42
    implicitWidth: row.implicitWidth + 8

    Row {
        id: row
        x: 4; y: 4; spacing: 4
        Repeater {
            model: t.model
            Rectangle {
                id: b
                required property string modelData
                required property int index
                width: Math.max(t.minBtnWidth, lab.implicitWidth + 24); height: 34; radius: 3
                color: t.current === index ? Theme.select : "transparent"
                Text { id: lab; anchors.centerIn: parent; text: b.modelData; font.pixelSize: 13
                       font.weight: t.current === b.index ? Font.ExtraBold : Font.Bold
                       color: t.current === b.index ? Theme.selectInk : (ma.containsMouse ? Theme.text : Theme.sub) }
                MouseArea { id: ma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onClicked: function(m) { if (m.button === Qt.RightButton) t.contextRequested(b.index); else t.picked(b.index) } }
            }
        }
        Rectangle {
            visible: t.addButton
            width: 34; height: 34; radius: 3; color: "transparent"
            Text { anchors.centerIn: parent; text: "＋"; font.pixelSize: 13; font.weight: Font.Bold
                   color: addMa.containsMouse ? Theme.text : Theme.sub }
            MouseArea { id: addMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: t.addClicked() }
        }
    }
}
