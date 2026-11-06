import QtQuick
import Daboyijo 1.0

// 관제 타일 (#videoTile) — 영상·LIVE 배지·경보 테두리/배지·십자선은 VideoItem(C++)이
// 위젯 앱 VideoView와 같은 코드로 그리고, 여기서는 타일 크롬(선택 테두리·✕·호버 툴바)만.
Rectangle {
    id: tile
    required property int channel
    required property bool live
    required property int frameId
    required property bool alarm
    required property string alarmText
    required property real alertNx
    required property real alertNy
    required property string displayName
    required property var zones

    property bool selected: false
    signal picked()
    signal closeRequested()
    signal zoomRequested()

    color: Theme.videoSurface
    border.width: 2
    border.color: selected ? Theme.select : "transparent"

    VideoItem {
        id: video
        anchors.fill: parent
        anchors.margins: 3          // 테두리 2px + 레이아웃 여백 1px
        channel: tile.channel
        frameId: tile.frameId
        live: tile.live
        alert: tile.alarm
        alertLabel: tile.alarmText
        alertNx: tile.alertNx
        alertNy: tile.alertNy
        displayName: tile.displayName
        zones: tile.zones
    }

    HoverHandler { id: hover }
    TapHandler { onTapped: tile.picked() }

    // 우상단: 레이아웃에서 이 타일 빼기 (카메라는 계속 녹화)
    Rectangle {
        x: tile.width - 6 - width; y: 6
        width: 20; height: 20; radius: 3
        color: closeMa.containsMouse ? Theme.critical : Qt.rgba(0, 0, 0, 150 / 255)
        Text {
            anchors.centerIn: parent
            text: "✕"
            color: closeMa.containsMouse ? "#FFFFFF" : "#E6EDF3"
            font.pixelSize: 11; font.bold: true
        }
        MouseArea {
            id: closeMa
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: tile.closeRequested()
        }
    }

    // 하단 가운데: 호버 툴바 (크게 보기 · 스냅샷)
    Rectangle {
        id: toolbar
        visible: hover.hovered
        width: row.width + 12 + 2; height: row.height + 8 + 2
        x: (tile.width - width) / 2
        y: tile.height - 8 - height
        radius: 4
        color: Qt.rgba(0, 0, 0, 170 / 255)
        border.color: Qt.rgba(1, 1, 1, 40 / 255)
        Row {
            id: row
            anchors.centerIn: parent
            spacing: 2
            Repeater {
                model: 2
                Rectangle {
                    required property int index
                    width: 26; height: 22; radius: 3
                    color: toolMa.containsMouse ? Theme.select : "transparent"
                    Image {
                        anchors.centerIn: parent
                        source: "image://icons/tool/" + index + "/E6EDF3"
                        sourceSize: Qt.size(15, 15)
                    }
                    MouseArea {
                        id: toolMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            tile.picked()
                            if (index === 0) tile.zoomRequested()
                            else {
                                const path = backend.snapshotPath(tile.channel)
                                video.grabToImage(function(r) { r.saveToFile(path) })
                            }
                        }
                    }
                }
            }
        }
    }
}
