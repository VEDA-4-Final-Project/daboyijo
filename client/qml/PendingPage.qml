import QtQuick
import QtQuick.Layouts

// 아직 QML로 옮기지 않은 페이지 자리 (위젯 앱에서는 동작 중)
Item {
    property string title: ""
    Rectangle {
        anchors.fill: parent
        anchors.margins: 12
        color: Theme.panel
        border.color: Theme.border
        radius: 4
        ColumnLayout {
            anchors.fill: parent
            anchors.leftMargin: 16; anchors.rightMargin: 16
            anchors.topMargin: 14; anchors.bottomMargin: 16
            spacing: 12
            PanelTitle { text: title }
            Text {
                Layout.fillWidth: true
                Layout.fillHeight: true
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: "이 화면은 아직 QML로 옮기는 중입니다."
                color: Theme.sub
                font.pixelSize: 13
            }
        }
    }
}
