import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtMultimedia
import Daboyijo 1.0

// 이벤트 기록 — 위젯 buildEventLogTab() 과 같은 배치(필터 | 표 | 블랙박스).
// 조회·확인 처리·NVR 이어보기·다운로드는 EventLogBackend(C++).
Rectangle {
    id: page
    color: Theme.panel; border.color: Theme.border; radius: 4

    property string clipUrl: ""
    property string context: ""
    property int selChannel: -1
    property double selAtMs: -1
    property int selRow: -1
    property int retries: 0
    property double pendingSeek: -1
    property string placeholder: "블랙박스 영상을 불러오는 중…"
    property bool showVideo: false

    function sevColor(t) { return t === "낙상" ? Theme.critical : t === "침상이탈" ? Theme.high : Theme.warn }
    function fmt(ms) { const s = Math.floor(ms / 1000); return String(Math.floor(s / 60)).padStart(2, "0") + ":" + String(s % 60).padStart(2, "0") }

    function play(url) {
        if (url !== page.clipUrl) { page.clipUrl = url; page.retries = 0 }
        showVideo = true
        player.stop(); player.source = ""; player.source = url; player.play()
    }
    Component.onCompleted: { events.reload(); events.refreshNvr() }

    ConfirmDialog { id: dlg }
    MediaPlayer {
        id: player
        videoOutput: vout
        audioOutput: AudioOutput {}
        onDurationChanged: if (page.pendingSeek >= 0) { position = Math.min(page.pendingSeek, duration); page.pendingSeek = -1 }
        onErrorOccurred: function(err, msg) {
            if (page.retries < 8 && page.clipUrl !== "") {
                page.retries++
                page.placeholder = "블랙박스 저장 중… 잠시 후 재생됩니다 (" + page.retries + ")"
                page.showVideo = false
                const u = page.clipUrl
                retry.url = u; retry.restart()
            } else {
                page.placeholder = "재생 실패 — 클립을 찾을 수 없습니다\n(" + msg + ")"
                page.showVideo = false
            }
        }
    }
    Timer { id: retry; property string url; interval: 1500; onTriggered: if (page.clipUrl === url) page.play(url) }
    FileDialog {
        id: saveDlg
        fileMode: FileDialog.SaveFile
        nameFilters: ["영상 파일 (*.mp4)"]
        onAccepted: events.download(page.clipUrl, selectedFile.toString())
    }
    Connections {
        target: events
        function onDownloadFinished(ok, msg) { dlg.notify(ok ? "다운로드 완료" : "다운로드 실패", msg) }
    }

    ColumnLayout {
        anchors.fill: parent; anchors.margins: 18; anchors.topMargin: 16; anchors.bottomMargin: 16
        spacing: 14
        PanelTitle { text: "이벤트 기록" }

        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 12

            // ── 검색 조건 ──
            Rectangle {
                Layout.preferredWidth: 206; Layout.fillHeight: true
                color: Theme.card; border.color: Theme.border; radius: 4
                Flickable {
                    anchors.fill: parent; anchors.margins: 12
                    contentHeight: fcol.implicitHeight; clip: true
                    Column {
                        id: fcol; width: parent.width; spacing: 6
                        Row {
                            spacing: 8
                            Rectangle { width: 3; height: 18; color: Theme.select }
                            Text { text: "검색 조건"; color: Theme.text; font.pixelSize: 12; font.weight: Font.ExtraBold; font.letterSpacing: 0.6 }
                        }
                        Item { width: 1; height: 0 }
                        Row {
                            spacing: 4
                            Repeater {
                                model: [["오늘", 0], ["7일", 6], ["30일", 29]]
                                FlatButton { required property var modelData
                                    text: modelData[0]; px: 12; padH: 10; padV: 5; weight: Font.DemiBold
                                    onClicked: events.preset(modelData[1]) }
                            }
                        }
                        Item { width: 1; height: 6 }
                        Text { text: "시작일"; color: Theme.sub; font.pixelSize: 11; font.weight: Font.Bold }
                        FormEdit { width: parent.width; text: events.dateFrom; placeholderText: "yyyy-MM-dd"
                                   onEditingFinished: events.dateFrom = text }
                        Item { width: 1; height: 6 }
                        Text { text: "종료일"; color: Theme.sub; font.pixelSize: 11; font.weight: Font.Bold }
                        FormEdit { width: parent.width; text: events.dateTo; placeholderText: "yyyy-MM-dd"
                                   onEditingFinished: events.dateTo = text }
                        Item { width: 1; height: 6 }
                        Text { text: "병실"; color: Theme.sub; font.pixelSize: 11; font.weight: Font.Bold }
                        DropBox { width: parent.width; model: ["전체 병실", events.roomName] }
                        Item { width: 1; height: 6 }
                        Text { text: "채널"; color: Theme.sub; font.pixelSize: 11; font.weight: Font.Bold }
                        DropBox { width: parent.width; model: ["전체 채널", "채널 1", "채널 2", "채널 3", "채널 4"]
                                  currentIndex: events.channel + 1
                                  onActivated: events.channel = currentIndex - 1 }
                        Item { width: 1; height: 6 }
                        Text { text: "이벤트 종류"; color: Theme.sub; font.pixelSize: 11; font.weight: Font.Bold }
                        DropBox { width: parent.width; model: ["전체 이벤트", "낙상", "침상이탈", "생체신호 이상"]
                                  currentIndex: Math.max(0, model.indexOf(events.eventType))
                                  onActivated: events.eventType = model[currentIndex] }
                        Item { width: 1; height: 6 }
                        Text { text: "확인 여부"; color: Theme.sub; font.pixelSize: 11; font.weight: Font.Bold }
                        DropBox { width: parent.width; model: ["전체", "미확인만", "확인만"]
                                  currentIndex: Math.max(0, model.indexOf(events.confirmed))
                                  onActivated: events.confirmed = model[currentIndex] }
                        Item { width: 1; height: 6 }
                        FlatButton { width: parent.width; text: "조건 초기화"; px: 12; padV: 5; onClicked: events.resetFilters() }
                    }
                }
            }

            // ── 표 ──
            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 5; spacing: 6
                RowLayout {
                    Layout.fillWidth: true; Layout.leftMargin: 2; Layout.rightMargin: 2
                    Text { text: events.countText; color: events.hasUnconfirmed ? Theme.critical : Theme.sub
                           font.pixelSize: 12; font.weight: Font.Bold }
                    Item { Layout.fillWidth: true }
                    FlatButton { text: "새로고침"; px: 12; padV: 5; onClicked: { events.reload(); events.refreshNvr() } }
                }
                Rectangle {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    color: Theme.panel; border.color: Theme.border; radius: 4; clip: true
                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 1; spacing: 0
                        Rectangle {
                            Layout.fillWidth: true; height: 34; color: Theme.bgDeep
                            RowLayout {
                                anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 0
                                Repeater {
                                    model: [["발생시각", 150], ["종류", 100], ["위치", -1], ["입소자", 90], ["출처", 70], ["상태", 60]]
                                    Text { required property var modelData
                                           Layout.preferredWidth: modelData[1]; Layout.fillWidth: modelData[1] < 0
                                           horizontalAlignment: modelData[0] === "발생시각" || modelData[0] === "위치" ? Text.AlignLeft : Text.AlignHCenter
                                           text: modelData[0]; color: Theme.sub; font.pixelSize: 13; font.weight: Font.Bold }
                                }
                            }
                        }
                        ListView {
                            id: lv
                            Layout.fillWidth: true; Layout.fillHeight: true
                            clip: true; model: events.rows
                            ScrollBar.vertical: ScrollBar { }
                            delegate: Rectangle {
                                required property var modelData
                                required property int index
                                width: ListView.view.width; height: 34
                                color: page.selRow === index ? Theme.select : rowMa.containsMouse ? Theme.card
                                     : (index % 2 ? Theme.card : "transparent")
                                readonly property color ink: page.selRow === index ? Theme.selectInk : Theme.text
                                RowLayout {
                                    anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 0
                                    Text { Layout.preferredWidth: 150; text: modelData.when; font.pixelSize: 13
                                           color: page.selRow === index ? Theme.selectInk : Theme.sub }
                                    Text { Layout.preferredWidth: 100; horizontalAlignment: Text.AlignHCenter; text: modelData.type; font.pixelSize: 13
                                           color: page.selRow === index ? Theme.selectInk : page.sevColor(modelData.type) }
                                    Text { Layout.fillWidth: true; text: modelData.place; color: ink; font.pixelSize: 13; elide: Text.ElideRight }
                                    Text { Layout.preferredWidth: 90; horizontalAlignment: Text.AlignHCenter; text: modelData.resident; color: ink; font.pixelSize: 13 }
                                    Text { Layout.preferredWidth: 70; horizontalAlignment: Text.AlignHCenter; text: modelData.source; font.pixelSize: 13
                                           color: page.selRow === index ? Theme.selectInk : Theme.sub }
                                    Text { Layout.preferredWidth: 60; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 13
                                           text: modelData.confirmed ? "확인" : "미확인"
                                           color: page.selRow === index ? Theme.selectInk : (modelData.confirmed ? Theme.normal : Theme.critical) }
                                }
                                MouseArea {
                                    id: rowMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        page.selRow = index
                                        const r = events.activate(index)
                                        page.context = r.context
                                        page.selChannel = r.channel; page.selAtMs = r.atMs
                                        if (!r.clipUrl) {
                                            page.clipUrl = ""
                                            page.placeholder = "이 이벤트에는 저장된 클립이 없습니다.\n아래 [이 시점 NVR에서 이어보기]를 눌러 보세요."
                                            page.showVideo = false; player.stop()
                                        } else page.play(r.clipUrl)
                                    }
                                }
                            }
                            Text {
                                visible: events.rows.length === 0
                                anchors.horizontalCenter: parent.horizontalCenter; y: 40
                                horizontalAlignment: Text.AlignHCenter
                                text: events.dbOpen ? "이 조건에 맞는 이벤트가 없습니다.\n왼쪽에서 기간을 넓히거나 종류·채널 필터를 풀어 보세요."
                                                    : "DB에 연결되지 않았습니다.\n연결되면 지난 이벤트 기록이 여기에 나타납니다."
                                color: Theme.sub; font.pixelSize: 13
                            }
                        }
                    }
                }
            }

            // ── 블랙박스 ──
            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 4; spacing: 8
                PanelTitle { text: "블랙박스" }
                Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.sub; font.pixelSize: 13
                       text: "왼쪽 표에서 이벤트를 고르면 여기서 바로 재생됩니다." }
                Rectangle {
                    visible: page.context !== ""
                    Layout.fillWidth: true
                    implicitHeight: ctxT.implicitHeight + 14
                    color: Theme.card; border.color: Theme.border; radius: 3
                    Rectangle { width: 3; height: parent.height; color: Theme.select }
                    Text { id: ctxT; x: 13; width: parent.width - 23; anchors.verticalCenter: parent.verticalCenter
                           text: page.context; wrapMode: Text.Wrap; color: Theme.text; font.pixelSize: 12; font.weight: Font.Bold }
                }
                Rectangle {
                    Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 220
                    color: Theme.videoSurface; border.color: Theme.border; radius: 4
                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 10; spacing: 8
                        Item {
                            Layout.fillWidth: true; Layout.fillHeight: true
                            VideoOutput { id: vout; anchors.fill: parent; visible: page.showVideo }
                            Text { visible: !page.showVideo; anchors.fill: parent; wrapMode: Text.Wrap
                                   horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                   text: page.placeholder; color: "#8B98A5"; font.pixelSize: 13 }
                        }
                        RowLayout {
                            Layout.fillWidth: true; spacing: 8
                            Rectangle {
                                width: 30; height: 26; radius: 3; color: pp.containsMouse ? Theme.card : "transparent"
                                border.color: Theme.border; opacity: page.clipUrl === "" ? 0.4 : 1
                                Text { anchors.centerIn: parent; color: "#E6EDF3"; font.pixelSize: 13
                                       text: player.playbackState === MediaPlayer.PlayingState ? "❚❚" : "▶" }
                                MouseArea { id: pp; anchors.fill: parent; hoverEnabled: true; enabled: page.clipUrl !== ""
                                            onClicked: player.playbackState === MediaPlayer.PlayingState ? player.pause() : player.play() }
                            }
                            Slider {
                                Layout.fillWidth: true; enabled: page.clipUrl !== ""
                                from: 0; to: Math.max(1, player.duration)
                                value: player.position
                                onMoved: player.position = value
                            }
                            Text { text: page.fmt(player.position) + " / " + page.fmt(player.duration)
                                   color: "#8B98A5"; font.pixelSize: 13 }
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true; spacing: 8
                    FlatButton {
                        Layout.fillWidth: true; text: "이 시점 NVR에서 이어보기"; px: 13; weight: Font.Bold; padV: 6
                        bg: Theme.card; fg: page.selAtMs > 0 ? Theme.text : Theme.sub
                        opacity: page.selAtMs > 0 ? 1 : 0.6
                        onClicked: {
                            if (page.selAtMs <= 0) return
                            const r = events.nvrContext(page.selChannel, page.selAtMs)
                            if (!r.ok) { dlg.notify("NVR 없음", r.message); return }
                            page.pendingSeek = r.seekMs; page.play(r.url)
                        }
                    }
                    FlatButton {
                        text: "다운로드"; px: 13; weight: Font.Bold; padV: 6; bg: Theme.card
                        fg: page.clipUrl !== "" ? Theme.text : Theme.sub; opacity: page.clipUrl !== "" ? 1 : 0.6
                        onClicked: if (page.clipUrl !== "") { saveDlg.currentFile = "file:///" + events.suggestedName(page.clipUrl); saveDlg.open() }
                    }
                }
            }
        }
    }
}
