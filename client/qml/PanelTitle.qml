import QtQuick

// #panelTitle — 좌측 4px 청록 바 + 17px 굵은 제목
Item {
    property alias text: t.text
    implicitWidth: t.implicitWidth + 16
    implicitHeight: t.implicitHeight + 2
    Rectangle { width: 4; height: parent.height; color: Theme.accent }
    Text {
        id: t
        x: 16
        anchors.verticalCenter: parent.verticalCenter
        color: Theme.text
        font.pixelSize: 17
        font.weight: Font.ExtraBold
        font.letterSpacing: 0.3
    }
}
