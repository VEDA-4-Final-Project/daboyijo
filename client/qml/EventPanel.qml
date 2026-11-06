import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 실시간 이벤트 목록 + 경보 해제 버튼
Rectangle {
    id: panel
    color: Theme.panel
    radius: 4
    border.color: Theme.border

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 16; anchors.rightMargin: 16
        anchors.topMargin: 14; anchors.bottomMargin: 16
        spacing: 10

        PanelTitle { text: "실시간 이벤트" }

        Button {
            id: clearBtn
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            enabled: backend.anyAlarm
            text: backend.anyAlarm ? "경보 해제" : "경보 없음"
            onClicked: backend.clearAlarms()
            contentItem: Text {
                text: clearBtn.text
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                color: "white"; font.bold: true; font.pixelSize: 14
            }
            background: Rectangle {
                radius: 6
                color: backend.anyAlarm ? Theme.critical : Theme.card
            }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            model: backend.events
            delegate: Rectangle {
                required property string time
                required property string typeLabel
                required property string type
                required property string who
                width: ListView.view.width
                height: 54
                radius: 6
                color: Theme.card
                Rectangle {   // 왼쪽 색 띠: 낙상·생체이상 빨강, 이탈 주황
                    width: 4; height: parent.height; radius: 2
                    color: type === "EGRESS" ? Theme.warn : Theme.critical
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    x: 14; spacing: 3
                    Text {
                        text: typeLabel + "  ·  " + time
                        color: Theme.text; font.bold: true; font.pixelSize: 14
                    }
                    Text {
                        text: who
                        color: Theme.sub; font.pixelSize: 12
                    }
                }
            }
            Text {
                anchors.centerIn: parent
                visible: list.count === 0
                text: "아직 이벤트가 없습니다"
                color: Theme.sub; font.pixelSize: 13
            }
        }
    }
}
