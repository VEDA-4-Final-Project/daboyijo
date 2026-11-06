import QtQuick
import QtQuick.Layouts
import Daboyijo 1.0

// 웨어러블 카드 (#vitalCard) — 위젯 앱 VitalTile과 같은 배치.
// 웨어러블(MQTT) 연결 전이라 지금은 무신호 상태("○ 대기")로만 그린다.
Rectangle {
    id: card
    // VitalModel(C++) 롤에서 그대로 들어온다
    required property string name
    required property string bed
    required property string badge
    required property string spo2
    required property string hr
    required property string severity     // "" = 무신호 / critical / medium / normal
    required property var history
    readonly property color sevColor: severity === "critical" ? Theme.critical
                                    : severity === "medium" ? Theme.warn
                                    : severity === "normal" ? Theme.normal : Theme.sub

    color: Theme.card
    border.color: Theme.border
    radius: 4

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 1
        spacing: 0

        // 머리: ● 이름  호실·채널            [○ 대기]
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: headRow.implicitHeight + 14
            color: Theme.panel
            radius: 3
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
            RowLayout {
                id: headRow
                anchors.fill: parent
                anchors.leftMargin: 14; anchors.rightMargin: 12
                spacing: 8
                Rectangle { width: 24; height: 24; radius: 12; color: card.sevColor }
                Text { text: card.name; color: Theme.text; font.pixelSize: 14; font.weight: Font.ExtraBold }
                Text { text: card.bed; color: Theme.sub; font.pixelSize: 12 }
                Item { Layout.fillWidth: true }
                Rectangle {
                    implicitWidth: badge.implicitWidth + 22
                    implicitHeight: badge.implicitHeight + 4
                    radius: 3
                    color: Theme.card
                    border.color: card.sevColor
                    Text {
                        id: badge
                        anchors.centerIn: parent
                        text: card.badge
                        color: card.sevColor
                        font.pixelSize: 11; font.weight: Font.ExtraBold
                    }
                }
            }
        }

        // 몸통: 산소포화도 · 심박
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14; Layout.rightMargin: 14
            Layout.topMargin: 9; Layout.bottomMargin: 6
            spacing: 10
            Repeater {
                model: [{ cap: "🫁  산소포화도", unit: "%", v: card.spo2 }, { cap: "❤  심박", unit: "bpm", v: card.hr }]
                Rectangle {
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    implicitHeight: statCol.implicitHeight + 16
                    color: Theme.bgDeep
                    border.color: Theme.border
                    radius: 4
                    Column {
                        id: statCol
                        x: 13; y: 8
                        spacing: 2
                        Text {
                            text: modelData.cap
                            color: Theme.sub
                            font.pixelSize: 11; font.weight: Font.Bold; font.letterSpacing: 0.5
                        }
                        Row {
                            spacing: 4
                            Text {
                                id: val
                                text: modelData.v
                                color: card.sevColor
                                font.family: Theme.mono
                                font.pixelSize: 32; font.weight: Font.ExtraBold
                            }
                            Text {
                                anchors.baseline: val.baseline
                                text: modelData.unit
                                color: Theme.sub
                                font.pixelSize: 12; font.weight: Font.DemiBold
                            }
                        }
                    }
                }
            }
        }

        // 심박 추이 (기준선 4개)
        SparklineItem {
            lineColor: card.sevColor
            values: card.history
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 28
            Layout.leftMargin: 14; Layout.rightMargin: 14; Layout.bottomMargin: 10
        }
    }
}
