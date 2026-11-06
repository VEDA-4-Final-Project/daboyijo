import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import Daboyijo 1.0

// 영상 검색 — 위젯 buildVideoSearchTab() 과 같은 3단(조건 | 결과 | 재생기).
// 서버(LLM → 이벤트 DB → VLM) 답을 MonitorBackend.parseSearch 가 쪼개 준다.
Rectangle {
    id: panel
    color: Theme.panel; radius: 4; border.color: Theme.border

    property var hits: []
    property string message: ""
    property string countText: ""
    property string context: ""
    property bool showVideo: false

    function fmt(ms) { const s = Math.floor(ms / 1000); return String(Math.floor(s / 60)).padStart(2, "0") + ":" + String(s % 60).padStart(2, "0") }
    function go(text) {
        if (text !== undefined) query.text = text
        if (query.text.trim() === "") return
        panel.hits = []; panel.message = ""; panel.countText = "검색 중…"
        backend.search(chBox.currentIndex - 1, query.text)
    }
    function reset() {
        query.text = ""; chBox.currentIndex = 0; hits = []; message = ""; countText = ""
        context = ""; showVideo = false; player.stop()
    }

    Connections {
        target: backend
        function onSearchResult(text) {
            const r = backend.parseSearch(text)
            panel.hits = r.hits
            panel.message = r.message
            panel.countText = r.hits.length > 0 ? r.hits.length + "건" : ""
        }
    }
    MediaPlayer { id: player; videoOutput: vout; audioOutput: AudioOutput {} }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 14; anchors.rightMargin: 14; anchors.topMargin: 12; anchors.bottomMargin: 14
        spacing: 10
        PanelTitle { text: "영상 검색" }

        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 12

            // 좌: 검색 조건
            Rectangle {
                Layout.preferredWidth: 248; Layout.fillHeight: true
                color: Theme.card; border.color: Theme.border; radius: 4
                Column {
                    x: 12; y: 12; width: parent.width - 24; spacing: 6
                    Row {
                        spacing: 8
                        Rectangle { width: 3; height: 18; color: Theme.select }
                        Text { text: "검색 조건"; color: Theme.text; font.pixelSize: 12; font.weight: Font.ExtraBold; font.letterSpacing: 0.6 }
                    }
                    Item { width: 1; height: 4 }
                    Text { text: "채널"; color: Theme.sub; font.pixelSize: 11; font.weight: Font.Bold }
                    DropBox { id: chBox; width: parent.width; model: ["전체 채널", "채널 1", "채널 2", "채널 3", "채널 4"] }
                    Item { width: 1; height: 6 }
                    Text { text: "질문"; color: Theme.sub; font.pixelSize: 11; font.weight: Font.Bold }
                    FormEdit {
                        id: query; width: parent.width
                        placeholderText: "예: 어제 저녁에 낙상 있었어?"
                        onAccepted: panel.go()
                    }
                    Item { width: 1; height: 8 }
                    Row {
                        spacing: 6; width: parent.width
                        FlatButton {
                            width: parent.width - resetB.width - 6
                            text: backend.searching ? "검색 중…" : "검색"; px: 12; weight: Font.ExtraBold; padV: 8; padH: 16
                            bg: backend.searching ? Theme.border : Theme.accent
                            fg: backend.searching ? Theme.sub : Theme.accentInk
                            bd: bg; hoverBg: Theme.accentHover; hoverFg: Theme.accentInk; hoverBd: Theme.accentHover
                            onClicked: if (!backend.searching) panel.go()
                        }
                        FlatButton { id: resetB; text: "초기화"; px: 12; padV: 8; onClicked: panel.reset() }
                    }
                    Item { width: 1; height: 10 }
                    Text { text: "예시 질문"; color: Theme.sub; font.pixelSize: 11; font.weight: Font.Bold }
                    Repeater {
                        model: ["어제 저녁에 낙상 있었어?", "이번 주에 침대에서 나간 적 있어?", "오늘 새벽에 무슨 일 있었어?"]
                        Rectangle {
                            required property string modelData
                            width: parent.width; height: chipT.implicitHeight + 14
                            radius: 3; color: Theme.bgDeep
                            border.color: chipMa.containsMouse ? Theme.select : Theme.border
                            Text { id: chipT; x: 9; width: parent.width - 18; anchors.verticalCenter: parent.verticalCenter
                                   text: modelData; wrapMode: Text.Wrap; font.pixelSize: 12
                                   color: chipMa.containsMouse ? Theme.text : Theme.sub }
                            MouseArea { id: chipMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                        onClicked: panel.go(modelData) }
                        }
                    }
                }
            }

            // 중: 결과 목록
            ColumnLayout {
                Layout.preferredWidth: 300; Layout.fillHeight: true; spacing: 6
                Text { text: panel.countText; color: Theme.sub; font.pixelSize: 12; font.weight: Font.Bold }
                ListView {
                    visible: panel.hits.length > 0
                    Layout.fillWidth: true; Layout.fillHeight: true
                    clip: true; model: panel.hits; spacing: 2
                    ScrollBar.vertical: ScrollBar { }
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        width: ListView.view.width - 6
                        height: resT.implicitHeight + 20
                        radius: 3
                        color: ma.containsMouse ? Theme.card : (index % 2 ? Theme.card : "transparent")
                        border.color: panel.context === modelData.context ? Theme.select : "transparent"
                        Text { id: resT; x: 10; width: parent.width - 20; anchors.verticalCenter: parent.verticalCenter
                               wrapMode: Text.Wrap; font.pixelSize: 13
                               text: modelData.meta === "" ? modelData.when : modelData.when + "\n" + modelData.meta
                               color: modelData.url === "" ? Theme.sub : Theme.text }
                        MouseArea {
                            id: ma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                panel.context = modelData.context
                                if (modelData.url === "") { panel.showVideo = false; player.stop(); return }
                                panel.showVideo = true
                                player.source = modelData.url; player.play()
                            }
                        }
                    }
                }
                Text {
                    visible: panel.message !== ""
                    Layout.fillWidth: true; wrapMode: Text.Wrap; text: panel.message
                    color: Theme.text; font.pixelSize: 13
                }
                Item { Layout.fillHeight: panel.hits.length === 0 }
            }

            // 우: 재생기
            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true; spacing: 6
                Rectangle {
                    visible: panel.context !== ""
                    Layout.fillWidth: true; implicitHeight: ctxT.implicitHeight + 14
                    color: Theme.card; border.color: Theme.border; radius: 3
                    Rectangle { width: 3; height: parent.height; color: Theme.select }
                    Text { id: ctxT; x: 13; width: parent.width - 23; anchors.verticalCenter: parent.verticalCenter
                           text: panel.context; wrapMode: Text.Wrap; color: Theme.text; font.pixelSize: 12; font.weight: Font.Bold }
                }
                Rectangle {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    color: Theme.panel; border.color: Theme.border; radius: 4
                    VideoOutput { id: vout; anchors.fill: parent; anchors.margins: 1; visible: panel.showVideo }
                    Text { visible: !panel.showVideo; anchors.fill: parent; anchors.margins: 20; wrapMode: Text.Wrap
                           horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                           text: "왼쪽에서 질문하고, 가운데 목록에서 기록을 고르면 여기서 바로 재생됩니다."
                           color: Theme.sub; font.pixelSize: 13 }
                }
                Rectangle {
                    Layout.fillWidth: true; height: 40
                    color: Theme.card; border.color: Theme.border; radius: 4
                    RowLayout {
                        anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 8
                        Rectangle {
                            Layout.preferredWidth: 30; Layout.preferredHeight: 26; radius: 3
                            color: ppMa.containsMouse ? Theme.bgDeep : "transparent"; border.color: Theme.border
                            Text { anchors.centerIn: parent; color: Theme.text; font.pixelSize: 13
                                   text: player.playbackState === MediaPlayer.PlayingState ? "⏸" : "▶" }
                            MouseArea { id: ppMa; anchors.fill: parent; hoverEnabled: true
                                        onClicked: player.playbackState === MediaPlayer.PlayingState ? player.pause() : player.play() }
                        }
                        Slider { Layout.fillWidth: true; from: 0; to: Math.max(1, player.duration); value: player.position
                                 onMoved: player.position = value }
                        Text { text: panel.fmt(player.position) + " / " + panel.fmt(player.duration)
                               color: Theme.sub; font.pixelSize: 12 }
                    }
                }
            }
        }
    }
}
