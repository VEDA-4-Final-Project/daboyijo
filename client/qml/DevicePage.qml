import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Daboyijo 1.0

// 장치 설정 — 위젯 buildDeviceSettingsTab(): [카메라 | 알림] → 카메라는 채널 스트립 | 스테이지 | 인스펙터(연결·ROI·이미지).
// 연결·검색·ROI 전송·알림 노드 제어는 모두 DeviceBackend(C++).
Item {
    id: page
    property int deviceMode: 0            // 0 카메라, 1 알림
    property int camMode: 0               // 0 연결, 1 ROI, 2 이미지
    property int cur: 0                   // 선택 채널
    property int curFrameId: 0
    property bool curLive: false
    readonly property var chInfo: device.channels.length > cur ? device.channels[cur] : ({ active: false, server: false, ip: "", beds: 0 })
    readonly property bool liveRoom: device.selectedRoom === 0
    readonly property bool camOn: liveRoom && chInfo.active
    property var curBeds: []
    property var curChoices: []
    property bool roiShown: true

    function reloadBeds() { curBeds = device.beds(cur); curChoices = device.residentChoices(cur) }
    onCurChanged: { reloadBeds(); loadImage() }
    Connections { target: device; function onChannelsChanged() { page.reloadBeds() } }
    Component.onCompleted: { reloadBeds(); loadImage() }

    function loadImage() {
        const p = device.imageParams(cur)
        bright.value = p.bright; contrast.value = p.contrast; sat.value = p.saturation
    }
    function warn(r) { if (r && !r.ok && r.message) dlg.notify("전송 실패", r.message) }

    ConfirmDialog { id: dlg }
    Popup {
        id: roomDlg
        parent: Overlay.overlay; anchors.centerIn: parent; modal: true; padding: 0; width: 360
        background: Rectangle { color: Theme.panel; border.color: Theme.border; radius: 6 }
        Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.5) }
        onOpened: { roomEdit.text = device.suggestRoom(); roomEdit.forceActiveFocus(); roomEdit.selectAll() }
        contentItem: Column {
            spacing: 10; padding: 18
            Text { text: "호실 추가"; color: Theme.text; font.pixelSize: 15; font.weight: Font.ExtraBold }
            Text { text: "추가할 호실 이름"; color: Theme.sub; font.pixelSize: 13 }
            FormEdit { id: roomEdit; width: 324; onAccepted: okBtn.clicked() }
            Row {
                anchors.right: parent.right; anchors.rightMargin: 18; spacing: 8
                FlatButton { text: "취소"; px: 13; padH: 18; padV: 6; bg: Theme.bgDeep; onClicked: roomDlg.close() }
                FlatButton { id: okBtn; text: "확인"; px: 13; padH: 18; padV: 6; weight: Font.Bold
                    bg: Theme.accent; fg: Theme.accentInk; bd: Theme.accent
                    onClicked: { const e = device.addRoom(roomEdit.text); roomDlg.close(); if (e) dlg.notify("호실 추가", e) } }
            }
        }
    }

    // 현재 채널의 영상 상태를 모델에서 끌어온다
    Repeater {
        model: backend.channels
        delegate: Item {
            required property int channel
            required property int frameId
            required property bool live
            Binding { target: page; property: "curFrameId"; value: frameId; when: channel === page.cur }
            Binding { target: page; property: "curLive"; value: live; when: channel === page.cur }
        }
    }

    ColumnLayout {
        anchors.fill: parent; anchors.leftMargin: 18; anchors.rightMargin: 18; anchors.topMargin: 14
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            SegTrack { model: ["카메라", "알림"]; current: page.deviceMode; minBtnWidth: 110; onPicked: (i) => page.deviceMode = i }
            Item { Layout.fillWidth: true }
        }

        StackLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            currentIndex: page.deviceMode

            // ═══ 카메라 ═══
            Item {
                ColumnLayout {
                    anchors.fill: parent; anchors.topMargin: 2; anchors.bottomMargin: 16; spacing: 12
                    RowLayout {
                        Layout.fillWidth: true
                        PanelTitle { text: "카메라 설정" }
                        Item { Layout.fillWidth: true }
                        SegTrack {
                            model: device.rooms; current: device.selectedRoom; minBtnWidth: 76; addButton: true
                            onPicked: (i) => device.selectedRoom = i
                            onAddClicked: roomDlg.open()
                            onContextRequested: (i) => { if (i > 0) dlg.ask("호실 삭제", device.rooms[i] + " 자리를 지울까요?", function() { device.removeRoom(i) }) }
                        }
                        Item { width: 10 }
                        SegTrack { model: ["연결", "ROI", "이미지"]; current: page.camMode; onPicked: (i) => { video.cancelDraft(); page.camMode = i } }
                    }

                    RowLayout {
                        Layout.fillWidth: true; Layout.fillHeight: true; spacing: 14

                        // 채널 스트립
                        Rectangle {
                            Layout.preferredWidth: 196; Layout.fillHeight: true
                            color: Theme.card; border.color: Theme.border; radius: 4
                            Column {
                                x: 12; y: 12; width: parent.width - 24; spacing: 10
                                Text { text: "채널"; color: Theme.sub; font.pixelSize: 11; font.weight: Font.ExtraBold; font.letterSpacing: 2; leftPadding: 2 }
                                Repeater {
                                    model: 4
                                    Rectangle {
                                        id: tile
                                        required property int index
                                        readonly property var info: device.channels.length > index ? device.channels[index] : ({})
                                        readonly property bool on: page.liveRoom && !!info.active
                                        width: parent.width; height: 126; radius: 4
                                        color: page.cur === index ? Theme.bgDeep : (tma.containsMouse ? Theme.bgDeep : "transparent")
                                        border.width: 2
                                        border.color: page.cur === index ? Theme.select : (tma.containsMouse ? Theme.border : "transparent")
                                        Item {
                                            x: 7; y: 7; width: parent.width - 14; height: parent.height - 14 - 6 - 18
                                            Rectangle { anchors.fill: parent; color: Theme.videoSurface; radius: 3 }
                                            VideoItem { anchors.fill: parent; channel: tile.index; frameId: tile.index === page.cur ? page.curFrameId : 0
                                                        live: tile.on && tile.index === page.cur && page.curLive; visible: tile.on }
                                            Text { visible: !tile.on; anchors.centerIn: parent; text: "신호 없음"; color: "#8B98A5"; font.pixelSize: 12 }
                                        }
                                        Row {
                                            x: 7; y: parent.height - 7 - 18; width: parent.width - 14; height: 18
                                            Text { text: "CH " + (tile.index + 1); color: Theme.text; font.pixelSize: 12; font.weight: Font.ExtraBold; font.letterSpacing: 0.5 }
                                            Item { width: parent.width - chT.implicitWidth - chS.implicitWidth - 2; height: 1 }
                                            Text { id: chS; text: (tile.on ? "● 연결" : "○ 미연결") + (tile.info.beds > 0 ? " · 침대 " + tile.info.beds : "")
                                                   color: tile.on ? Theme.normal : Theme.sub; font.pixelSize: 11; font.weight: Font.Bold }
                                            Text { id: chT; visible: false; text: "CH 1"; font.pixelSize: 12; font.weight: Font.ExtraBold }
                                        }
                                        MouseArea { id: tma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                                    onClicked: { video.cancelDraft(); page.cur = tile.index } }
                                    }
                                }
                            }
                        }

                        // 스테이지
                        Rectangle {
                            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 400
                            color: Theme.videoSurface; border.color: Theme.border; radius: 4; clip: true
                            VideoItem {
                                id: video
                                anchors.fill: parent; anchors.margins: 1
                                channel: page.cur; frameId: page.curFrameId; live: page.camOn && page.curLive
                                displayName: "CH " + (page.cur + 1)
                                zones: page.roiShown && page.liveRoom ? device.zones(page.cur) : []
                                editable: page.camMode === 1 && page.camOn
                                Connections { target: device; function onChannelsChanged() { video.zones = page.roiShown && page.liveRoom ? device.zones(page.cur) : [] } }
                                onRoiCompleted: (id, pts) => { page.warn(device.addZone(page.cur, id, pts)); page.reloadBeds() }
                                onZoneClicked: page.reloadBeds()
                                MouseArea {
                                    anchors.fill: parent; enabled: page.camMode === 2 && page.camOn
                                    onClicked: function(m) {
                                        page.warn(device.focusAt(page.cur, Math.max(0, Math.min(1, m.x / width)), Math.max(0, Math.min(1, m.y / height))))
                                    }
                                }
                            }
                        }

                        // 인스펙터
                        Rectangle {
                            Layout.preferredWidth: 380; Layout.fillHeight: true
                            color: Theme.card; border.color: Theme.border; radius: 4
                            Flickable {
                                anchors.fill: parent; anchors.margins: 18; anchors.topMargin: 16
                                contentHeight: insp.implicitHeight; clip: true
                                ColumnLayout {
                                    id: insp; width: parent.width; spacing: 14
                                    RowLayout {
                                        spacing: 8
                                        Text { text: "CH " + (page.cur + 1); color: Theme.text; font.pixelSize: 19; font.weight: Font.ExtraBold; font.letterSpacing: 0.5 }
                                        Rectangle {
                                            implicitWidth: pillT.implicitWidth + 20; implicitHeight: pillT.implicitHeight + 4; radius: 3; color: "transparent"
                                            border.color: page.camOn ? Theme.normal : Theme.sub
                                            Text { id: pillT; anchors.centerIn: parent; text: page.camOn ? "● 연결됨" : "○ 미연결"
                                                   color: parent.border.color; font.pixelSize: 11; font.weight: Font.ExtraBold }
                                        }
                                        Item { Layout.fillWidth: true }
                                    }
                                    Text { text: page.camOn && page.chInfo.ip !== "" ? page.chInfo.ip + " · RTSP profile2" : "카메라가 연결되지 않았습니다"
                                           color: Theme.sub; font.pixelSize: 11; font.family: Theme.mono }
                                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

                                    // ── 연결 ──
                                    ColumnLayout {
                                        visible: page.camMode === 0; Layout.fillWidth: true; spacing: 12
                                        Cap { text: "카메라 접속" }
                                        Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.sub; font.pixelSize: 12
                                               text: "이 카메라는 센서 4개가 한 몸입니다 — IP 하나로 4채널이 함께 연결되고 함께 해제됩니다. 왼쪽 채널 선택은 ROI·이미지 탭에서만 적용됩니다." }
                                        GridLayout {
                                            Layout.fillWidth: true; columns: 2; columnSpacing: 12; rowSpacing: 10
                                            Text { text: "CCTV IP"; color: Theme.text; font.pixelSize: 13; font.weight: Font.DemiBold }
                                            FormEdit { id: ipEdit; Layout.fillWidth: true; text: device.savedIp
                                                       placeholderText: "예: 172.20.32.31  (아래 검색 결과를 클릭하면 자동 입력)" }
                                            Text { text: "계정"; color: Theme.text; font.pixelSize: 13; font.weight: Font.DemiBold }
                                            FormEdit { id: userEdit; Layout.fillWidth: true; text: device.savedUser }
                                            Text { text: "비밀번호"; color: Theme.text; font.pixelSize: 13; font.weight: Font.DemiBold }
                                            FormEdit { id: pwEdit; Layout.fillWidth: true; echoMode: TextInput.Password; placeholderText: "CCTV 비밀번호" }
                                        }
                                        FlatButton { Layout.fillWidth: true; text: "🔍 같은 망 카메라 검색"; px: 12; weight: Font.DemiBold; padV: 7
                                                     bg: Theme.card; fg: Theme.text; onClicked: device.searchCameras(ipEdit.text) }
                                        RowLayout {
                                            spacing: 8
                                            FlatButton { Layout.fillWidth: true; Layout.preferredWidth: 2; text: "📷 4채널 연결"; px: 12; weight: Font.ExtraBold; padV: 9
                                                bg: Theme.accent; fg: Theme.accentInk; bd: Theme.accent
                                                hoverBg: Theme.accentHover; hoverFg: Theme.accentInk; hoverBd: Theme.accentHover
                                                onClicked: { const r = device.connectCameras(ipEdit.text, userEdit.text, pwEdit.text)
                                                             dlg.notify(r.ok ? "카메라 연결 요청" : "카메라 연결", r.message) } }
                                            FlatButton { Layout.fillWidth: true; Layout.preferredWidth: 1; text: "전체 해제"; px: 12; padV: 9
                                                bg: Theme.card; fg: Theme.text; hoverBd: Theme.critical; hoverFg: Theme.critical
                                                onClicked: {
                                                    if (!device.anyCameraActive()) { dlg.notify("카메라 해제", "연결된 카메라가 없습니다."); return }
                                                    dlg.ask("카메라 해제", "모든 채널의 카메라 연결을 해제할까요?\n서버가 RTSP 연결을 끊고 대기 상태로 돌아갑니다.",
                                                            function() { device.clearCameras() })
                                                } }
                                        }
                                        Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.sub; font.pixelSize: 12
                                               text: device.searchStatus !== "" ? device.searchStatus : "‘검색’을 누르면 같은 망의 카메라가 아래에 나타납니다. 행을 클릭하면 IP가 채워져요." }
                                        Rectangle {
                                            visible: device.found.length > 0
                                            Layout.fillWidth: true; implicitHeight: 28 + device.found.length * 32 + 2
                                            color: Theme.panel; border.color: Theme.border; radius: 4; clip: true
                                            Column {
                                                x: 1; y: 1; width: parent.width - 2
                                                Rectangle {
                                                    width: parent.width; height: 28; color: Theme.bgDeep
                                                    Row { x: 8; anchors.verticalCenter: parent.verticalCenter
                                                        Text { width: 130; text: "모델"; color: Theme.sub; font.pixelSize: 12; font.weight: Font.Bold }
                                                        Text { width: 100; text: "IP"; color: Theme.sub; font.pixelSize: 12; font.weight: Font.Bold }
                                                        Text { text: "MAC / ID"; color: Theme.sub; font.pixelSize: 12; font.weight: Font.Bold } }
                                                }
                                                Repeater {
                                                    model: device.found
                                                    Rectangle {
                                                        required property var modelData
                                                        width: parent.width; height: 32
                                                        color: fma.containsMouse ? Theme.card : "transparent"
                                                        Row { x: 8; anchors.verticalCenter: parent.verticalCenter
                                                            Text { width: 130; text: modelData.model; color: Theme.text; font.pixelSize: 12; elide: Text.ElideRight }
                                                            Text { width: 100; text: modelData.ip; color: Theme.text; font.pixelSize: 12 }
                                                            Text { text: modelData.mac; color: Theme.sub; font.pixelSize: 12 } }
                                                        MouseArea { id: fma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                                                    onClicked: ipEdit.text = modelData.ip }
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    // ── ROI ──
                                    ColumnLayout {
                                        visible: page.camMode === 1; Layout.fillWidth: true; spacing: 14
                                        Rectangle {
                                            Layout.fillWidth: true; implicitHeight: steps.implicitHeight + 24
                                            color: Theme.bgDeep; border.color: Theme.border; radius: 4
                                            Column {
                                                id: steps; x: 14; y: 12; width: parent.width - 28; spacing: 11
                                                Repeater {
                                                    model: ["아래 ‘침대 추가’를 누릅니다.", "오른쪽 영상 위를 클릭해 침대 모서리를 찍습니다.",
                                                            "더블클릭(또는 우클릭)으로 완료합니다.", "아래 목록에서 그 침대의 입소자를 지정합니다."]
                                                    Row {
                                                        required property string modelData
                                                        required property int index
                                                        width: steps.width
                                                        spacing: 10
                                                        Rectangle { width: 22; height: 22; radius: 11; color: Theme.accent
                                                            Text { anchors.centerIn: parent; text: index + 1; color: Theme.accentInk; font.pixelSize: 11; font.weight: Font.ExtraBold } }
                                                        Text { width: steps.width - 32; anchors.verticalCenter: parent.verticalCenter
                                                               text: modelData; wrapMode: Text.Wrap; color: Theme.text; font.pixelSize: 12 }
                                                    }
                                                }
                                            }
                                        }
                                        FlatButton {
                                            Layout.fillWidth: true; Layout.preferredHeight: 38
                                            text: video.drawMode ? "그리기 취소" : "침대 추가"; px: 13; weight: Font.ExtraBold
                                            enabled: page.camOn; opacity: page.camOn ? 1 : 0.5
                                            bg: page.camOn ? Theme.accent : Theme.border; fg: page.camOn ? Theme.accentInk : Theme.sub; bd: bg
                                            onClicked: {
                                                if (!page.camOn) return
                                                if (video.drawMode) { video.cancelDraft(); return }
                                                if (video.nextFreeZoneId() < 0) { dlg.notify("침대 추가", "채널 " + (page.cur + 1) + "에는 침대를 최대 8개까지 지정할 수 있습니다.\n기존 침대를 제거한 뒤 다시 시도하세요."); return }
                                                video.drawMode = true
                                            }
                                        }
                                        RowLayout {
                                            spacing: 8
                                            FlatButton { Layout.fillWidth: true; Layout.preferredWidth: 1; text: "침대 제거"; px: 12; padV: 7
                                                bg: Theme.card; fg: Theme.text; hoverBd: Theme.critical; hoverFg: Theme.critical
                                                onClicked: {
                                                    video.cancelDraft()
                                                    const sel = video.selectedZone
                                                    const q = device.removeBedPrompt(page.cur, sel)
                                                    if (q === "") { dlg.notify("침대 제거", "채널 " + (page.cur + 1) + "에 제거할 침대가 없습니다."); return }
                                                    dlg.ask("침대 제거", q, function() { page.warn(device.removeBed(page.cur, sel)); video.selectedZone = -1; page.reloadBeds() })
                                                } }
                                            FlatButton { Layout.fillWidth: true; Layout.preferredWidth: 1; text: page.roiShown ? "영상에 표시" : "표시 숨김"; px: 12; padV: 7
                                                bg: Theme.card; fg: Theme.text; checked: page.roiShown; onClicked: page.roiShown = !page.roiShown } 
                                        }
                                        Text { text: "침대 · 입소자 매핑"; color: Theme.text; font.pixelSize: 12; font.weight: Font.DemiBold }
                                        Rectangle {
                                            Layout.fillWidth: true; implicitHeight: bedCol.implicitHeight + 20
                                            color: Theme.bgDeep; border.color: Theme.border; radius: 4
                                            Column {
                                                id: bedCol; x: 12; y: 10; width: parent.width - 24; spacing: 8
                                                Text { visible: page.curBeds.length === 0; width: parent.width; wrapMode: Text.Wrap; color: Theme.text; font.pixelSize: 12
                                                       text: "아직 지정된 침대가 없습니다.\n‘침대 추가’로 침대마다 영역을 그려 주세요." }
                                                Repeater {
                                                    model: page.curBeds
                                                    Rectangle {
                                                        id: bedRow
                                                        required property var modelData
                                                        width: parent.width; height: 34; radius: 3
                                                        color: video.selectedZone === modelData.id ? Theme.card : "transparent"
                                                        Rectangle { visible: video.selectedZone === modelData.id; width: 3; height: parent.height; color: Theme.select }
                                                        RowLayout {
                                                            anchors.fill: parent; anchors.leftMargin: 6; spacing: 8
                                                            Rectangle { Layout.preferredWidth: 22; Layout.preferredHeight: 22; radius: 11
                                                                color: ["#2F9E8F","#5D8FD6","#D6953D","#B27AC9","#60B060","#D66A7A","#7AB0C2","#B8A060"][bedRow.modelData.id % 8]
                                                                Text { anchors.centerIn: parent; text: bedRow.modelData.id + 1; color: Theme.text; font.pixelSize: 11; font.weight: Font.Bold } }
                                                            DropBox {
                                                                Layout.fillWidth: true; implicitHeight: 30; fieldBg: Theme.panel
                                                                textRole: "name"; valueRole: "id"; model: page.curChoices
                                                                currentIndex: Math.max(0, indexOfValue(bedRow.modelData.residentId))
                                                                onActivated: { page.warn(device.bindBed(page.cur, bedRow.modelData.id, currentValue)); page.reloadBeds() }
                                                            }
                                                            FlatButton { text: "삭제"; px: 12; padV: 4; bg: Theme.card; fg: Theme.text; hoverBd: Theme.critical; hoverFg: Theme.critical
                                                                onClicked: { video.selectedZone = bedRow.modelData.id
                                                                    dlg.ask("침대 제거", device.removeBedPrompt(page.cur, bedRow.modelData.id),
                                                                            function() { page.warn(device.removeBed(page.cur, bedRow.modelData.id)); video.selectedZone = -1; page.reloadBeds() }) } }
                                                        }
                                                        MouseArea { anchors.fill: parent; z: -1; onClicked: video.selectedZone = bedRow.modelData.id }
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    // ── 이미지 ──
                                    ColumnLayout {
                                        visible: page.camMode === 2; Layout.fillWidth: true; spacing: 12
                                        Cap { text: "밝기 · 대비 · 채도" }
                                        SliderRow { id: bright; Layout.fillWidth: true; label: "밝기"; active: page.camOn }
                                        SliderRow { id: contrast; Layout.fillWidth: true; label: "대비"; active: page.camOn }
                                        SliderRow { id: sat; Layout.fillWidth: true; label: "채도"; active: page.camOn }
                                        Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.sub; font.pixelSize: 12
                                               text: "값은 이 PC에 채널별로 기억됩니다. 카메라가 지금 실제로 어떤 값인지를 읽어오는 것은 아니므로, 다른 곳에서 바꿨을 때는 다를 수 있습니다." }
                                        RowLayout {
                                            Item { Layout.fillWidth: true }
                                            FlatButton { text: "초기화"; px: 12; padV: 7; padH: 15; bg: Theme.card; fg: Theme.text; enabled: page.camOn; opacity: page.camOn ? 1 : 0.5
                                                         hoverBd: Theme.critical; hoverFg: Theme.critical
                                                         onClicked: { if (page.camOn) { bright.value = 50; contrast.value = 50; sat.value = 50 } } }
                                            FlatButton { text: "적용"; px: 12; weight: Font.ExtraBold; padV: 8; padH: 16; enabled: page.camOn; opacity: page.camOn ? 1 : 0.5
                                                         bg: page.camOn ? Theme.accent : Theme.border; fg: page.camOn ? Theme.accentInk : Theme.sub; bd: bg
                                                         onClicked: if (page.camOn) page.warn(device.applyImage(page.cur, bright.value, contrast.value, sat.value)) }
                                        }
                                        Cap { text: "초점" }
                                        FlatButton { Layout.fillWidth: true; text: "전체 자동초점"; px: 12; weight: Font.DemiBold; padV: 7; bg: Theme.card; fg: Theme.text
                                                     enabled: page.camOn; opacity: page.camOn ? 1 : 0.5
                                                     onClicked: if (page.camOn) page.warn(device.focusAll(page.cur)) }
                                        Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.sub; font.pixelSize: 12
                                               text: "💡 가운데 영상을 클릭하면 그 지점에 초점을 맞춥니다." }
                                        Text { visible: !page.camOn; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.warn; font.pixelSize: 12
                                               text: "이 채널에 카메라가 연결되어 있지 않아 조절할 수 없습니다. 위의 [연결] 탭에서 먼저 카메라를 붙여 주세요." }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ═══ 알림 ═══
            Item {
                id: alertPage
                property string node: device.alertNodes[0]
                readonly property var st: device.alertSettings(node)
                ColumnLayout {
                    anchors.fill: parent; anchors.topMargin: 2; anchors.bottomMargin: 16; spacing: 12
                    PanelTitle { text: "알림 설정" }
                    RowLayout {
                        Layout.fillWidth: true; Layout.fillHeight: true; spacing: 14
                        Rectangle {
                            Layout.fillWidth: true; Layout.fillHeight: true
                            color: Theme.videoSurface; border.color: Theme.border; radius: 4
                            Column {
                                anchors.fill: parent; anchors.margins: 12; spacing: 8
                                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "LED 미리보기 (64×32)"
                                       color: Theme.sub; font.pixelSize: 11; font.weight: Font.ExtraBold; font.letterSpacing: 1 }
                                AlertMatrixItem {
                                    id: matrix
                                    width: parent.width; height: parent.height - 30
                                    text: device.alertTestText()
                                    brightness: Math.round(76 + (255 - 76) * abright.value / 100)
                                }
                            }
                        }
                        Rectangle {
                            Layout.preferredWidth: 380; Layout.fillHeight: true
                            color: Theme.card; border.color: Theme.border; radius: 4
                            ColumnLayout {
                                anchors.fill: parent; anchors.margins: 18; anchors.topMargin: 16; spacing: 14
                                RowLayout {
                                    spacing: 8
                                    Text { text: "대상 노드"; color: Theme.text; font.pixelSize: 19; font.weight: Font.ExtraBold }
                                    Rectangle {
                                        readonly property string s: device.alertStatus
                                        implicitWidth: nb.implicitWidth + 20; implicitHeight: nb.implicitHeight + 4; radius: 3; color: "transparent"
                                        border.color: s === "online" ? Theme.normal : s === "offline" ? Theme.critical : Theme.border
                                        Text { id: nb; anchors.centerIn: parent; font.pixelSize: 11; font.weight: Font.ExtraBold
                                               text: parent.s === "online" ? "✓ 온라인" : parent.s === "offline" ? "✖ 오프라인" : "○ 상태 미확인"
                                               color: parent.border.color === Theme.border ? Theme.sub : parent.border.color }
                                    }
                                    Item { Layout.fillWidth: true }
                                }
                                DropBox { Layout.fillWidth: true; fieldBg: Theme.panel; model: device.alertNodes }
                                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
                                Cap { text: "밝기 · 음량" }
                                SliderRow { id: abright; Layout.fillWidth: true; label: "LED 밝기"; value: alertPage.st.brightness }
                                SliderRow { id: avol; Layout.fillWidth: true; label: "스피커 음량"; value: alertPage.st.volume }
                                RowLayout {
                                    Item { Layout.fillWidth: true }
                                    FlatButton { text: "테스트"; px: 12; padV: 7; padH: 15; bg: Theme.card; fg: Theme.text; hoverBd: Theme.critical; hoverFg: Theme.critical
                                                 onClicked: if (!device.testAlert(alertPage.node, abright.value, avol.value)) dlg.notify("테스트", "알림 노드에 연결되어 있지 않습니다.") }
                                    FlatButton { text: "적용"; px: 12; weight: Font.ExtraBold; padV: 8; padH: 16
                                                 bg: Theme.accent; fg: Theme.accentInk; bd: Theme.accent
                                                 onClicked: if (!device.applyAlert(alertPage.node, abright.value, avol.value)) dlg.notify("적용", "알림 노드에 연결되어 있지 않습니다.") }
                                }
                                Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.sub; font.pixelSize: 12; text: "💡 테스트는 저장하지 않고 지금 값으로 잠깐 보여줍니다." }
                                Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.sub; font.pixelSize: 12; text: "💡 적용은 지금 값을 노드에 바로 반영하고 저장합니다." }
                                Item { Layout.fillHeight: true }
                                Text { text: device.lastApplied; color: Theme.sub; font.pixelSize: 12 }
                            }
                        }
                    }
                }
            }
        }
    }

    component Cap: Item {
        property alias text: capT.text
        implicitHeight: capT.implicitHeight + 2; Layout.fillWidth: true
        Rectangle { width: 3; height: parent.height; color: Theme.accent }
        Text { id: capT; x: 11; anchors.verticalCenter: parent.verticalCenter; color: Theme.text
               font.pixelSize: 12; font.weight: Font.ExtraBold; font.letterSpacing: 0.5 }
    }
}
