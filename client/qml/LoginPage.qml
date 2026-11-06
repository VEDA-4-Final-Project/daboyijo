import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Daboyijo 1.0

// 로그인 / 회원가입 — 위젯 LoginDialog · SignupDialog 와 같은 배치
Item {
    id: root
    property bool signupMode: false
    property string error: ""

    BackdropItem { anchors.fill: parent }

    // 전체화면 로그인에는 타이틀바가 없다 — 이 버튼이 앱을 끝내는 유일한 경로
    Rectangle {
        x: parent.width - 44 - 24; y: 24
        width: 44; height: 44; radius: 22
        color: closeMa.containsMouse ? Qt.rgba(1, 1, 1, 0.14) : "transparent"
        Text { anchors.centerIn: parent; text: "✕"; color: closeMa.containsMouse ? "#E6EDF3" : "#8B98A5"; font.pixelSize: 17; font.bold: true }
        MouseArea { id: closeMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: App.quit() }
    }
    Keys.onEscapePressed: App.quit()

    // ── 로그인 카드 (#authGlassCard) ───────────────────────
    Rectangle {
        id: card
        visible: !root.signupMode
        width: 480
        height: col.implicitHeight + 44 + 28
        x: (parent.width - width) / 2
        y: parent.height * 5 / 11 - height / 2 + 0     // 위 5 : 아래 6 비율
        radius: 18
        color: Qt.rgba(21/255, 29/255, 38/255, 0.72)
        border.color: Qt.rgba(1, 1, 1, 0.10)

        Column {
            id: col
            x: 46; y: 44
            width: parent.width - 92
            spacing: 0

            Text { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: "Carenet"
                   color: "#17C7B6"; font.pixelSize: 42; font.weight: Font.ExtraBold; font.letterSpacing: 4 }
            Item { width: 1; height: 6 }
            Text { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: "요양원 통합 관제 시스템"
                   color: "#8B98A5"; font.pixelSize: 13; font.letterSpacing: 0.5 }
            Item { width: 1; height: 32 }

            AuthField {
                id: idF
                glass: true; showHint: false
                caption: "아이디"; placeholder: "아이디를 입력하세요"; maxLength: 50
                text: App.savedId
                onEdited: root.error = ""
                onAccepted: pwF.forceFocus()
            }
            Item { width: 1; height: 16 }
            AuthField {
                id: pwF
                glass: true; showHint: false
                caption: "비밀번호"; placeholder: "비밀번호를 입력하세요"; maxLength: 128
                echoMode: TextInput.Password
                onEdited: root.error = ""
                onAccepted: root.attempt()
            }
            Item { width: 1; height: 12 }
            CheckBox {
                id: remember
                checked: App.savedId.length > 0
                padding: 0
                indicator: Rectangle {
                    x: 0; y: (remember.height - height) / 2
                    width: 14; height: 14; radius: 3
                    color: remember.checked ? "#17C7B6" : Qt.rgba(9/255, 13/255, 18/255, 0.55)
                    border.color: remember.checked ? "#17C7B6" : Qt.rgba(1, 1, 1, 0.14)
                }
                contentItem: Text { leftPadding: 14 + 6; text: "아이디 저장"; color: "#8B98A5"; font.pixelSize: 12
                                    verticalAlignment: Text.AlignVCenter }
            }
            Item { width: 1; height: 8 }
            Text {
                width: parent.width; height: 34
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                wrapMode: Text.Wrap
                text: root.error
                color: "#FF5A5F"; font.pixelSize: 12; font.weight: Font.DemiBold
            }
            Rectangle {
                id: loginBtn
                property bool busy: false
                width: parent.width; height: 44; radius: 8
                color: busy ? "#2A3742" : loginMa.containsMouse ? "#3AD4C4" : "#17C7B6"
                Text { anchors.centerIn: parent; text: loginBtn.busy ? "확인 중…" : "로그인"
                       color: loginBtn.busy ? "#8B98A5" : "#0E141B"; font.pixelSize: 15; font.weight: Font.Bold; font.letterSpacing: 1 }
                MouseArea { id: loginMa; anchors.fill: parent; hoverEnabled: true; enabled: !loginBtn.busy
                            cursorShape: Qt.PointingHandCursor; onClicked: root.attempt() }
            }
            Item { width: 1; height: 10 }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 2
                Text { anchors.verticalCenter: parent.verticalCenter; text: "계정이 없으신가요?"; color: "#8B98A5"; font.pixelSize: 11 }
                Text {
                    padding: 6
                    text: "회원가입"
                    color: signMa.containsMouse ? "#3AD4C4" : "#17C7B6"
                    font.pixelSize: 13; font.weight: Font.Bold
                    font.underline: signMa.containsMouse
                    MouseArea { id: signMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: { root.signupMode = true; root.error = ""; su.reset() } }
                }
            }
            Item { width: 1; height: 14 }
            Text {
                width: parent.width; horizontalAlignment: Text.AlignHCenter
                text: App.dbConnected ? "v1.0 · 관제 클라이언트" : "⚠ 데이터베이스에 연결되어 있지 않습니다"
                color: "#8B98A5"; font.pixelSize: 11
            }
        }
    }

    // ── 회원가입 카드 (#authCard, 440×660) ──────────────────
    Rectangle {
        id: su
        visible: root.signupMode
        width: 440; height: 660
        x: (parent.width - width) / 2; y: (parent.height - height) / 2
        radius: 8
        color: Theme.panel
        border.color: Theme.border
        property bool idChecked: false
        property string err: ""
        property bool busy: false
        function reset() { idF2.text = ""; nameF.text = ""; pw1.text = ""; pw2.text = ""
                           idF2.hint = ""; idF2.invalid = false; idF2.okState = false
                           nameF.hint = ""; nameF.invalid = false
                           pw1.hint = ""; pw1.invalid = false; pw2.hint = ""; pw2.invalid = false; pw2.okState = false
                           idChecked = false; err = "" }
        readonly property bool canSubmit:
            App.validateLoginId(idF2.text) === "" && App.validateName(nameF.text) === ""
            && App.validatePassword(pw1.text) === "" && pw2.text.length > 0 && pw2.text === pw1.text

        Column {
            x: 38; y: 30
            width: parent.width - 76
            spacing: 0
            Text { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: "회원가입"
                   color: Theme.text; font.pixelSize: 20; font.weight: Font.ExtraBold }
            Item { width: 1; height: 6 }
            Text { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: "관제 시스템에서 사용할 계정을 만듭니다"
                   color: Theme.sub; font.pixelSize: 13; font.letterSpacing: 0.5 }
            Item { width: 1; height: 24 }

            AuthField {
                id: idF2
                caption: "아이디"; placeholder: "영문 소문자·숫자 4~20자"; maxLength: 20
                onEdited: {
                    su.idChecked = false; su.err = ""
                    const e = App.validateLoginId(text)
                    hint = e; invalid = e !== ""; okState = false
                }
                onFinished: {
                    const r = App.checkLoginId(text)
                    hint = r.message; invalid = r.error; okState = r.ok; su.idChecked = r.ok
                }
                onAccepted: nameF.forceFocus()
            }
            AuthField {
                id: nameF
                caption: "이름"; placeholder: "화면에 표시될 이름"; maxLength: 50
                onEdited: { su.err = ""; const e = App.validateName(text); hint = e; invalid = e !== "" }
                onAccepted: pw1.forceFocus()
            }
            AuthField {
                id: pw1
                caption: "비밀번호"; placeholder: "4자 이상"; maxLength: 128
                echoMode: TextInput.Password
                onEdited: {
                    su.err = ""
                    const e = App.validatePassword(text); hint = e; invalid = e !== ""
                    if (pw2.text.length > 0) su.confirm()
                }
                onAccepted: pw2.forceFocus()
            }
            AuthField {
                id: pw2
                caption: "비밀번호 확인"; placeholder: "비밀번호를 다시 입력하세요"; maxLength: 128
                echoMode: TextInput.Password
                onEdited: { su.err = ""; su.confirm() }
                onAccepted: su.submit()
            }
            Item { width: 1; height: 4 }
            Text { width: parent.width; height: 32; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                   wrapMode: Text.Wrap; text: su.err; color: Theme.critical; font.pixelSize: 12; font.weight: Font.DemiBold }
            Rectangle {
                width: parent.width; height: 44; radius: 8
                color: !su.canSubmit || su.busy ? Theme.border : subMa.containsMouse ? Theme.accentHover : Theme.accent
                Text { anchors.centerIn: parent; text: su.busy ? "가입 중…" : "가입하기"
                       color: su.canSubmit ? Theme.accentInk : Theme.sub; font.pixelSize: 15; font.weight: Font.Bold; font.letterSpacing: 1 }
                MouseArea { id: subMa; anchors.fill: parent; hoverEnabled: true; enabled: su.canSubmit && !su.busy
                            cursorShape: Qt.PointingHandCursor; onClicked: su.submit() }
            }
            Item { width: 1; height: 4 }
            Text {
                width: parent.width; horizontalAlignment: Text.AlignHCenter; padding: 6
                text: "취소"
                color: canMa.containsMouse ? Theme.accentHover : Theme.accent
                font.pixelSize: 13; font.weight: Font.Bold; font.underline: canMa.containsMouse
                MouseArea { id: canMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: root.signupMode = false }
            }
        }

        function confirm() {
            if (pw2.text.length === 0) { pw2.hint = ""; pw2.invalid = false; pw2.okState = false; return }
            const same = pw2.text === pw1.text
            pw2.hint = same ? "비밀번호가 일치합니다" : "비밀번호가 일치하지 않습니다"
            pw2.invalid = !same; pw2.okState = same
        }
        function submit() {
            if (!canSubmit) return
            if (!idChecked) {
                const r = App.checkLoginId(idF2.text)
                idF2.hint = r.message; idF2.invalid = r.error; idF2.okState = r.ok; idChecked = r.ok
                if (!idChecked) { idF2.forceFocus(); return }
            }
            err = ""; busy = true
            const r = App.signup(idF2.text, nameF.text, pw1.text)
            busy = false
            if (r.ok) {
                idF.text = r.loginId; pwF.text = ""
                root.signupMode = false
                root.error = ""
                pwF.forceFocus()
            } else if (r.field === "id") {
                idF2.hint = r.error; idF2.invalid = true; idF2.okState = false; idChecked = false; idF2.forceFocus()
            } else {
                err = r.error
            }
        }
    }

    function attempt() {
        loginBtn.busy = true
        const r = App.login(idF.text, pwF.text, remember.checked)
        loginBtn.busy = false
        if (!r.ok) {
            error = r.error
            if (r.field === "pw") { pwF.text = ""; pwF.forceFocus() }
            else if (r.field === "id") idF.forceFocus()
        }
    }
    Component.onCompleted: (App.savedId.length > 0 ? pwF : idF).forceFocus()
}
