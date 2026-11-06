import QtQuick
import QtQuick.Controls

// 확인/알림 상자 (QMessageBox 대용). ask()로 예/아니오, notify()로 확인만.
Popup {
    id: d
    property string title: ""
    property string message: ""
    property bool askMode: true
    property var onYes: null
    function ask(t, m, cb) { title = t; message = m; askMode = true; onYes = cb; open() }
    function notify(t, m) { title = t; message = m; askMode = false; onYes = null; open() }

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    padding: 0
    closePolicy: Popup.CloseOnEscape
    width: 380
    background: Rectangle { color: Theme.panel; border.color: Theme.border; radius: 6 }
    Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.5) }

    contentItem: Column {
        spacing: 0
        Item {
            width: parent.width; height: 46
            Text { x: 18; anchors.verticalCenter: parent.verticalCenter; text: d.title
                   color: Theme.text; font.pixelSize: 15; font.weight: Font.ExtraBold }
        }
        Text {
            x: 18; width: parent.width - 36
            text: d.message; color: Theme.text; font.pixelSize: 13
            wrapMode: Text.Wrap; lineHeight: 1.3
        }
        Item { width: 1; height: 18 }
        Row {
            anchors.right: parent.right; anchors.rightMargin: 18
            spacing: 8
            FlatButton {
                visible: d.askMode; text: "아니오"; px: 13; padH: 18; padV: 6
                bg: Theme.bgDeep; onClicked: d.close()
            }
            FlatButton {
                text: d.askMode ? "예" : "확인"; px: 13; padH: 18; padV: 6; weight: Font.Bold
                bg: Theme.accent; fg: Theme.accentInk; bd: Theme.accent
                hoverBg: Theme.accentHover; hoverFg: Theme.accentInk; hoverBd: Theme.accentHover
                onClicked: { const cb = d.onYes; d.close(); if (cb) cb() }
            }
        }
        Item { width: 1; height: 16 }
    }
}
