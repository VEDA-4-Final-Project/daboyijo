import QtQuick
import QtQuick.Controls

// QLineEdit#formEdit
TextField {
    color: Theme.text
    font.pixelSize: 13
    placeholderTextColor: Theme.sub
    selectByMouse: true
    leftPadding: 10; rightPadding: 10; topPadding: 6; bottomPadding: 6
    implicitHeight: 32
    background: Rectangle {
        radius: 3
        color: Theme.panel
        border.width: 1
        border.color: parent.activeFocus ? Theme.accent : Theme.border
    }
}
