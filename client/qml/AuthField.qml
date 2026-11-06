import QtQuick
import QtQuick.Controls

// 로그인·회원가입 입력칸 (#authEdit) + 밑줄 힌트(#authHint)
Column {
    id: f
    property alias text: edit.text
    property alias placeholder: edit.placeholderText
    property alias echoMode: edit.echoMode
    property alias maxLength: edit.maximumLength
    property string caption: ""
    property string hint: ""
    property bool invalid: false
    property bool okState: false
    property bool glass: false
    property bool showHint: true
    signal edited()
    signal accepted()
    signal finished()
    function forceFocus() { edit.forceActiveFocus() }
    spacing: 0
    width: parent ? parent.width : 300

    Text { text: f.caption; color: Theme.sub; font.pixelSize: 12; font.weight: Font.Bold }
    Item { width: 1; height: 6 }
    TextField {
        id: edit
        width: parent.width
        height: 44
        color: Theme.text
        font.pixelSize: 14
        placeholderTextColor: Theme.sub
        leftPadding: 12; rightPadding: 12; topPadding: 10; bottomPadding: 10
        selectByMouse: true
        background: Rectangle {
            radius: 8
            color: f.glass ? (edit.activeFocus ? Qt.rgba(9/255, 13/255, 18/255, 0.75) : Qt.rgba(9/255, 13/255, 18/255, 0.55))
                           : Theme.bgDeep
            border.width: 1
            border.color: f.invalid ? Theme.critical
                        : edit.activeFocus ? Theme.accent
                        : f.glass ? Qt.rgba(1, 1, 1, 0.14) : Theme.border
        }
        onTextEdited: f.edited()
        onAccepted: f.accepted()
        onEditingFinished: f.finished()
    }
    Item { visible: f.showHint; width: 1; height: 3 }
    Text {
        visible: f.showHint
        height: 16
        text: f.hint
        color: f.invalid ? Theme.critical : f.okState ? Theme.normal : Theme.sub
        font.pixelSize: 11
        font.weight: (f.invalid || f.okState) ? Font.DemiBold : Font.Normal
    }
}
