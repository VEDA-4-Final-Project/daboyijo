import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import Daboyijo 1.0

// 실시간 관제 — 위젯 앱과 같은 3분할: 리소스 · 영상월(레이아웃 탭+그리드+타임라인) · 웨어러블
Item {
    id: page

    // 0 = 2×2 전체, 1 = 스포트라이트, 2 = 단일 채널
    property int layoutMode: 0
    property int selectedChannel: 0
    property var hidden: [false, false, false, false]
    property bool playbackMode: false
    property string pbHint: ""           // 재생 모드 안내 문구 ("" = 영상 재생 중)
    property double pbSegStart: 0
    property double pbLastMs: 0
    property real pbRate: 1

    function refreshTimeline() {
        timeline.setSpans(events.spans(selectedChannel))
        timeline.setMarkers(events.markers(selectedChannel))
    }
    function fmt(ms) { return Qt.formatDateTime(new Date(ms), "yyyy-MM-dd HH:mm:ss") }
    function setPlayback(on) {
        playbackMode = on
        if (!on) { player.stop(); player.source = ""; timeline.followNow(); pbHint = ""; return }
        pbHint = "아래 타임라인에서 볼 시각을 클릭하세요.\n초록 구간이 CH" + (selectedChannel + 1) + " 녹화가 남아 있는 시간대입니다."
        events.refreshNvr()
        refreshTimeline()
    }
    // 그 시각을 담은 NVR 세그먼트를 찾아 그 안의 상대 위치로 재생한다
    function seekTo(ms) {
        timeline.playhead = ms
        pbLastMs = ms
        tTime.text = fmt(ms)
        if (!playbackMode) setPlayback(true)
        const seg = events.segmentAt(selectedChannel, ms)
        if (!seg.ok) {
            tTime.text = Qt.formatDateTime(new Date(ms), "MM-dd HH:mm:ss") + " · 녹화 없음"
            player.stop()
            pbHint = Qt.formatDateTime(new Date(ms), "MM-dd HH:mm:ss") + " 에는 CH" + (selectedChannel + 1)
                     + " 녹화가 없습니다.\n타임라인의 초록 구간을 클릭하세요."
            return
        }
        pbHint = ""
        pbSegStart = seg.startMs
        const url = seg.url
        if (player.source.toString() !== url) {
            pendingSeek = seg.seekMs
            player.source = url
        } else {
            player.position = seg.seekMs
        }
        player.playbackRate = pbRate
        player.play()
    }
    property double pendingSeek: -1
    onSelectedChannelChanged: { refreshTimeline(); if (playbackMode && pbLastMs > 0) seekTo(timeline.playhead > 0 ? timeline.playhead : pbLastMs) }
    Component.onCompleted: refreshTimeline()
    Connections { target: events; function onNvrChanged() { page.refreshTimeline() } function onRowsChanged() { page.refreshTimeline() } }

    MediaPlayer {
        id: player
        videoOutput: pbVideo
        audioOutput: AudioOutput { }
        onDurationChanged: if (duration > 0 && page.pendingSeek >= 0) { position = Math.min(page.pendingSeek, duration); page.pendingSeek = -1 }
        onPositionChanged: {
            if (!page.playbackMode || page.pbHint !== "") return
            const at = page.pbSegStart + position
            timeline.playhead = at
            tTime.text = page.fmt(at)
        }
    }
    property bool resourceCollapsed: backend.setting("ui/resource_collapsed_v2", true) === true
                                     || backend.setting("ui/resource_collapsed_v2", true) === "true"

    function selectChannel(ch) {
        if (ch < 0 || ch > 3) return
        if (hidden[ch]) setHidden(ch, false)
        selectedChannel = ch
    }
    function setHidden(ch, h) {
        if (hidden[ch] === h) return
        if (h) {
            let visible = 0
            for (let i = 0; i < 4; ++i) if (!hidden[i]) ++visible
            if (visible <= 1) return          // 마지막 한 칸은 못 뺀다
        }
        const copy = hidden.slice(); copy[ch] = h; hidden = copy
        if (h && selectedChannel === ch)
            for (let i = 0; i < 4; ++i) if (!hidden[i]) { selectedChannel = i; break }
    }
    // 위젯 앱 setVideoFocus(): 경보 채널을 크게(스포트라이트), 해제하면 2×2
    function focusChannel(ch) {
        if (ch < 0) { layoutMode = 0; return }
        selectChannel(ch)
        layoutMode = 1
    }

    Connections {
        target: backend
        function onAlarmRaised(channel, atMs, markerColor) {
            page.focusChannel(channel)
            timeline.addMarker(atMs, markerColor)
        }
        function onAlarmChanged() {
            if (!backend.anyAlarm) page.focusChannel(-1)
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        // ── 리소스 패널 (접힘: 채널 칩 1~4) ─────────────────
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: page.resourceCollapsed ? 48 : 208
            color: Theme.panel
            border.color: Theme.border
            radius: 4

            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: page.resourceCollapsed ? 9 : 10
                anchors.rightMargin: page.resourceCollapsed ? 9 : 10
                anchors.topMargin: 12; anchors.bottomMargin: 12
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Text {
                        visible: !page.resourceCollapsed
                        Layout.fillWidth: true
                        text: "리소스"
                        color: Theme.sub
                        font.pixelSize: 12; font.weight: Font.Bold; font.letterSpacing: 0.6
                    }
                    FlatButton {
                        Layout.alignment: page.resourceCollapsed ? Qt.AlignHCenter : Qt.AlignRight
                        implicitWidth: 22; implicitHeight: 22
                        radius: 4
                        text: page.resourceCollapsed ? "›" : "‹"
                        weight: Font.Bold
                        onClicked: {
                            page.resourceCollapsed = !page.resourceCollapsed
                            backend.setSetting("ui/resource_collapsed_v2", page.resourceCollapsed)
                        }
                    }
                }

                // 접힘: 채널 칩
                Column {
                    visible: page.resourceCollapsed
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.topMargin: 6
                    spacing: 4
                    Repeater {
                        model: backend.channels
                        FlatButton {
                            required property int channel
                            required property bool live
                            anchors.horizontalCenter: parent.horizontalCenter
                            implicitWidth: 30; implicitHeight: 26
                            text: channel + 1
                            weight: Font.Bold
                            bg: Theme.card
                            checked: page.selectedChannel === channel
                            fg: page.hidden[channel] ? Theme.border : live ? Theme.normal : Theme.sub
                            bd: live && !page.hidden[channel] ? Theme.normal : Theme.border
                            onClicked: page.selectChannel(channel)
                        }
                    }
                }

                // 펼침: 검색 + 목록
                ColumnLayout {
                    visible: !page.resourceCollapsed
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 8
                    TextField {
                        id: resSearch
                        Layout.fillWidth: true
                        placeholderText: "카메라 · 입소자 검색"
                        placeholderTextColor: Theme.sub
                        color: Theme.text
                        font.pixelSize: 12
                        leftPadding: 8; rightPadding: 8; topPadding: 5; bottomPadding: 5
                        background: Rectangle {
                            color: Theme.card; radius: 4
                            border.color: resSearch.activeFocus ? Theme.select : Theme.border
                        }
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: ListModel {
                            ListElement { kind: "group"; label: "Root"; idx: -1; depth: 0 }
                            ListElement { kind: "room"; label: ""; idx: -1; depth: 1 }
                            ListElement { kind: "cam"; label: "CH1"; idx: 0; depth: 2 }
                            ListElement { kind: "cam"; label: "CH2"; idx: 1; depth: 2 }
                            ListElement { kind: "cam"; label: "CH3"; idx: 2; depth: 2 }
                            ListElement { kind: "cam"; label: "CH4"; idx: 3; depth: 2 }
                            ListElement { kind: "group"; label: "레이아웃"; idx: -1; depth: 0 }
                            ListElement { kind: "layout"; label: "2×2 전체"; idx: 0; depth: 1 }
                            ListElement { kind: "layout"; label: "스포트라이트"; idx: 1; depth: 1 }
                            ListElement { kind: "layout"; label: "단일 채널"; idx: 2; depth: 1 }
                        }
                        delegate: Rectangle {
                            required property string kind
                            required property string label
                            required property int idx
                            required property int depth
                            readonly property string shown: kind === "room" ? backend.roomName : label
                            readonly property bool sel: (kind === "cam" && page.selectedChannel === idx)
                                                     || (kind === "layout" && page.layoutMode === idx)
                            visible: resSearch.text.length === 0 || kind === "group"
                                     || shown.indexOf(resSearch.text) >= 0
                            width: ListView.view.width
                            height: visible ? 26 : 0
                            radius: 3
                            color: sel ? Theme.select : rowMa.containsMouse ? Theme.card : "transparent"
                            Row {
                                x: 4 + depth * 14
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 6
                                Image {
                                    visible: kind === "layout"
                                    anchors.verticalCenter: parent.verticalCenter
                                    source: "image://icons/nav/0/" + Theme.subHex
                                    sourceSize: Qt.size(16, 16)
                                }
                                Text {
                                    text: shown
                                    color: sel ? Theme.selectInk : Theme.text
                                    font.pixelSize: 12
                                }
                            }
                            MouseArea {
                                id: rowMa
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    if (kind === "cam") page.selectChannel(idx)
                                    else if (kind === "layout") page.layoutMode = idx
                                }
                                onDoubleClicked: if (kind === "cam") { page.selectChannel(idx); page.layoutMode = 2 }
                            }
                        }
                    }
                }
            }
        }

        // ── 영상월 ────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.panel
            border.color: Theme.border
            radius: 4

            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: 12; anchors.rightMargin: 12
                anchors.topMargin: 10; anchors.bottomMargin: 10
                spacing: 8

                // 레이아웃 프리셋 탭
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    Repeater {
                        model: ["2×2 전체", "스포트라이트", "단일 채널"]
                        FlatButton {
                            required property string modelData
                            required property int index
                            text: modelData
                            checked: page.layoutMode === index
                            onClicked: page.layoutMode = index
                        }
                    }
                    Item { Layout.fillWidth: true }
                    FlatButton {
                        text: "타일 모두 표시"
                        padH: 10
                        weight: Font.Normal
                        onClicked: page.hidden = [false, false, false, false]
                    }
                }

                // 그리드 (재생 모드에서는 안내 문구)
                Item {
                    id: grid
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.margins: 10      // 위젯 QGridLayout 기본 여백
                    readonly property int gap: 6

                    // 위젯 앱 relayoutGrid()와 같은 배치 규칙
                    function rectFor(ch) {
                        const shown = []
                        for (let i = 0; i < 4; ++i) if (!page.hidden[i]) shown.push(i)
                        if (shown.indexOf(ch) < 0) return null
                        const focus = page.hidden[page.selectedChannel] ? shown[0] : page.selectedChannel
                        const W = width, H = height, g = gap
                        if (page.layoutMode === 2)
                            return ch === focus ? Qt.rect(0, 0, W, H) : null
                        if (page.layoutMode === 1 && shown.length > 1) {
                            const others = shown.length - 1
                            const bigW = (W - g) * 3 / 4
                            if (ch === focus) return Qt.rect(0, 0, bigW, H)
                            const rows = shown.filter(c => c !== focus)
                            const r = rows.indexOf(ch)
                            const h = (H - g * (others - 1)) / others
                            return Qt.rect(bigW + g, r * (h + g), W - bigW - g, h)
                        }
                        const cols = shown.length <= 1 ? 1 : 2
                        const rowsN = Math.ceil(shown.length / cols)
                        const i = shown.indexOf(ch)
                        const cw = (W - g * (cols - 1)) / cols
                        const rh = (H - g * (rowsN - 1)) / rowsN
                        return Qt.rect((i % cols) * (cw + g), Math.floor(i / cols) * (rh + g), cw, rh)
                    }

                    Repeater {
                        model: backend.channels
                        VideoTile {
                            id: vt
                            readonly property var r: {
                                grid.width; grid.height; page.layoutMode; page.selectedChannel; page.hidden
                                return grid.rectFor(channel)
                            }
                            visible: !page.playbackMode && r !== null
                            x: r ? r.x : 0; y: r ? r.y : 0
                            width: r ? r.width : 0; height: r ? r.height : 0
                            selected: page.selectedChannel === channel
                            onPicked: page.selectChannel(channel)
                            onCloseRequested: page.setHidden(channel, true)
                            onZoomRequested: page.layoutMode = page.layoutMode === 2 ? 0 : 2
                        }
                    }

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -10    // 재생 안내는 여백 없이 꽉 (위젯 QStackedWidget 페이지)
                        visible: page.playbackMode
                        color: Theme.videoSurface
                        VideoOutput { id: pbVideo; anchors.fill: parent; visible: page.pbHint === "" }
                        Text {
                            visible: page.pbHint !== ""
                            anchors.centerIn: parent
                            horizontalAlignment: Text.AlignHCenter
                            lineHeight: 20; lineHeightMode: Text.FixedHeight
                            text: page.pbHint
                            color: Theme.sub
                            font.pixelSize: 13
                        }
                    }
                }

                // 트랜스포트 + 타임라인
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: tRow.implicitHeight + 12
                        color: Theme.card
                        border.color: Theme.border
                        radius: 4
                        // 아래쪽은 타임라인과 붙는다 — 아래 모서리·테두리를 덮는다
                        Rectangle {
                            anchors.bottom: parent.bottom
                            x: 1; width: parent.width - 2; height: 5
                            color: Theme.card
                        }
                        RowLayout {
                            id: tRow
                            anchors.fill: parent
                            anchors.leftMargin: 10; anchors.rightMargin: 10
                            spacing: 6
                            Repeater {
                                model: [{ g: "⏮", d: -600000 }, { g: "⏪", d: -10000 }, { g: "▶", d: 0 }, { g: "⏩", d: 10000 }, { g: "⏭", d: 600000 }]
                                FlatButton {
                                    required property var modelData
                                    implicitWidth: 30; implicitHeight: 26
                                    text: modelData.d === 0 && player.playbackState === MediaPlayer.PlayingState ? "⏸" : modelData.g
                                    onClicked: {
                                        if (!page.playbackMode) return
                                        if (modelData.d === 0) { if (player.playbackState === MediaPlayer.PlayingState) player.pause(); else player.play() }
                                        else page.seekTo(timeline.playhead + modelData.d)
                                    }
                                    px: 13; weight: Font.Normal
                                    bd: "transparent"; hoverBd: "transparent"
                                    fg: Theme.text; hoverFg: Theme.select; hoverBg: Theme.panel
                                }
                            }
                            Text {
                                id: tTime
                                leftPadding: 8
                                color: Theme.sub
                                font.pixelSize: 12; font.weight: Font.DemiBold
                                Timer {
                                    interval: 1000; running: !page.playbackMode; repeat: true
                                    triggeredOnStart: true
                                    onTriggered: tTime.text = Qt.formatDateTime(new Date(), "yyyy-MM-dd HH:mm:ss")
                                }
                            }
                            Item { Layout.fillWidth: true }
                            ComboBox {
                                id: speed
                                model: ["x0.5", "x1", "x2", "x4", "x8"]
                                currentIndex: 1
                                onActivated: { page.pbRate = [0.5, 1, 2, 4, 8][currentIndex]; player.playbackRate = page.pbRate }
                                implicitHeight: 26
                                implicitWidth: Math.max(52 + 18, contentItem.implicitWidth + 16 + 18)
                                font.pixelSize: 12
                                contentItem: Text {
                                    leftPadding: 8
                                    text: speed.displayText
                                    color: Theme.text
                                    font: speed.font
                                    verticalAlignment: Text.AlignVCenter
                                }
                                indicator: Item { width: 16 }
                                background: Rectangle { color: Theme.panel; border.color: Theme.border; radius: 3 }
                            }
                            FlatButton {
                                text: "라이브"
                                checked: !page.playbackMode
                                onClicked: page.setPlayback(false)
                            }
                            FlatButton {
                                text: "녹화 재생"
                                checked: page.playbackMode
                                onClicked: page.setPlayback(true)
                            }
                        }
                    }
                    TimelineItem {
                        id: timeline
                        Layout.fillWidth: true
                        Layout.preferredHeight: 58
                        liveMode: !page.playbackMode
                        onSeekRequested: (ms) => tTime.text = page.fmt(ms)
                        onSeekCommitted: (ms) => page.seekTo(ms)
                    }
                }
            }
        }

        // ── 웨어러블 생체신호 ──────────────────────────────
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 300
            color: Theme.panel
            border.color: Theme.border
            radius: 4

            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: 16; anchors.rightMargin: 16
                anchors.topMargin: 14; anchors.bottomMargin: 16
                spacing: 12

                PanelTitle { text: "웨어러블 생체신호" }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.rightMargin: 6
                    spacing: 10
                    Repeater {
                        model: vitals.cards
                        VitalCard {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                        }
                    }
                }
            }
        }
    }
}
