import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Daboyijo 1.0

// 일일 리포트 — 위젯 buildReportPage() 와 같은 배치(달력 | 입소자 탭 + 지표 4칸 + 그래프 + 요약).
Rectangle {
    id: page
    color: Theme.panel; border.color: Theme.border; radius: 4

    property int month: new Date().getMonth()
    property int year: new Date().getFullYear()
    readonly property date sel: new Date(report.date + "T00:00:00")

    function syncMonth() { year = sel.getFullYear(); month = sel.getMonth() }
    Connections { target: report; function onDateChanged() { page.syncMonth() }
                  function onNotice(t, m) { dlg.notify(t, m) } }
    Component.onCompleted: { report.reloadResidents(); syncMonth() }
    function iso(d) { return d.getFullYear() + "-" + String(d.getMonth() + 1).padStart(2, "0") + "-" + String(d.getDate()).padStart(2, "0") }

    ConfirmDialog { id: dlg }
    FileDialog {
        id: pdfDlg
        fileMode: FileDialog.SaveFile
        nameFilters: ["PDF 파일 (*.pdf)"]
        onAccepted: { const r = report.exportPdf(selectedFile.toString()); if (r.message) dlg.notify("PDF 내보내기", r.message) }
    }

    ColumnLayout {
        anchors.fill: parent; anchors.margins: 18; anchors.topMargin: 16; anchors.bottomMargin: 18
        spacing: 6
        PanelTitle { text: "일일 리포트" }
        Text { text: "날짜를 선택하면 그 날의 입소자별 기록을 조회합니다"; color: Theme.sub; font.pixelSize: 13 }
        Item { Layout.preferredHeight: 4 }

        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 16

            // 좌: 달력
            Rectangle {
                Layout.preferredWidth: 320; Layout.fillHeight: true
                color: Theme.card; border.color: Theme.border; radius: 4
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 14; spacing: 10
                    Text { text: "날짜 선택"; color: Theme.sub; font.pixelSize: 12; font.weight: Font.DemiBold }
                    RowLayout {
                        Layout.fillWidth: true
                        FlatButton { text: "◀"; px: 12; padH: 8; bd: "transparent"
                                     onClicked: { if (page.month === 0) { page.month = 11; page.year-- } else page.month-- } }
                        Text { Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter
                               text: page.year + "년 " + (page.month + 1) + "월"
                               color: Theme.text; font.pixelSize: 14; font.weight: Font.Bold }
                        FlatButton { text: "▶"; px: 12; padH: 8; bd: "transparent"
                                     onClicked: { if (page.month === 11) { page.month = 0; page.year++ } else page.month++ } }
                    }
                    DayOfWeekRow {
                        Layout.fillWidth: true; locale: Qt.locale("ko_KR")
                        delegate: Text { required property string shortName; required property int index
                            text: shortName; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 12; font.weight: Font.Bold
                            color: (index === 0 || index === 6) ? Theme.critical : Theme.sub }
                    }
                    MonthGrid {
                        id: grid
                        Layout.fillWidth: true; Layout.preferredHeight: 230
                        month: page.month; year: page.year; locale: Qt.locale("ko_KR")
                        delegate: Rectangle {
                            id: cell
                            required property var model
                            readonly property bool cur: model.month === grid.month
                            readonly property bool future: model.date > new Date()
                            readonly property bool isSel: model.day === page.sel.getDate() && model.month === page.sel.getMonth()
                                                       && model.year === page.sel.getFullYear()
                            radius: 3
                            color: isSel ? Theme.select : (cma.containsMouse && !future ? Theme.panel : "transparent")
                            Text { anchors.centerIn: parent; text: cell.model.day; font.pixelSize: 13
                                   color: cell.isSel ? Theme.selectInk : (!cell.cur || cell.future) ? Theme.border
                                        : (cell.model.date.getDay() === 0 || cell.model.date.getDay() === 6) ? Theme.critical : Theme.text }
                            MouseArea { id: cma; anchors.fill: parent; hoverEnabled: true
                                        enabled: !cell.future; cursorShape: Qt.PointingHandCursor
                                        onClicked: report.date = page.iso(cell.model.date) }
                        }
                    }
                    Repeater {
                        model: [["오늘", 0], ["PDF 내보내기", 1], [report.hasSummary ? "요약 다시 생성" : "요약", 2]]
                        FlatButton {
                            required property var modelData
                            Layout.fillWidth: true
                            text: modelData[0]; px: 12; weight: Font.Bold; padV: 7
                            fg: Theme.text; bd: Theme.accent; hoverBg: Theme.panel; hoverBd: Theme.accent
                            opacity: modelData[1] === 2 && report.summaryBusy ? 0.5 : 1
                            onClicked: {
                                if (modelData[1] === 0) report.today()
                                else if (modelData[1] === 1) { pdfDlg.currentFile = "file:///" + report.suggestedPdfName(); pdfDlg.open() }
                                else if (!report.summaryBusy) report.requestSummary()
                            }
                        }
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // 우: 상세
            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true; spacing: 10
                Text { text: report.dateLabel; color: Theme.text; font.pixelSize: 16; font.weight: Font.ExtraBold }
                Rectangle {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    color: Theme.card; border.color: Theme.border; radius: 4
                    ColumnLayout {
                        anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 16
                        anchors.topMargin: 14; anchors.bottomMargin: 16; spacing: 12

                        Flow {
                            Layout.fillWidth: true; spacing: 6
                            Repeater {
                                model: report.residents
                                FlatButton {
                                    required property var modelData
                                    text: modelData.name; px: 13; weight: Font.Bold; padH: 14; padV: 5
                                    checked: report.residentId === modelData.id
                                    onClicked: report.residentId = modelData.id
                                }
                            }
                        }
                        Text { text: report.meta; color: Theme.sub; font.pixelSize: 12 }

                        RowLayout {
                            Layout.fillWidth: true; spacing: 12
                            Repeater {
                                model: [["누워있는 시간", "lying"], ["활동량", "activity"], ["케어시간", "care"], ["이벤트", "event"]]
                                Rectangle {
                                    required property var modelData
                                    Layout.fillWidth: true; Layout.preferredWidth: 1
                                    implicitHeight: 12 + 12 + 2 * 2 + c1.implicitHeight + v1.implicitHeight + s1.implicitHeight
                                    color: Theme.panel; border.color: Theme.border; radius: 4
                                    Column {
                                        x: 14; y: 12; spacing: 2; width: parent.width - 28
                                        Text { id: c1; text: modelData[0]; color: Theme.sub; font.pixelSize: 12; font.weight: Font.DemiBold }
                                        Text { id: v1; text: report.tiles[modelData[1]].val; color: Theme.text; font.family: Theme.mono
                                               font.pixelSize: 26; font.weight: Font.ExtraBold; elide: Text.ElideRight; width: parent.width }
                                        Text { id: s1; text: report.tiles[modelData[1]].sub; color: Theme.sub; font.pixelSize: 11
                                               font.weight: Font.DemiBold; elide: Text.ElideRight; width: parent.width }
                                    }
                                }
                            }
                        }

                        Text { text: "시간별 활동량"; color: Theme.sub; font.pixelSize: 12; font.weight: Font.DemiBold }
                        ActivityChartItem { Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 190; steps: report.hours }

                        Text { text: "요약"; color: Theme.sub; font.pixelSize: 12; font.weight: Font.DemiBold }
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: sumT.implicitHeight + 20
                            color: Theme.card; border.color: Theme.border; radius: 10
                            Text {
                                id: sumT; x: 12; y: 10; width: parent.width - 24
                                text: report.summary !== "" ? report.summary : "요약이 아직 없습니다. [요약] 버튼을 눌러 생성하세요."
                                wrapMode: Text.Wrap; font.pixelSize: 13
                                font.italic: report.summary === ""
                                color: report.summary === "" ? Theme.sub : Theme.text
                            }
                        }
                    }
                }
            }
        }
    }
}
