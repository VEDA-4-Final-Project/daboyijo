import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Daboyijo 1.0

// 입소자 관리 — 위젯 buildDbTab() 과 같은 배치. 모든 DB 처리는 ResidentBackend(C++).
Rectangle {
    id: page
    color: Theme.panel
    border.color: Theme.border
    radius: 4

    readonly property var f: residents.form
    property string risk: "중"
    property string status: "재원"
    readonly property bool isNew: residents.selectedId < 0
    readonly property bool editing: residents.selectedId !== 0

    function sevColor(s) {
        return s === "critical" ? Theme.critical : s === "medium" ? Theme.warn
             : s === "normal" ? Theme.normal : Theme.sub
    }
    function riskSev(r) { return r === "상" ? "critical" : r === "중" ? "medium" : "normal" }

    Connections {
        target: residents
        function onSelectionChanged() {
            const v = residents.form
            nameEdit.text = v.name || ""; roomEdit.text = v.room || ""
            camEdit.text = v.camera || ""; wearEdit.text = v.wearable || ""
            admEdit.text = v.admittedAt || ""; dueEdit.text = v.dischargeDue || ""
            notesEdit.text = v.notes || ""
            page.risk = v.risk || "중"; page.status = v.status || "재원"
        }
    }
    Component.onCompleted: residents.refresh()

    function collect() {
        return { name: nameEdit.text, room: roomEdit.text, camera: camEdit.text, wearable: wearEdit.text,
                 risk: page.risk, admittedAt: admEdit.text, dischargeDue: dueEdit.text,
                 status: page.status, notes: notesEdit.text }
    }

    ConfirmDialog { id: dlg }

    // 변경 내역 팝업
    Popup {
        id: logPop
        property var rows: []
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true; padding: 16
        width: Math.min(820, page.width - 60); height: 480
        background: Rectangle { color: Theme.panel; border.color: Theme.border; radius: 6 }
        Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.5) }
        ColumnLayout {
            anchors.fill: parent; spacing: 10
            Text { text: "변경 내역"; color: Theme.text; font.pixelSize: 15; font.weight: Font.ExtraBold }
            Text { visible: logPop.rows.length === 0; text: "이 입원 기간에 기록된 변경 내역이 없습니다."
                   color: Theme.sub; font.pixelSize: 11; font.weight: Font.ExtraBold }
            Rectangle {
                Layout.fillWidth: true; height: 28; color: Theme.bgDeep
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 8
                    Repeater {
                        model: [["시각", 150], ["구분", 50], ["항목", 90], ["변경 내용", -1], ["작업자", 70]]
                        Text { required property var modelData
                               Layout.preferredWidth: modelData[1]; Layout.fillWidth: modelData[1] < 0
                               text: modelData[0]; color: Theme.sub; font.pixelSize: 12; font.weight: Font.Bold }
                    }
                }
            }
            ListView {
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                model: logPop.rows
                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    width: ListView.view.width; height: 30
                    color: index % 2 ? "transparent" : Qt.rgba(1, 1, 1, Theme.dark ? 0.02 : 0.4)
                    RowLayout {
                        anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 8
                        Text { Layout.preferredWidth: 150; text: modelData.at; color: Theme.text; font.pixelSize: 12; elide: Text.ElideRight }
                        Text { Layout.preferredWidth: 50; text: modelData.type; color: Theme.text; font.pixelSize: 12 }
                        Text { Layout.preferredWidth: 90; text: modelData.field; color: Theme.text; font.pixelSize: 12 }
                        Text { Layout.fillWidth: true; text: modelData.change; color: Theme.text; font.pixelSize: 12; elide: Text.ElideRight }
                        Text { Layout.preferredWidth: 70; text: modelData.by; color: Theme.text; font.pixelSize: 12; elide: Text.ElideRight }
                    }
                }
            }
            RowLayout {
                Item { Layout.fillWidth: true }
                FlatButton { text: "닫기"; px: 13; padH: 18; padV: 6; bg: Theme.bgDeep; onClicked: logPop.close() }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 18
        anchors.topMargin: 16; anchors.bottomMargin: 16
        spacing: 12

        // 제목 + DB 상태
        RowLayout {
            Layout.fillWidth: true
            PanelTitle { text: "입소자 관리" }
            Item { Layout.fillWidth: true }
            Rectangle { width: 9; height: 9; radius: 5; color: App.dbConnected ? Theme.normal : Theme.critical }
            Text { text: App.dbConnected ? "DB 연결됨 · daboijo" : "DB 연결 안 됨"
                   color: Theme.sub; font.pixelSize: 12 }
        }

        // 요약 통계
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            // 모델은 고정(5칸) — 값만 델리게이트에서 residents.summary 를 읽는다
            // (모델 자체를 바인딩하면 갱신 때 델리게이트가 다시 만들어져 레이아웃이 깨진다)
            Repeater {
                model: 5
                Rectangle {
                    id: stat
                    required property int index
                    readonly property var spec: [
                        { cap: "재원", key: "active", c: Theme.text },
                        { cap: "위험 상", key: "high", c: Theme.critical },
                        { cap: "위험 중", key: "mid", c: Theme.warn },
                        { cap: "위험 하", key: "low", c: Theme.normal },
                        { cap: "채널 배정", key: "cams", c: Theme.text }][index]
                    Layout.fillWidth: true; Layout.preferredWidth: 1
                    implicitHeight: 13 + 12 + 34 + 3 + capT.implicitHeight
                    color: Theme.card; border.color: Theme.border; radius: 4
                    Column {
                        x: 16; y: 13; spacing: 3
                        Text { height: 34; verticalAlignment: Text.AlignVCenter
                               text: residents.summary[stat.spec.key] === undefined ? "0" : String(residents.summary[stat.spec.key])
                               color: stat.spec.c; font.family: Theme.mono; font.pixelSize: 26; font.weight: Font.ExtraBold }
                        Text { id: capT; text: stat.spec.cap; color: Theme.sub; font.pixelSize: 12; font.weight: Font.Bold }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 16

            // ── 좌측 목록 ──
            Rectangle {
                Layout.preferredWidth: 344
                Layout.fillHeight: true
                color: Theme.card; border.color: Theme.border; radius: 4
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 12; spacing: 10
                    RowLayout {
                        Layout.fillWidth: true; spacing: 0
                        Repeater {
                            model: ["재원", "전체", "퇴원"]
                            FlatButton {
                                required property string modelData
                                Layout.fillWidth: true; Layout.preferredWidth: 1
                                text: modelData; px: 12; weight: Font.Bold; padV: 6
                                radius: 0
                                bg: Theme.bgDeep
                                checked: residents.filter === modelData
                                onClicked: residents.filter = modelData
                            }
                        }
                    }
                    FormEdit {
                        id: searchEdit
                        Layout.fillWidth: true; implicitHeight: 34
                        placeholderText: "🔍  이름 검색"
                        onTextChanged: residents.search = text
                    }
                    FlatButton {
                        Layout.fillWidth: true
                        text: "＋ 신규 등록"; px: 13; weight: Font.Bold; padV: 7
                        bg: Theme.accent; fg: Theme.accentInk; bd: Theme.accent
                        hoverBg: Theme.accentHover; hoverFg: Theme.accentInk; hoverBd: Theme.accentHover
                        onClicked: { residents.openResident(-1); nameEdit.forceActiveFocus() }
                    }
                    Text { text: residents.countText; color: Theme.sub; font.pixelSize: 11; font.weight: Font.ExtraBold; font.letterSpacing: 1 }
                    ListView {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        clip: true; spacing: 8
                        model: residents.residents
                        ScrollBar.vertical: ScrollBar { }
                        delegate: Rectangle {
                            required property var modelData
                            readonly property bool sel: modelData.id === residents.selectedId
                            width: ListView.view.width - 6; height: 64
                            radius: 4
                            color: sel ? Theme.panel : (modelData.active ? Theme.bgDeep : "transparent")
                            border.width: sel ? 2 : 1
                            border.color: (sel || rowMa.containsMouse) ? Theme.select : Theme.border
                            Rectangle { x: 8; y: 8; width: 4; height: parent.height - 16; radius: 2
                                        color: page.sevColor(modelData.severity) }
                            Column {
                                x: 22; anchors.verticalCenter: parent.verticalCenter; spacing: 1
                                Text { text: modelData.name; color: Theme.text; font.pixelSize: 15; font.weight: Font.ExtraBold }
                                Text { text: modelData.meta; color: Theme.sub; font.pixelSize: 12 }
                            }
                            Rectangle {
                                anchors.right: parent.right; anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                width: chip.implicitWidth + 18; height: chip.implicitHeight + 4
                                radius: 3; color: "transparent"
                                border.color: modelData.active ? page.sevColor(modelData.severity) : Theme.sub
                                Text { id: chip; anchors.centerIn: parent
                                       text: (modelData.active ? (modelData.severity === "critical" ? "▲ " : modelData.severity === "medium" ? "◆ " : "● ") : "") + modelData.chip
                                       color: parent.border.color; font.pixelSize: 11; font.weight: Font.ExtraBold }
                            }
                            MouseArea { id: rowMa; anchors.fill: parent; hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: residents.openResident(modelData.id) }
                        }
                    }
                }
            }

            // ── 우측 상세 ──
            Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true
                color: Theme.card; border.color: Theme.border; radius: 4

                Text {
                    visible: !page.editing
                    anchors.centerIn: parent
                    horizontalAlignment: Text.AlignHCenter
                    text: "왼쪽 목록에서 입소자를 선택하거나\n＋ 신규 등록을 눌러 시작하세요."
                    color: Theme.sub; font.pixelSize: 14
                }

                ColumnLayout {
                    visible: page.editing
                    anchors.fill: parent; anchors.margins: 16; spacing: 14

                    // 헤더
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 12 + 12 + hCol.implicitHeight
                        color: Theme.panel; border.color: Theme.border; radius: 4
                        RowLayout {
                            anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 16; spacing: 14
                            Column {
                                id: hCol; spacing: 2
                                Text { text: page.isNew ? "신규 입소자" : (nameEdit.text.trim() === "" ? "(이름 없음)" : nameEdit.text.trim())
                                       color: Theme.text; font.pixelSize: 19; font.weight: Font.ExtraBold }
                                Text { text: camEdit.text.trim() === "" ? "채널 미지정" : "채널 " + camEdit.text.trim()
                                       color: Theme.sub; font.pixelSize: 13 }
                            }
                            Item { Layout.fillWidth: true }
                            Rectangle {
                                visible: !page.isNew
                                implicitWidth: rb.implicitWidth + 20; implicitHeight: rb.implicitHeight + 6
                                radius: 3; color: "transparent"; border.color: page.sevColor(page.riskSev(page.risk))
                                Text { id: rb; anchors.centerIn: parent
                                       text: (page.risk === "상" ? "▲ " : page.risk === "중" ? "◆ " : "● ") + "위험 " + page.risk
                                       color: parent.border.color; font.pixelSize: 12; font.weight: Font.ExtraBold }
                            }
                            Rectangle {
                                visible: !page.isNew
                                implicitWidth: sb.implicitWidth + 20; implicitHeight: sb.implicitHeight + 6
                                radius: 3; color: "transparent"
                                border.color: page.status === "재원" ? Theme.normal : Theme.sub
                                Text { id: sb; anchors.centerIn: parent; text: page.status
                                       color: parent.border.color; font.pixelSize: 12; font.weight: Font.ExtraBold }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true; Layout.fillHeight: true; spacing: 16

                        // 폼
                        Flickable {
                            Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 3
                            contentHeight: formCol.implicitHeight; clip: true
                            ScrollBar.vertical: ScrollBar { }
                            Column {
                                id: formCol
                                width: parent.width - 6
                                spacing: 12

                                component Group: Rectangle {
                                    id: g
                                    property string title
                                    default property alias body: bodyCol.data
                                    width: parent.width
                                    implicitHeight: bodyCol.implicitHeight + 40
                                    color: "transparent"; border.color: Theme.border; radius: 4
                                    Text { x: 12; y: 8; text: g.title; color: Theme.sub; font.pixelSize: 12; font.weight: Font.Bold }
                                    Column { id: bodyCol; x: 14; y: 30; width: parent.width - 28; spacing: 8 }
                                }
                                component Row2: RowLayout {
                                    property string label
                                    default property alias content: slot.data
                                    width: parent.width; spacing: 10
                                    Text { Layout.preferredWidth: 88; text: parent.label; color: Theme.sub; font.pixelSize: 13 }
                                    Item { id: slot; Layout.fillWidth: true; implicitHeight: childrenRect.height
                                           onChildrenRectChanged: { if (children.length) children[0].width = width } }
                                }

                                Group {
                                    title: "기본 정보"
                                    Row2 { label: "이름";        FormEdit { id: nameEdit; placeholderText: "홍길동" } }
                                    Row2 { label: "호실";        FormEdit { id: roomEdit; placeholderText: "예: 101" } }
                                    Row2 { label: "카메라 채널"; FormEdit { id: camEdit; placeholderText: "1~4" } }
                                    Row2 { label: "웨어러블 ID"; FormEdit { id: wearEdit; placeholderText: "기기 번호" } }
                                }
                                Group {
                                    title: "케어 정보"
                                    Row2 {
                                        label: "위험도"
                                        Row { spacing: 6
                                            Repeater { model: ["상", "중", "하"]
                                                FlatButton { required property string modelData
                                                    text: modelData; px: 13; padH: 16; padV: 5; bg: Theme.bgDeep
                                                    checked: page.risk === modelData
                                                    onClicked: page.risk = modelData } } }
                                    }
                                    Row2 { label: "입원일";      FormEdit { id: admEdit; placeholderText: "yyyy-MM-dd" } }
                                    Row2 { label: "퇴원 예정일"; FormEdit { id: dueEdit; placeholderText: "yyyy-MM-dd" } }
                                    Row2 {
                                        label: "상태"
                                        Row { spacing: 6
                                            Repeater { model: ["재원", "퇴원"]
                                                FlatButton { required property string modelData
                                                    text: modelData; px: 13; padH: 16; padV: 5; bg: Theme.bgDeep
                                                    checked: page.status === modelData
                                                    onClicked: page.status = modelData } } }
                                    }
                                }
                                Group {
                                    title: "특이사항"
                                    Rectangle {
                                        width: parent.width; height: 80
                                        radius: 4; color: Theme.bgDeep
                                        border.color: notesEdit.activeFocus ? Theme.select : Theme.border
                                        TextArea {
                                            id: notesEdit
                                            anchors.fill: parent
                                            color: Theme.text; font.pixelSize: 13
                                            placeholderText: "지병, 알레르기, 거동 가능 여부 등"
                                            placeholderTextColor: Theme.sub
                                            wrapMode: TextEdit.Wrap; selectByMouse: true
                                            background: null
                                        }
                                    }
                                }
                            }
                        }

                        // 입원 이력
                        Rectangle {
                            visible: !page.isNew
                            Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 2
                            color: "transparent"; border.color: Theme.border; radius: 4
                            Text { x: 12; y: 8; text: "입원 이력"; color: Theme.sub; font.pixelSize: 12; font.weight: Font.Bold }
                            ColumnLayout {
                                anchors.fill: parent; anchors.topMargin: 30; anchors.margins: 12; spacing: 6
                                Text {
                                    Layout.fillWidth: true; wrapMode: Text.Wrap
                                    text: residents.admissions.length > 0
                                        ? "입원 " + residents.admissions.length + "건 — 행 더블클릭 시 변경 내역"
                                        : "입소자를 선택하면 입원 이력이 표시됩니다."
                                    color: Theme.sub; font.pixelSize: 11; font.weight: Font.ExtraBold
                                }
                                Rectangle {
                                    Layout.fillWidth: true; height: 26; color: Theme.bgDeep
                                    RowLayout {
                                        anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8
                                        Repeater { model: ["입원일", "퇴원일", "상태", "변경"]
                                            Text { required property string modelData; Layout.fillWidth: true; Layout.preferredWidth: 1
                                                   text: modelData; color: Theme.sub; font.pixelSize: 12; font.weight: Font.Bold } }
                                    }
                                }
                                ListView {
                                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                                    model: residents.admissions
                                    delegate: Rectangle {
                                        required property var modelData
                                        width: ListView.view.width; height: 30
                                        color: admMa.containsMouse ? Theme.card : "transparent"
                                        RowLayout {
                                            anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8
                                            Repeater { model: [modelData.in, modelData.out, modelData.status, modelData.changes]
                                                Text { required property string modelData; Layout.fillWidth: true; Layout.preferredWidth: 1
                                                       text: modelData; color: Theme.text; font.pixelSize: 12 } }
                                        }
                                        MouseArea { id: admMa; anchors.fill: parent; hoverEnabled: true
                                            onDoubleClicked: { logPop.rows = residents.changeLog(modelData.id); logPop.open() } }
                                    }
                                }
                            }
                        }
                    }

                    // 푸터
                    RowLayout {
                        Layout.fillWidth: true; spacing: 8
                        FlatButton {
                            text: "저장"; px: 13; weight: Font.Bold; padH: 28; padV: 7
                            bg: Theme.accent; fg: Theme.accentInk; bd: Theme.accent
                            hoverBg: Theme.accentHover; hoverFg: Theme.accentInk; hoverBd: Theme.accentHover
                            onClicked: {
                                const r = residents.save(page.collect())
                                dlg.notify(r.ok ? "저장" : "저장 실패", r.message)
                            }
                        }
                        Item { Layout.fillWidth: true }
                        FlatButton {
                            visible: !page.isNew
                            readonly property bool active: page.status === "재원"
                            text: active ? "퇴원 처리" : "재입원"
                            px: 13; weight: Font.Bold; padH: 16; padV: 7
                            fg: Theme.critical; bd: Theme.critical
                            hoverBg: Theme.critical; hoverFg: "#FFFFFF"; hoverBd: Theme.critical
                            onClicked: {
                                if (active)
                                    dlg.ask("퇴원 처리", "선택한 입소자를 퇴원 처리하시겠습니까?\n(기록은 보존됩니다)",
                                            function() { const r = residents.discharge(); dlg.notify("퇴원 처리", r.message) })
                                else
                                    dlg.ask("재입원 처리", "새 입원 기록을 만들까요?\n(이전 입원 이력은 그대로 보존됩니다)",
                                            function() { const r = residents.readmit(); dlg.notify("재입원 처리", r.message) })
                            }
                        }
                    }
                }
            }
        }
    }
}
