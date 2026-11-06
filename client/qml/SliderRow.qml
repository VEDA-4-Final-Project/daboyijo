import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 라벨 + 클릭으로 바로 이동하는 슬라이더 (ClickSlider)
RowLayout {
    id: r
    property string label
    property alias value: s.value
    property alias from: s.from
    property alias to: s.to
    property bool active: true
    signal moved()
    spacing: 12
    Text { Layout.preferredWidth: 78; text: r.label; color: Theme.text; font.pixelSize: 13; font.weight: Font.DemiBold }
    Slider {
        id: s
        Layout.fillWidth: true; from: 0; to: 100; stepSize: 1; enabled: r.active
        onMoved: r.moved()
        background: Rectangle {
            x: s.leftPadding; y: s.topPadding + s.availableHeight / 2 - 2
            width: s.availableWidth; height: 4; radius: 2; color: Theme.border
            Rectangle { width: s.visualPosition * parent.width; height: parent.height; radius: 2
                        color: s.enabled ? Theme.accent : Theme.sub }
        }
        handle: Rectangle {
            x: s.leftPadding + s.visualPosition * (s.availableWidth - width); y: s.topPadding + s.availableHeight / 2 - height / 2
            width: 16; height: 16; radius: 8
            color: s.enabled ? Theme.text : Theme.sub; border.color: Theme.accent
        }
    }
    Text { Layout.preferredWidth: 28; horizontalAlignment: Text.AlignRight; text: Math.round(s.value)
           color: Theme.sub; font.pixelSize: 12; font.family: Theme.mono }
}
