import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import Daboyijo 1.0
import "HelpData.js" as Help

// 도움말 창 — 위젯 onHelpClicked/renderHelpTopic 와 같은 구성 (좌: 검색+주제 목록 / 우: 카드 본문)
Window {
    id: dlg
    width: 940; height: 700; minimumWidth: 700; minimumHeight: 480
    title: "Carenet 도움말"
    color: Theme.panel
    property int shown: 0
    property string needle: ""

    function matches(t) {
        if (needle === "") return true
        let hay = t.title + " " + t.summary
        for (const g of t.groups) { hay += " " + g.heading; for (const e of g.entries) hay += " " + e.term + " " + e.desc }
        return hay.toLowerCase().indexOf(needle.toLowerCase()) >= 0
    }
    // 주제마다 머리글을 보일지(같은 section 첫 항목) 계산
    readonly property var rows: {
        const out = []; let last = ""
        for (let i = 0; i < Help.topics.length; ++i) {
            const t = Help.topics[i]
            if (!matches(t)) continue
            if (t.section !== last) { out.push({ head: true, text: t.section }); last = t.section }
            out.push({ head: false, text: t.icon + "   " + t.title, idx: i })
        }
        return out
    }
    readonly property var tp: Help.topics[shown]

    RowLayout {
        anchors.fill: parent; spacing: 0
        Rectangle {
            Layout.preferredWidth: 236; Layout.fillHeight: true; color: Theme.bgDeep
            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 14; anchors.topMargin: 16; anchors.bottomMargin: 12; spacing: 9
                Text { text: "Carenet 도움말"; color: Theme.text; font.pixelSize: 16; font.weight: Font.ExtraBold }
                FormEdit { Layout.fillWidth: true; placeholderText: "🔍  기능 검색"; onTextChanged: dlg.needle = text.trim() }
                ListView {
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 2
                    model: dlg.rows
                    ScrollBar.vertical: ScrollBar { }
                    delegate: Rectangle {
                        required property var modelData
                        width: ListView.view.width; height: modelData.head ? 30 : 34; radius: 4
                        color: !modelData.head && dlg.shown === modelData.idx ? Theme.select : (ma.containsMouse && !modelData.head ? Theme.card : "transparent")
                        Text {
                            x: 10; anchors.verticalCenter: parent.verticalCenter; text: modelData.text
                            color: modelData.head ? Theme.accent : (dlg.shown === modelData.idx ? Theme.selectInk : Theme.text)
                            font.pixelSize: modelData.head ? 11 : 13
                            font.weight: modelData.head || dlg.shown === modelData.idx ? Font.ExtraBold : Font.DemiBold
                        }
                        MouseArea { id: ma; anchors.fill: parent; hoverEnabled: !modelData.head; enabled: !modelData.head
                                    cursorShape: Qt.PointingHandCursor; onClicked: dlg.shown = modelData.idx }
                    }
                }
            }
        }
        Flickable {
            id: fl
            Layout.fillWidth: true; Layout.fillHeight: true; clip: true
            contentWidth: width; contentHeight: body.implicitHeight + 48
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }
            onContentHeightChanged: if (contentY > contentHeight) contentY = 0
            Connections { target: dlg; function onShownChanged() { fl.contentY = 0 } }
            ColumnLayout {
                id: body
                x: 28; y: 24; width: fl.width - 56 - 8; spacing: 7
                RowLayout {
                    Layout.fillWidth: true; spacing: 12
                    Text { text: dlg.tp.icon; font.pixelSize: 28; Layout.alignment: Qt.AlignTop }
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 4
                        Text { text: dlg.tp.title; color: Theme.text; font.pixelSize: 21; font.weight: Font.ExtraBold }
                        Text { Layout.fillWidth: true; text: dlg.tp.summary; wrapMode: Text.Wrap; color: Theme.sub; font.pixelSize: 13; lineHeight: 1.45 }
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border; Layout.topMargin: 6 }
                Repeater {
                    model: dlg.tp.groups
                    ColumnLayout {
                        id: grp
                        required property var modelData
                        Layout.fillWidth: true; spacing: 7
                        Text { visible: grp.modelData.heading !== ""; Layout.topMargin: 8; text: grp.modelData.heading
                               color: Theme.accent; font.pixelSize: 12; font.weight: Font.ExtraBold; font.letterSpacing: 1 }
                        Repeater {
                            model: grp.modelData.entries
                            HelpCard { required property var modelData; Layout.fillWidth: true; bar: Theme.accent
                                       head: modelData.term; desc: modelData.desc; isNew: modelData.isNew }
                        }
                    }
                }
                HelpCard { visible: dlg.tp.tip !== ""; Layout.fillWidth: true; Layout.topMargin: 4; bar: Theme.warn
                           head: "💡 팁"; desc: dlg.tp.tip }
            }
        }
    }

    component HelpCard: Rectangle {
        property color bar: Theme.accent
        property string head
        property string desc
        property bool isNew: false
        implicitHeight: col.implicitHeight + 18
        color: Theme.card
        Rectangle { width: 3; height: parent.height; color: parent.bar }
        Column {
            id: col
            x: 17; y: 9; width: parent.width - 31; spacing: 3
            Row {
                spacing: 8
                Text { text: head; color: Theme.text; font.pixelSize: 13; font.weight: Font.Bold }
                Rectangle { visible: isNew; anchors.verticalCenter: parent.verticalCenter; width: nw.implicitWidth + 8; height: nw.implicitHeight + 2; color: Theme.accent
                            Text { id: nw; anchors.centerIn: parent; text: "NEW"; color: Theme.accentInk; font.pixelSize: 10; font.weight: Font.ExtraBold } }
            }
            Text { width: parent.width; text: desc; textFormat: Text.RichText; wrapMode: Text.Wrap; color: Theme.sub; font.pixelSize: 13; lineHeight: 1.45 }
        }
    }
}
