import QtQuick
import QtQuick.Controls

// QComboBox 대용 (#filterBar QComboBox 모양)
ComboBox {
    id: c
    property color fieldBg: Theme.card
    implicitHeight: 32
    font.pixelSize: 13
    background: Rectangle {
        radius: 3; color: c.fieldBg
        border.width: 1; border.color: c.activeFocus || c.popup.visible ? Theme.accent : Theme.border
    }
    contentItem: Text {
        leftPadding: 10; rightPadding: 24
        text: c.displayText; color: Theme.text; font: c.font
        verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
    }
    indicator: Canvas {
        x: c.width - width - 8; y: (c.height - height) / 2; width: 9; height: 5
        onPaint: { const g = getContext("2d"); g.clearRect(0, 0, width, height)
                   g.fillStyle = Theme.sub; g.beginPath(); g.moveTo(0, 0); g.lineTo(width, 0)
                   g.lineTo(width / 2, height); g.closePath(); g.fill() }
    }
    delegate: ItemDelegate {
        required property var modelData
        required property int index
        width: c.width; height: 30
        contentItem: Text { text: modelData; color: highlighted ? Theme.accentInk : Theme.text
                            font.pixelSize: 13; verticalAlignment: Text.AlignVCenter }
        highlighted: c.highlightedIndex === index
        background: Rectangle { color: highlighted ? Theme.accent : "transparent" }
    }
    popup: Popup {
        y: c.height + 2; width: c.width; padding: 1
        implicitHeight: Math.min(contentItem.implicitHeight + 2, 260)
        contentItem: ListView { clip: true; implicitHeight: contentHeight; model: c.popup.visible ? c.delegateModel : null }
        background: Rectangle { color: Theme.bgDeep; border.color: Theme.border; radius: 3 }
    }
}
