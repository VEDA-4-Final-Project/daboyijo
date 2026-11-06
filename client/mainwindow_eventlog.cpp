// MainWindow — 이벤트 기록 탭 — 로그 표, 블랙박스·NVR 재생
// (mainwindow.cpp를 기능별로 나눈 파일 중 하나. 공용 헬퍼는 mainwindow_p.h)
#include "mainwindow_p.h"

// ═══════════════════════════════════════════════════════════
//  TAB2: 이벤트 기록 — 상단 요약 카드 + 필터 + [로그 표 | 인라인 블랙박스+NVR].
//  왼쪽 표의 행을 열면 페이지 이동 없이 바로 이 페이지 오른쪽에서 재생된다
//  (onLogRowActivated). 영상 검색 페이지의 AI 검색 결과 클립도 같은 재생기로
//  오게 하려면 이 페이지로 이동시키는 쪽을 택했다(재생기를 두 곳에 중복해서
//  두지 않기 위해) — buildVideoSearchTab() 참고.
// ═══════════════════════════════════════════════════════════
QWidget* MainWindow::buildEventLogTab()
{
    auto* panel = new QFrame();
    panel->setObjectName("panel");

    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(18, 16, 18, 16);
    outer->setSpacing(14);

    auto* title = new QLabel(QStringLiteral("이벤트 기록"));
    title->setObjectName("panelTitle");
    outer->addWidget(title);

    // 본문 3단: 좌측 필터 컬럼 / 가운데 로그 표 / 우측 재생기 + NVR 탐색.
    // Wisenet Viewer의 Event search 창과 같은 순서다 — 조건→결과→미리보기.
    auto* body = new QHBoxLayout();
    body->setSpacing(12);
    body->addWidget(buildSearchFilters(), 0);
    body->addWidget(buildLogTable(), 5);

    auto* right = new QVBoxLayout();
    right->setSpacing(8);
    auto* bbCap = new QLabel(QStringLiteral("블랙박스"));
    bbCap->setObjectName("panelTitle");
    right->addWidget(bbCap);
    auto* bbHint = new QLabel(
        QStringLiteral("왼쪽 표에서 이벤트를 고르면 여기서 바로 재생됩니다."));
    bbHint->setObjectName("subtitle");
    bbHint->setWordWrap(true);
    right->addWidget(bbHint);

    // 지금 보고 있는 게 어느 사건인지 — 표에서 눈을 떼면 잊는다.
    eventContextLabel = new QLabel();
    eventContextLabel->setObjectName("eventContext");
    eventContextLabel->setWordWrap(true);
    eventContextLabel->hide();   // 행을 고르기 전엔 보여줄 게 없다
    right->addWidget(eventContextLabel);
    right->addWidget(buildBlackboxPlayer(), 1);

    auto* actionRow = new QHBoxLayout();
    nvrJumpButton = new QPushButton(QStringLiteral("이 시점 NVR에서 이어보기"));
    nvrJumpButton->setObjectName("nvrJumpButton");
    nvrJumpButton->setCursor(Qt::PointingHandCursor);
    nvrJumpButton->setEnabled(false);   // 행을 열기 전까진 대상이 없음
    connect(nvrJumpButton, &QPushButton::clicked, this, &MainWindow::jumpToNvrContext);
    actionRow->addWidget(nvrJumpButton, 1);

    clipDownloadButton = new QPushButton(QStringLiteral("다운로드"));
    clipDownloadButton->setObjectName("clipDownloadButton");
    clipDownloadButton->setCursor(Qt::PointingHandCursor);
    clipDownloadButton->setEnabled(false);   // 재생된 클립이 있어야 저장할 게 있음
    connect(clipDownloadButton, &QPushButton::clicked, this, &MainWindow::downloadCurrentClip);
    actionRow->addWidget(clipDownloadButton);
    right->addLayout(actionRow);

    // NVR 세그먼트 목록 카드는 없앴다 — 같은 nvrSegments_를 관제화면 하단
    // 타임라인이 시간축 위에 그려 주고 클릭 탐색까지 된다. 파일 목록은 그 데이터를
    // "채널 1 · 11:00:00" 같은 파일명으로 늘어놓을 뿐이라, 원하는 시각을 찾으려면
    // 파일명에서 시간을 역산해야 했다. 이 페이지에 남는 NVR 접점은
    // [이 시점 NVR에서 이어보기] 하나 — 이벤트에서 그 순간으로 건너뛰는 링크다.
    body->addLayout(right, 4);

    outer->addLayout(body, 1);

    // 페이지를 만들자마자 원장을 한 번 읽는다 — 예전엔 이 표가 세션 메모리만
    // 담고 있어서 앱을 켤 때마다 비어 있었다.
    reloadEventLog();
    return panel;
}

// 일일 리포트 — [좌: 달력] | [우: 이름 탭 + 그 사람의 그 날 지표]
//
// 예전엔 "오늘" 만 볼 수 있는 케어 타임 대시보드였다. 리포트는 지난 날짜를
// 되짚어 보는 게 본질이라 날짜 선택을 앞에 세운다. 채널이 아니라 입소자 단위인
// 이유: 침대마다 사람을 매핑하면 한 채널에 여러 명이 들어와 4칸 고정이 안 맞는다.
QWidget* MainWindow::buildReportPage()
{
    auto* panel = new QFrame();
    panel->setObjectName("panel");
    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(18, 16, 18, 16);
    outer->setSpacing(6);

    auto* title = new QLabel(QStringLiteral("일일 리포트"));
    title->setObjectName("panelTitle");
    outer->addWidget(title);

    auto* sub = new QLabel(
        QStringLiteral("날짜를 선택하면 그 날의 입소자별 기록을 조회합니다"));
    sub->setObjectName("subtitle");
    outer->addWidget(sub);
    outer->addSpacing(10);

    // ── 좌우 2단: 달력(고정 폭) | 리포트 본문(남는 공간 전부) ──
    auto* body = new QHBoxLayout();
    body->setSpacing(16);
    body->addWidget(buildReportCalendar(), 0);

    auto* right = new QVBoxLayout();
    right->setSpacing(10);
    // 선택한 날짜를 우측에도 크게 — 달력에서 눈을 떼도 어느 날 자료인지 보이게.
    reportDateLabel = new QLabel();
    reportDateLabel->setObjectName("reportDate");
    right->addWidget(reportDateLabel);
    right->addWidget(buildReportDetail(), 1);

    body->addLayout(right, 1);
    outer->addLayout(body, 1);

    reloadReportResidents();            // 이름 탭 채우기 → 첫 입소자 자동 선택
    onReportDateChanged(reportDate_);   // 날짜 라벨 + 지표 초기 조회
    return panel;
}

// 좌측 날짜 선택 칼럼 — 달력 + (앞으로) PDF·AI 요약 버튼이 붙을 자리.
// QCalendarWidget의 요일 머리글(일~토)은 QSS의 QHeaderView::section 규칙이 닿지
// 않는다 — 헤더는 위젯 내부에서 QTextCharFormat으로 그려지기 때문이다. 그래서
// 다크 테마에서도 흰 바탕 한 줄이 남아 있었다. 배경/글자색을 코드로 직접 준다.
// (주말 색은 QCalendarWidget 기본값인 빨강을 유지하되 팔레트 톤에 맞춘다.)
void MainWindow::applyCalendarPalette(QCalendarWidget* cal)
{
    if (!cal) return;

    QTextCharFormat head;
    head.setBackground(QColor(QString::fromLatin1(kCard)));
    head.setForeground(QColor(QString::fromLatin1(kTextSub)));
    head.setFontWeight(QFont::Bold);
    cal->setHeaderTextFormat(head);

    // 평일/주말 본문 색도 같은 경로로 맞춘다 — QSS로는 요일별 색을 못 준다.
    QTextCharFormat weekday;
    weekday.setForeground(QColor(QString::fromLatin1(kTextMain)));
    for (int d = Qt::Monday; d <= Qt::Friday; ++d)
        cal->setWeekdayTextFormat(Qt::DayOfWeek(d), weekday);

    QTextCharFormat weekend;
    weekend.setForeground(QColor(QString::fromLatin1(kCritical)));
    cal->setWeekdayTextFormat(Qt::Saturday, weekend);
    cal->setWeekdayTextFormat(Qt::Sunday, weekend);
}

QWidget* MainWindow::buildReportCalendar()
{
    auto* col = new QFrame();
    col->setObjectName("careCard");
    col->setFixedWidth(320);
    auto* lay = new QVBoxLayout(col);
    lay->setContentsMargins(14, 14, 14, 14);
    lay->setSpacing(10);

    auto* cap = new QLabel(QStringLiteral("날짜 선택"));
    cap->setObjectName("careBigCap");
    lay->addWidget(cap);

    reportCalendar = new QCalendarWidget();
    reportCalendar->setObjectName("reportCalendar");
    reportCalendar->setGridVisible(false);
    reportCalendar->setVerticalHeaderFormat(QCalendarWidget::NoVerticalHeader);  // 주차 번호 숨김
    reportCalendar->setHorizontalHeaderFormat(QCalendarWidget::ShortDayNames);
    reportCalendar->setSelectedDate(reportDate_);
    // 미래 날짜는 볼 자료가 없다 — 아예 못 고르게 막는다.
    reportCalendar->setMaximumDate(QDate::currentDate());
    applyCalendarPalette(reportCalendar);
    lay->addWidget(reportCalendar);

    // 오늘로 되돌아오는 버튼 — 과거를 뒤지다 보면 오늘 찾아 돌아오기가 번거롭다.
    auto* today = new QPushButton(QStringLiteral("오늘"));
    today->setObjectName("reportTodayBtn");
    today->setCursor(Qt::PointingHandCursor);
    connect(today, &QPushButton::clicked, this, [this] {
        reportCalendar->setSelectedDate(QDate::currentDate());
    });
    lay->addWidget(today);

    // TODO(리포트): AI 요약 버튼이 이 아래에 들어간다.
    auto* pdfBtn = new QPushButton(QStringLiteral("PDF 내보내기"));
    pdfBtn->setObjectName("reportTodayBtn");
    pdfBtn->setCursor(Qt::PointingHandCursor);
    connect(pdfBtn, &QPushButton::clicked, this, &MainWindow::exportReportPdf);
    lay->addWidget(pdfBtn);

    summaryBtn = new QPushButton(QStringLiteral("요약"));
    summaryBtn->setObjectName("reportTodayBtn");
    summaryBtn->setCursor(Qt::PointingHandCursor);
    connect(summaryBtn, &QPushButton::clicked, this, &MainWindow::requestAiSummary);
    lay->addWidget(summaryBtn);

    lay->addStretch(1);

    connect(reportCalendar, &QCalendarWidget::selectionChanged, this, [this] {
        onReportDateChanged(reportCalendar->selectedDate());
    });
    return col;
}

// 우측 상세 — [이름 탭] + [지표 타일 4개] + (앞으로) 그래프·이벤트 표·AI 요약.
QWidget* MainWindow::buildReportDetail()
{
    auto* host = new QFrame();
    host->setObjectName("careCard");
    auto* lay = new QVBoxLayout(host);
    lay->setContentsMargins(16, 14, 16, 16);
    lay->setSpacing(12);

    // ── 입소자 이름 탭 ──
    // 버튼은 reloadReportResidents()가 DB를 보고 만든다. 여기선 담을 줄만 잡는다.
    auto* tabRow = new QWidget();
    residentTabLayout = new QHBoxLayout(tabRow);
    residentTabLayout->setContentsMargins(0, 0, 0, 0);
    residentTabLayout->setSpacing(6);
    residentTabLayout->addStretch(1);   // 버튼은 이 스트레치 앞에 끼워 넣는다
    lay->addWidget(tabRow);

    reportResidentMeta = new QLabel(QStringLiteral("—"));
    reportResidentMeta->setObjectName("careMeta");
    lay->addWidget(reportResidentMeta);

    // ── 지표 타일 4개 ──
    // 큰 숫자 + 아래 보조 문구. 라벨만 멤버로 잡아두고 갱신 때 텍스트만 바꾼다.
    auto makeTile = [](const QString& cap, QLabel** val, QLabel** sub) {
        auto* tile = new QFrame();
        tile->setObjectName("reportTile");
        auto* v = new QVBoxLayout(tile);
        v->setContentsMargins(14, 12, 14, 12);
        v->setSpacing(2);
        auto* c = new QLabel(cap);
        c->setObjectName("careBigCap");
        *val = new QLabel(QStringLiteral("—"));
        (*val)->setObjectName("reportTileVal");
        *sub = new QLabel(QStringLiteral(" "));
        (*sub)->setObjectName("careMiniCap");
        v->addWidget(c);
        v->addWidget(*val);
        v->addWidget(*sub);
        return tile;
    };

    auto* tiles = new QGridLayout();
    tiles->setSpacing(12);
    tiles->addWidget(makeTile(QStringLiteral("누워있는 시간"),
                              &tileLyingVal, &tileLyingSub), 0, 0);
    tiles->addWidget(makeTile(QStringLiteral("활동량"),
                              &tileActivityVal, &tileActivitySub), 0, 1);
    tiles->addWidget(makeTile(QStringLiteral("케어시간"),
                              &tileCareVal, &tileCareSub), 0, 2);
    tiles->addWidget(makeTile(QStringLiteral("이벤트"),
                              &tileEventVal, &tileEventSub), 0, 3);
    for (int c = 0; c < 4; ++c) tiles->setColumnStretch(c, 1);
    lay->addLayout(tiles);

    // ── 24시간 활동량 그래프 ──
    auto* chartCap = new QLabel(QStringLiteral("시간별 활동량"));
    chartCap->setObjectName("careBigCap");
    lay->addWidget(chartCap);

    activityChart = new ActivityChart();
    lay->addWidget(activityChart, 1);

    // ── AI 요약 ──
    // 위쪽 숫자는 기계가 센 값이고 이 문단은 생성된 글이다. 감사 상황에서 그
    // 구분이 중요하므로 배경을 달리해 시각적으로 떼어 놓는다.
    auto* sumCap = new QLabel(QStringLiteral("요약"));
    sumCap->setObjectName("careBigCap");
    lay->addWidget(sumCap);

    summaryLabel = new QLabel();
    summaryLabel->setObjectName("aiSummary");
    summaryLabel->setWordWrap(true);
    summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(summaryLabel);

    // TODO(리포트): 이 아래에 이벤트 타임라인 · AI 요약이 온다.
    return host;
}

// 재원 입소자로 이름 탭을 다시 만든다. 입소자 관리에서 사람이 바뀌면 다시 부른다.
//
// ※ 지금은 "현재 재원"만 본다. 과거 날짜를 보면서 그 시점 재원자로 목록을 맞추려면
//   admissions(입원 에피소드)를 조회해야 하는데, 그건 리포트가 자리 잡은 뒤에.
void MainWindow::reloadReportResidents()
{
    if (!residentTabLayout) return;

    for (auto* b : residentTabBtns) { residentTabLayout->removeWidget(b); b->deleteLater(); }
    residentTabBtns.clear();
    residentTabIds.clear();

    QSqlQuery q;
    if (!q.exec(QStringLiteral(
            "SELECT resident_id, name FROM residents WHERE status='재원' "
            "ORDER BY camera_id, bed, resident_id"))) {
        qDebug() << "리포트 입소자 목록 조회 실패:" << q.lastError().text();
        return;
    }
    while (q.next()) {
        const int id = q.value(0).toInt();
        auto* b = new QPushButton(q.value(1).toString());
        b->setObjectName("residentTab");
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::clicked, this, [this, id] { onReportResidentChanged(id); });
        // 마지막 stretch 앞에 끼워 넣어 버튼들이 왼쪽으로 몰리게 한다.
        residentTabLayout->insertWidget(residentTabLayout->count() - 1, b);
        residentTabBtns.push_back(b);
        residentTabIds.push_back(id);
    }

    // 보던 사람이 목록에서 사라졌으면 첫 사람으로 되돌린다.
    if (!residentTabIds.contains(reportResidentId_))
        reportResidentId_ = residentTabIds.isEmpty() ? -1 : residentTabIds.first();
    onReportResidentChanged(reportResidentId_);
}

void MainWindow::onReportResidentChanged(int residentId)
{
    reportResidentId_ = residentId;

    // 선택된 탭만 눌린 상태로 — QButtonGroup 없이 직접 맞춘다(탭이 매번 재생성됨).
    for (int i = 0; i < residentTabBtns.size(); ++i)
        residentTabBtns[i]->setChecked(residentTabIds.value(i) == residentId);

    if (reportResidentMeta) {
        QString meta = QStringLiteral("—");
        if (residentId > 0) {
            QSqlQuery q;
            q.prepare(QStringLiteral(
                "SELECT room, bed, COALESCE(risk_level,'—') FROM residents WHERE resident_id=?"));
            q.addBindValue(residentId);
            if (q.exec() && q.next()) {
                // 호실·침대는 아직 안 채운 입소자가 많다. 빈 값을 그대로 이으면
                // "호 ·  · 위험도 하" 처럼 구분점만 남아 오히려 지저분하다.
                QStringList parts;
                const QString room = q.value(0).toString().trimmed();
                const QString bed  = q.value(1).toString().trimmed();
                if (!room.isEmpty()) parts << QStringLiteral("%1호").arg(room);
                if (!bed.isEmpty())  parts << bed;
                parts << QStringLiteral("위험도 %1").arg(q.value(2).toString());
                meta = parts.join(QStringLiteral(" · "));
            }
        }
        reportResidentMeta->setText(meta);
    }
    updateCareTime();
}

// 날짜가 바뀌면 라벨을 고치고 그 날짜로 집계를 다시 돌린다.
void MainWindow::onReportDateChanged(const QDate& date)
{
    if (!date.isValid()) return;
    reportDate_ = date;

    if (reportDateLabel) {
        const QString when = QStringLiteral("%1 (%2)")
                                 .arg(date.toString(QStringLiteral("yyyy-MM-dd")),
                                      koreanDow(date));
        reportDateLabel->setText(date == QDate::currentDate()
                                     ? QStringLiteral("%1 · 오늘").arg(when)
                                     : when);
    }
    updateCareTime();
}

QWidget* MainWindow::buildSearchFilters()
{
    // Wisenet Viewer의 Event search 좌측 패널과 같은 배치 — 조건을 세로로 쌓고
    // 결과 표는 오른쪽에 넓게 둔다. 조건이 바뀌면 화면의 행을 숨기는 게 아니라
    // events 원장을 다시 조회한다(reloadEventLog) — 그래야 지난 기록이 나온다.
    auto* bar = new QFrame();
    bar->setObjectName("filterBar");
    bar->setFixedWidth(206);

    auto* lay = new QVBoxLayout(bar);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(6);

    auto* cap = new QLabel(QStringLiteral("검색 조건"));
    cap->setObjectName("filterCap");
    lay->addWidget(cap);
    lay->addSpacing(4);

    // 조건을 바꾸면 즉시 다시 조회한다 — 별도 '검색' 버튼은 두지 않는다.
    filterDateFrom = new QDateEdit(QDate::currentDate().addDays(-7));
    filterDateFrom->setCalendarPopup(true);
    filterDateFrom->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    filterDateTo = new QDateEdit(QDate::currentDate());
    filterDateTo->setCalendarPopup(true);
    filterDateTo->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    connect(filterDateFrom, &QDateEdit::dateChanged, this,
            [this](const QDate&) { reloadEventLog(); });
    connect(filterDateTo, &QDateEdit::dateChanged, this,
            [this](const QDate&) { reloadEventLog(); });

    filterRoom = new QComboBox();
    // 카메라(4채널) 한 대 = 방 하나. 지금은 room이 하나뿐이라 목록도 하나지만,
    // 카메라가 늘어나면 currentRoomName() 자리가 room 목록으로 바뀌면서 여기도
    // 자연히 늘어나는 구조 — 로그 위치 문구("{room} · 채널 N")와 접두어를 맞춰야
    // applyLogFilters()의 startsWith 매칭이 먹는다.
    filterRoom->addItem(QStringLiteral("전체 병실"));
    filterRoom->addItem(currentRoomName());
    connect(filterRoom, &QComboBox::currentTextChanged,
            this, [this](const QString&) { applyLogFilters(true); });

    filterChannel = new QComboBox();
    filterChannel->addItem(QStringLiteral("전체 채널"), -1);
    for (int ch = 0; ch < 4; ++ch)
        filterChannel->addItem(QStringLiteral("채널 %1").arg(ch + 1), ch);
    connect(filterChannel, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { reloadEventLog(); });

    filterEventType = new QComboBox();
    filterEventType->addItems({QStringLiteral("전체 이벤트"), QStringLiteral("낙상"),
                               QStringLiteral("침상이탈"), QStringLiteral("생체신호 이상")});
    connect(filterEventType, &QComboBox::currentTextChanged,
            this, [this](const QString&) { reloadEventLog(); });

    filterConfirmed = new QComboBox();
    filterConfirmed->addItems({QStringLiteral("전체"), QStringLiteral("미확인만"),
                               QStringLiteral("확인만")});
    connect(filterConfirmed, &QComboBox::currentTextChanged,
            this, [this](const QString&) { reloadEventLog(); });

    // 기간 프리셋 — 관제사가 실제로 묻는 단위는 "오늘", "이번 주", "이번 달"이다.
    // 날짜 두 칸을 매번 돌리게 하면 그 질문을 하기까지 손이 너무 많이 간다.
    auto* presetRow = new QHBoxLayout();
    presetRow->setContentsMargins(0, 0, 0, 0);
    presetRow->setSpacing(4);
    const QPair<QString, int> presets[3] = {
        {QStringLiteral("오늘"), 0}, {QStringLiteral("7일"), 6}, {QStringLiteral("30일"), 29}};
    for (const auto& pr : presets) {
        auto* b2 = new QPushButton(pr.first);
        b2->setObjectName("filterResetBtn");
        b2->setCursor(Qt::PointingHandCursor);
        connect(b2, &QPushButton::clicked, this, [this, days = pr.second] {
            // 두 날짜를 연달아 바꾸면 dateChanged가 두 번 떠 조회가 두 번 돈다.
            // 신호를 잠깐 막고 마지막에 한 번만 조회한다.
            const QSignalBlocker b1(filterDateFrom), b2(filterDateTo);
            filterDateFrom->setDate(QDate::currentDate().addDays(-days));
            filterDateTo->setDate(QDate::currentDate());
            reloadEventLog();
        });
        presetRow->addWidget(b2);
    }
    lay->addLayout(presetRow);
    lay->addSpacing(6);

    // 라벨을 필드 위에 얹는다 — 좁은 컬럼에서 라벨과 값을 한 줄에 두면 값이 눌린다.
    auto addField = [&](const QString& label, QWidget* w) {
        auto* l = new QLabel(label);
        l->setObjectName("filterFieldCap");
        lay->addWidget(l);
        lay->addWidget(w);
        lay->addSpacing(6);
    };
    addField(QStringLiteral("시작일"), filterDateFrom);
    addField(QStringLiteral("종료일"), filterDateTo);
    addField(QStringLiteral("병실"), filterRoom);
    addField(QStringLiteral("채널"), filterChannel);
    addField(QStringLiteral("이벤트 종류"), filterEventType);
    addField(QStringLiteral("확인 여부"), filterConfirmed);

    auto* resetBtn = new QPushButton(QStringLiteral("조건 초기화"));
    resetBtn->setObjectName("filterResetBtn");
    resetBtn->setCursor(Qt::PointingHandCursor);
    connect(resetBtn, &QPushButton::clicked, this, [this] {
        const QSignalBlocker b1(filterDateFrom), b2(filterDateTo), b3(filterRoom),
                             b4(filterChannel), b5(filterEventType), b6(filterConfirmed);
        filterDateFrom->setDate(QDate::currentDate().addDays(-7));
        filterDateTo->setDate(QDate::currentDate());
        filterRoom->setCurrentIndex(0);
        filterChannel->setCurrentIndex(0);
        filterEventType->setCurrentIndex(0);
        filterConfirmed->setCurrentIndex(0);
        reloadEventLog();
    });
    lay->addWidget(resetBtn);

    lay->addStretch();
    return bar;
}

QWidget* MainWindow::buildLogTable()
{
    auto* wrap = new QWidget();
    auto* v = new QVBoxLayout(wrap);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(6);

    // 결과 건수 — Wisenet Event search 하단의 "N Results"에 해당한다.
    // 미확인 수를 같이 보여준다: 관제사가 실제로 처리해야 할 양이 그 숫자다.
    auto* head = new QHBoxLayout();
    head->setContentsMargins(2, 0, 2, 0);
    logCountLabel = new QLabel();
    logCountLabel->setObjectName("logCount");
    head->addWidget(logCountLabel);
    head->addStretch();

    auto* reloadBtn = new QPushButton(QStringLiteral("새로고침"));
    reloadBtn->setObjectName("filterResetBtn");
    reloadBtn->setCursor(Qt::PointingHandCursor);
    reloadBtn->setToolTip(QStringLiteral("서버 원장에서 다시 조회"));
    connect(reloadBtn, &QPushButton::clicked, this, &MainWindow::reloadEventLog);
    head->addWidget(reloadBtn);
    v->addLayout(head);

    logTable = new QTableWidget(0, LogColCount);
    logTable->setObjectName("logTable");
    logTable->setHorizontalHeaderLabels(
        {QStringLiteral("발생시각"), QStringLiteral("종류"),
         QStringLiteral("위치"), QStringLiteral("입소자"),
         QStringLiteral("출처"), QStringLiteral("상태")});
    auto* hh = logTable->horizontalHeader();
    // 발생시각은 "yyyy-MM-dd HH:mm:ss"가 잘리지 않게 내용 폭, 위치만 남는 폭을 먹는다.
    hh->setSectionResizeMode(LogWhen, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(LogType, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(LogPlace, QHeaderView::Stretch);
    hh->setSectionResizeMode(LogResident, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(LogSource, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(LogStatus, QHeaderView::ResizeToContents);
    hh->setHighlightSections(false);
    logTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    logTable->setSelectionMode(QAbstractItemView::SingleSelection);
    logTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    logTable->setShowGrid(false);                       // 격자선 대신 행 여백으로 구분
    logTable->setAlternatingRowColors(true);            // 얼룩 배경으로 행 가독성 ↑
    logTable->verticalHeader()->setVisible(false);      // 행 번호 숨김
    logTable->verticalHeader()->setDefaultSectionSize(34);
    logTable->setCursor(Qt::PointingHandCursor);
    // 한 번 클릭 = 미리보기(Wisenet도 목록에서 고르면 바로 오른쪽에 뜬다).
    // 더블클릭까지 기다리게 하면 "왜 안 나오지"를 매번 겪는다.
    connect(logTable, &QTableWidget::cellClicked, this, &MainWindow::onLogRowActivated);

    // 조회 결과가 0건일 때 표 한가운데 띄우는 안내. 표 위에 겹쳐 놓고
    // 행이 없을 때만 보이게 한다 — 빈 격자만 남으면 고장으로 읽힌다.
    logEmptyHint = new QLabel(logTable);
    logEmptyHint->setObjectName("logEmptyHint");
    logEmptyHint->setAlignment(Qt::AlignCenter);
    logEmptyHint->setWordWrap(true);
    logEmptyHint->hide();

    v->addWidget(logTable, 1);
    return wrap;
}

// events 원장을 현재 필터 조건으로 조회해 표를 통째로 다시 채운다.
//
// 예전엔 이 표가 "이번 세션에 소켓으로 들어온 이벤트"만 담고 있어서, 앱을 껐다
// 켜면 항상 비어 있었다(서버는 그동안 계속 DB에 쌓고 있었다). 필터도 화면에 있는
// 행을 숨기는 방식이라 조회 범위가 곧 세션 길이였다. 이제 조건은 SQL로 내려간다.
void MainWindow::reloadEventLog()
{
    if (!logTable) return;

    if (!QSqlDatabase::database().isOpen()) {
        logTable->setRowCount(0);
        refreshLogSummary();
        return;
    }

    // 500행 상한 — 관제 PC에서 표가 버벅이지 않는 선. 넘치면 기간을 좁히라고 알린다.
    // (상한을 넘겼는지 알려면 한 줄 더 받아 봐야 한다 → LIMIT 501)
    static constexpr int kMaxRows = 500;

    QString sql = QStringLiteral(
        "SELECT e.event_id, UNIX_TIMESTAMP(e.occurred_at)*1000, e.camera_id, "
        "       e.event_type, e.source, e.clip_url, e.confirmed_at, e.confirmed_by, "
        "       r.name, r.room "
        "FROM events e LEFT JOIN residents r ON r.resident_id = e.resident_id "
        "WHERE DATE(e.occurred_at) BETWEEN ? AND ? ");

    QVariantList binds;
    binds << (filterDateFrom ? filterDateFrom->date() : QDate::currentDate().addDays(-7));
    binds << (filterDateTo ? filterDateTo->date() : QDate::currentDate());

    const QString typeSel = filterEventType ? filterEventType->currentText() : QString();
    if (typeSel == QStringLiteral("낙상"))            { sql += "AND e.event_type='FALL' "; }
    else if (typeSel == QStringLiteral("침상이탈")) { sql += "AND e.event_type='EGRESS' "; }
    else if (typeSel == QStringLiteral("생체신호 이상")) { sql += "AND e.event_type='VITAL_ABNORMAL' "; }

    if (filterChannel && filterChannel->currentData().toInt() >= 0) {
        sql += "AND e.camera_id = ? ";
        binds << filterChannel->currentData().toInt();
    }
    const QString confSel = filterConfirmed ? filterConfirmed->currentText() : QString();
    if (confSel == QStringLiteral("미확인만"))      sql += "AND e.confirmed_at IS NULL ";
    else if (confSel == QStringLiteral("확인만"))        sql += "AND e.confirmed_at IS NOT NULL ";

    sql += QStringLiteral("ORDER BY e.occurred_at DESC LIMIT %1").arg(kMaxRows + 1);

    QSqlQuery q;
    q.prepare(sql);
    for (const QVariant& b : binds) q.addBindValue(b);
    if (!q.exec()) {
        qDebug() << "이벤트 원장 조회 실패:" << q.lastError().text();
        return;
    }

    logTable->setSortingEnabled(false);
    logTable->setRowCount(0);

    int shown = 0;
    bool truncated = false;
    while (q.next()) {
        if (shown >= kMaxRows) { truncated = true; break; }

        EventLogRow e;
        e.eventId     = q.value(0).toLongLong();
        e.occurredMs  = q.value(1).toLongLong();
        e.channel     = q.value(2).isNull() ? -1 : q.value(2).toInt();
        e.typeCode    = q.value(3).toString();
        e.source      = q.value(4).toString();
        e.confirmed   = !q.value(6).isNull();
        e.confirmedBy = q.value(7).toString();
        e.residentName = q.value(8).toString();

        // clip_url 은 파일명만 저장된다(서버는 자기 외부 IP를 모른다 —
        // server/src/main.cpp 의 insertEvent 호출부 주석 참조). 호스트는 채널로 정해진다.
        const QString file = q.value(5).toString();
        if (!file.isEmpty() && e.channel >= 0) {
            e.clipUrl = file.startsWith(QLatin1String("http"))
                            ? file
                            : QStringLiteral("http://%1:%2/%3")
                                  .arg(hostForChannel(e.channel))
                                  .arg(kClipHttpPort)
                                  .arg(file);
        }

        // 위치 — 발생 당시 사람의 호실이 있으면 그걸 쓰고, 없으면 현재 방 이름.
        const QString room = q.value(9).toString();
        e.place = QStringLiteral("%1 · 채널 %2")
                      .arg(room.isEmpty() ? currentRoomName() : room)
                      .arg(e.channel + 1);

        insertEventRow(e);
        ++shown;
    }

    logTable->setSortingEnabled(true);
    logTable->sortItems(LogWhen, Qt::DescendingOrder);
    // 색·정렬은 refreshEventLog()가 입힌다(요약 갱신도 그 안에서 이어진다).
    // 이걸 건너뛰고 refreshLogSummary()만 부르면 방금 넣은 행들이 전경색 없이
    // 남아 표가 통째로 비어 보인다.
    refreshEventLog();

    if (truncated && logCountLabel) {
        logCountLabel->setText(
            logCountLabel->text() +
            QStringLiteral("  ·  %1행까지만 표시 — 기간을 좀 더 좁혀 보세요")
                .arg(kMaxRows));
    }
}

QString MainWindow::eventTypeLabel(const QString& code)
{
    if (code == QLatin1String("FALL"))   return QStringLiteral("낙상");
    if (code == QLatin1String("EGRESS")) return QStringLiteral("침상이탈");
    if (code == QLatin1String("VITAL_ABNORMAL")) return QStringLiteral("생체신호 이상");
    return code;
}

QString MainWindow::eventSourceLabel(const QString& code)
{
    if (code == QLatin1String("CAMERA"))   return QStringLiteral("카메라");
    if (code == QLatin1String("WEARABLE")) return QStringLiteral("웨어러블");
    return code;
}

// 표에 한 줄 넣기 — DB 조회분과 실시간 도착분이 공유하는 유일한 삽입 경로.
void MainWindow::insertEventRow(const EventLogRow& e)
{
    if (!logTable) return;

    const int row = logTable->rowCount();
    logTable->insertRow(row);

    auto* when = new QTableWidgetItem(
        QDateTime::fromMSecsSinceEpoch(e.occurredMs).toString("yyyy-MM-dd HH:mm:ss"));
    when->setData(LogClipUrl, e.clipUrl);
    when->setData(LogChannel, e.channel);
    when->setData(LogTimestamp, e.occurredMs);
    when->setData(LogEventId, e.eventId);
    logTable->setItem(row, LogWhen, when);

    logTable->setItem(row, LogType, new QTableWidgetItem(eventTypeLabel(e.typeCode)));
    logTable->setItem(row, LogPlace, new QTableWidgetItem(e.place));
    // 사람을 특정 못 한 이벤트를 빈칸으로 두면 "표가 덜 찼나"로 읽힌다 — 모른다고 쓴다.
    logTable->setItem(row, LogResident,
                      new QTableWidgetItem(e.residentName.isEmpty()
                                               ? QStringLiteral("신원 미상")
                                               : e.residentName));
    logTable->setItem(row, LogSource, new QTableWidgetItem(eventSourceLabel(e.source)));

    auto* st = new QTableWidgetItem(e.confirmed ? QStringLiteral("확인")
                                                : QStringLiteral("미확인"));
    if (e.confirmed && !e.confirmedBy.isEmpty())
        st->setToolTip(QStringLiteral("%1 확인").arg(e.confirmedBy));
    logTable->setItem(row, LogStatus, st);
}

// 실시간으로 도착한 이벤트를 표 맨 위에 얹는다.
// 서버도 같은 이벤트를 events 에 쓰므로, 다음 조회(reloadEventLog) 때 DB 쪽으로
// 자연히 합쳐진다 — 여기서 넣는 행은 event_id 가 없어 '확인'이 DB에 남지 않는다.
void MainWindow::appendLiveEvent(int channel, int roiId, qint64 occurredMs,
                                 const QString& typeCode, const QString& source)
{
    if (!logTable) return;

    EventLogRow e;
    e.occurredMs = occurredMs;
    e.channel = channel;
    e.typeCode = typeCode;
    e.source = source;
    e.place = eventPlaceLabel(channel, roiId);
    e.residentName = zoneResidentName(channel, roiId);
    // 서버 저장 규칙: ch{N}_{ms}_{TYPE}.mp4 (blackbox.trigger의 접미사와 같다)
    e.clipUrl = QStringLiteral("http://%1:%2/ch%3_%4_%5.mp4")
                    .arg(hostForChannel(channel))
                    .arg(kClipHttpPort)
                    .arg(channel)
                    .arg(occurredMs)
                    .arg(typeCode);

    const bool wasSorting = logTable->isSortingEnabled();
    logTable->setSortingEnabled(false);
    insertEventRow(e);
    logTable->setSortingEnabled(true);
    logTable->sortItems(LogWhen, Qt::DescendingOrder);   // 최신 이벤트가 위로
    applyLogFilters();
}

// 결과 건수/미확인 수 + 빈 상태 안내.
void MainWindow::refreshLogSummary()
{
    if (!logTable) return;

    int visible = 0, unconfirmed = 0;
    for (int r = 0; r < logTable->rowCount(); ++r) {
        if (logTable->isRowHidden(r)) continue;
        ++visible;
        auto* st = logTable->item(r, LogStatus);
        if (st && st->text() == QStringLiteral("미확인")) ++unconfirmed;
    }

    if (logCountLabel) {
        logCountLabel->setText(
            unconfirmed > 0
                ? QStringLiteral("%1건  ·  미확인 %2건").arg(visible).arg(unconfirmed)
                : QStringLiteral("%1건").arg(visible));
        logCountLabel->setProperty("alert", unconfirmed > 0);
        logCountLabel->style()->unpolish(logCountLabel);
        logCountLabel->style()->polish(logCountLabel);
    }

    if (logEmptyHint) {
        logEmptyHint->setVisible(visible == 0);
        if (visible == 0) {
            logEmptyHint->setText(
                QSqlDatabase::database().isOpen()
                    ? QStringLiteral("이 조건에 맞는 이벤트가 없습니다.\n"
                                     "왼쪽에서 기간을 넓히거나 종류·채널 필터를 풀어 보세요.")
                    : QStringLiteral("DB에 연결되지 않았습니다.\n"
                                     "연결되면 지난 이벤트 기록이 여기에 나타납니다."));
            logEmptyHint->setGeometry(logTable->viewport()->rect().adjusted(20, 40, -20, 0));
            logEmptyHint->raise();
        }
    }
}

QWidget* MainWindow::buildBlackboxPlayer()
{
    auto* card = new QFrame();
    card->setObjectName("videoCard");
    card->setMinimumHeight(220);

    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    blackboxPlaceholder = new QLabel(
        QStringLiteral("블랙박스 영상을 불러오는 중…"));
    blackboxPlaceholder->setAlignment(Qt::AlignCenter);
    blackboxPlaceholder->setObjectName("video");

    blackboxVideoWidget = new QVideoWidget();
    blackboxVideoWidget->setObjectName("video");

    // placeholder ↔ 영상은 hide/show 대신 스택으로 전환한다.
    // hide/show 방식은 두 위젯의 선호 크기가 다를 때(특히 QVideoWidget은
    // 영상 원본 해상도를 선호 크기로 보고) 전환·로드 때마다 카드가
    // 커졌다 작아졌다 하는 원인이 된다. 스택 + Ignored 정책으로 내용물이
    // 레이아웃 크기에 영향을 못 주게 고정한다.
    blackboxStack = new QStackedWidget();
    blackboxStack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    blackboxStack->addWidget(blackboxPlaceholder);
    blackboxStack->addWidget(blackboxVideoWidget);
    blackboxStack->setCurrentWidget(blackboxPlaceholder);
    lay->addWidget(blackboxStack, 1);

    // [재생/일시정지] + 재생바 + 시간(현재/전체) 한 줄
    auto* seekRow = new QHBoxLayout();
    seekRow->setSpacing(8);

    blackboxPlayPauseButton = new QPushButton();
    blackboxPlayPauseButton->setEnabled(false);
    blackboxPlayPauseButton->setCursor(Qt::PointingHandCursor);
    blackboxPlayPauseButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    seekRow->addWidget(blackboxPlayPauseButton);

    blackboxSeek = new ClickSeekSlider(Qt::Horizontal);
    blackboxSeek->setEnabled(false);
    blackboxSeek->setRange(0, 0);   // duration 확정 전엔 0
    seekRow->addWidget(blackboxSeek, 1);

    blackboxTimeLabel = new QLabel(QStringLiteral("00:00 / 00:00"));
    blackboxTimeLabel->setObjectName("subtitle");
    // 시간 표시 폭을 고정해 재생 중 숫자 폭 변화로 재생바가 흔들리지 않게.
    blackboxTimeLabel->setMinimumWidth(
        blackboxTimeLabel->fontMetrics().horizontalAdvance(
            QStringLiteral("00:00 / 00:00")) + 8);
    seekRow->addWidget(blackboxTimeLabel);
    lay->addLayout(seekRow);

    blackboxPlayer = new QMediaPlayer(this);
    blackboxPlayer->setVideoOutput(blackboxVideoWidget);

    // 재생/일시정지 버튼 ↔ 플레이어 상태 동기화
    connect(blackboxPlayPauseButton, &QPushButton::clicked, this, [this] {
        if (blackboxPlayer->playbackState() == QMediaPlayer::PlayingState)
            blackboxPlayer->pause();
        else
            blackboxPlayer->play();
    });
    connect(blackboxPlayer, &QMediaPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState st) {
        blackboxPlayPauseButton->setIcon(style()->standardIcon(
            st == QMediaPlayer::PlayingState ? QStyle::SP_MediaPause
                                             : QStyle::SP_MediaPlay));
    });

    // 클립 길이가 확정되면 슬라이더 범위를 ms 단위로 맞춘다(정확한 탐색).
    connect(blackboxPlayer, &QMediaPlayer::durationChanged, this, [this](qint64 dur) {
        blackboxSeek->setRange(0, static_cast<int>(dur));
        blackboxTimeLabel->setText(formatMs(blackboxPlayer->position()) +
                                   QStringLiteral(" / ") + formatMs(dur));
        // playBlackboxClipAt()이 예약해둔 탐색 위치 — 길이가 확정된 지금이
        // setPosition을 걸 수 있는 시점(그 전엔 무시되거나 씹힐 수 있음).
        if (blackboxPendingSeekMs_ >= 0) {
            blackboxPlayer->setPosition(qBound<qint64>(0, blackboxPendingSeekMs_, dur));
            blackboxPendingSeekMs_ = -1;
        }
    });
    // 재생 위치 변화 → 슬라이더/시간 갱신. 단, 사용자가 슬라이더를 잡고
    // 있는 동안은 덮어쓰지 않는다(안 그러면 드래그가 튕긴다).
    connect(blackboxPlayer, &QMediaPlayer::positionChanged, this, [this](qint64 pos) {
        if (!blackboxSeeking)
            blackboxSeek->setValue(static_cast<int>(pos));
        blackboxTimeLabel->setText(formatMs(pos) + QStringLiteral(" / ") +
                                   formatMs(blackboxPlayer->duration()));
    });
    // 슬라이더 조작 → 탐색. 잡는 동안 positionChanged를 무시하기 위해 플래그 사용.
    // ClickSeekSlider가 press 시점에 value를 클릭 지점으로 먼저 옮겨두므로,
    // 누르는 순간 그 위치로 즉시 탐색된다(유튜브식 클릭 점프).
    connect(blackboxSeek, &QSlider::sliderPressed, this, [this] {
        blackboxSeeking = true;
        blackboxPlayer->setPosition(blackboxSeek->value());
    });
    connect(blackboxSeek, &QSlider::sliderReleased, this, [this] {
        blackboxPlayer->setPosition(blackboxSeek->value());
        blackboxSeeking = false;
    });
    connect(blackboxSeek, &QSlider::sliderMoved, this, [this](int v) {
        blackboxTimeLabel->setText(formatMs(v) + QStringLiteral(" / ") +
                                   formatMs(blackboxPlayer->duration()));
    });

    connect(blackboxPlayer, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString& msg) {
        // 저장 완료 전에 눌렀으면 파일이 아직 없어 실패한다 — 몇 번 재시도.
        if (blackboxRetries < kClipMaxRetries && !blackboxUrl.isEmpty()) {
            ++blackboxRetries;
            blackboxPlaceholder->setText(
                QStringLiteral("블랙박스 저장 중… 잠시 후 재생됩니다 (%1)")
                    .arg(blackboxRetries));
            blackboxStack->setCurrentWidget(blackboxPlaceholder);
            const QString url = blackboxUrl;
            QTimer::singleShot(kClipRetryDelayMs, this, [this, url] {
                if (blackboxUrl == url) playBlackboxClip(url);
            });
            return;
        }
        blackboxPlaceholder->setText(
            QStringLiteral("재생 실패 — 클립을 찾을 수 없습니다\n(%1)").arg(msg));
        blackboxStack->setCurrentWidget(blackboxPlaceholder);
    });

    return card;
}

void MainWindow::playBlackboxClip(const QString& url)
{
    if (!blackboxPlayer) return;
    // 새 클립을 누른 것이면 재시도 카운터 초기화(같은 url 재시도면 유지).
    if (url != blackboxUrl) {
        blackboxUrl = url;
        blackboxRetries = 0;
    }

    // "이 시점 NVR에서 이어보기" 대상(selectedEventChannel_/Timestamp_)을 URL의
    // 파일명에서 직접 뽑아 채운다. 예전엔 이벤트 기록 행(onLogRowActivated)만
    // 이 값을 채워서, 검색 결과 클립을 직접 눌렀을 땐 채워지지 않아 버튼이
    // 계속 비활성 상태였다.
    // ★ 블랙박스 이벤트 클립(ch{N}_{ms}_{TYPE}.mp4, 3토큰)에서만 채운다 — NVR
    //   세그먼트(ch{N}_{ms}.mp4, 2토큰)까지 여기 걸리면, "이 시점 NVR에서
    //   이어보기"로 방금 도착한 세그먼트를 볼 때 그 세그먼트 자신의 시작시각으로
    //   원래 이벤트 시각을 덮어써버려서, 다음 판정이 엉뚱한 세그먼트/오프셋을
    //   가리키게 되는 버그가 있었다. NVR 세그먼트 재생 중엔 이 값을 그대로 둬서
    //   "원래 보러 온 이벤트"를 계속 가리키게 한다.
    const QString fileName = QUrl(url).fileName();
    const QString stem = fileName.left(fileName.lastIndexOf('.'));
    const QStringList parts = stem.split(QLatin1Char('_'));
    if (parts.size() >= 3 && parts[0].startsWith(QLatin1String("ch"))) {
        bool chOk = false, msOk = false;
        const int ch = parts[0].mid(2).toInt(&chOk);
        const qint64 ms = parts[1].toLongLong(&msOk);
        if (chOk && msOk && ch >= 0 && ch < 4 && ms > 0) {
            selectedEventChannel_ = ch;
            selectedEventTimestampMs_ = ms;
        }
    }
    if (nvrJumpButton)
        nvrJumpButton->setEnabled(selectedEventChannel_ >= 0 && selectedEventTimestampMs_ >= 0);
    blackboxStack->setCurrentWidget(blackboxVideoWidget);
    blackboxSeek->setEnabled(true);
    blackboxPlayPauseButton->setEnabled(true);

    // 같은 URL을 다시 setSource하면 QMediaPlayer가 "소스 변경 없음"으로 보고
    // 재로딩을 건너뛴다(특히 직전 재생이 끝까지 간 뒤). 그러면 같은 클립을
    // 연속으로 다시 틀 때 화면이 안 나온다 — 소스를 한 번 비웠다가 다시
    // 지정해 매번 확실히 처음부터 로드/재생되게 한다.
    blackboxPlayer->stop();
    blackboxPlayer->setSource(QUrl());
    blackboxPlayer->setSource(QUrl(url));
    blackboxPlayer->play();

    if (clipDownloadButton) clipDownloadButton->setEnabled(true);
}

// blackboxUrl로 재생을 시작하되, 길이가 확정되면(durationChanged) 한 번 지정
// 위치로 탐색한다 — 이벤트↔NVR 연결에서 "그 시점 근처로 이동"할 때 사용.
void MainWindow::playBlackboxClipAt(const QString& url, qint64 seekMs)
{
    blackboxPendingSeekMs_ = qMax<qint64>(0, seekMs);
    playBlackboxClip(url);
}

// 로그에서 선택된 이벤트(selectedEventChannel_/selectedEventTimestampMs_)를
// 담고 있는 NVR 세그먼트를 nvrSegments_에서 찾아 그 시점으로 재생한다.
// 매칭 규칙: 같은 채널에서 시작시각이 이벤트 시각보다 앞선 것 중 가장 늦게
// 시작한 세그먼트(=그 시각을 담고 있을 세그먼트) — 세그먼트 길이는 클라가
// 몰라 정확한 종료시각 검증은 못 하지만, 서버 로테이션이 순차적이라 이걸로
// 충분하다.
void MainWindow::jumpToNvrContext()
{
    if (selectedEventChannel_ < 0 || selectedEventTimestampMs_ < 0) return;

    bool found = false;
    NvrSegmentInfo best;
    for (const auto& seg : nvrSegments_) {
        if (seg.channel != selectedEventChannel_) continue;
        if (seg.startMs > selectedEventTimestampMs_) continue;
        if (!found || seg.startMs > best.startMs) {
            best = seg;
            found = true;
        }
    }
    if (!found) {
        QMessageBox::information(this, QStringLiteral("NVR 없음"),
            QStringLiteral("이 시점에 해당하는 NVR 녹화를 찾을 수 없습니다.\n"
                           "(보존기간 경과, 미녹화 구간, 또는 아직 목록을 못 받아왔을 수 있습니다 — "
                           "새로고침 후 다시 시도해보세요.)"));
        return;
    }
    playBlackboxClipAt(best.url, selectedEventTimestampMs_ - best.startMs);
}

// 현재 재생기에 로드된 클립(블랙박스든 NVR이든 — blackboxUrl 하나로 공용)을
// 사용자가 고른 위치에 로컬 파일로 저장한다.
void MainWindow::downloadCurrentClip()
{
    if (blackboxUrl.isEmpty()) return;

    const QUrl srcUrl(blackboxUrl);
    const QString suggestedName = srcUrl.fileName();
    const QString savePath = QFileDialog::getSaveFileName(
        this, QStringLiteral("클립 저장"), suggestedName,
        QStringLiteral("영상 파일 (*.mp4)"));
    if (savePath.isEmpty()) return;   // 사용자가 취소

    auto* manager = new QNetworkAccessManager(this);
    QNetworkReply* reply = manager->get(QNetworkRequest(srcUrl));
    connect(reply, &QNetworkReply::finished, this, [this, reply, savePath]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            QMessageBox::warning(this, QStringLiteral("다운로드 실패"), reply->errorString());
            return;
        }
        QFile file(savePath);
        if (!file.open(QIODevice::WriteOnly)) {
            QMessageBox::warning(this, QStringLiteral("다운로드 실패"),
                QStringLiteral("파일을 저장할 수 없습니다: %1").arg(savePath));
            return;
        }
        file.write(reply->readAll());
        file.close();
        QMessageBox::information(this, QStringLiteral("다운로드 완료"),
            QStringLiteral("저장됨: %1").arg(savePath));
    });
}

// NVR(연속녹화) 탐색 — 채널 콤보 + 세그먼트 목록. 재생기는 새로 안 만들고
// 위의 블랙박스 재생기(blackboxPlayer 등)를 그대로 재사용한다.
// 각 Pi의 NVR HTTP 서버(/list)에서 세그먼트 파일 목록을 받아 nvrSegments_에 누적하고
// 목록 UI를 다시 채운다. 파일명 규칙: ch{채널}_{세그먼트시작 unixMs}.mp4
void MainWindow::refreshNvrSegments()
{
    nvrSegments_.clear();
    for (int si = 0; si < kNumServers; ++si) {
        auto* manager = new QNetworkAccessManager(this);
        const QString host = serverHost(si);
        QUrl url(QStringLiteral("http://%1:%2/list").arg(host).arg(kNvrHttpPort));
        QNetworkReply* reply = manager->get(QNetworkRequest(url));

        connect(reply, &QNetworkReply::finished, this, [this, reply, host]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                qDebug() << "⚠️ NVR 세그먼트 목록 수집 실패(" << host << "):" << reply->errorString();
                return;
            }

            const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            if (!doc.isArray()) return;

            for (const QJsonValue& value : doc.array()) {
                const QString fileName = value.toString();
                const QString cleanName = fileName.left(fileName.lastIndexOf('.'));
                const QStringList parts = cleanName.split(QLatin1Char('_'));
                if (parts.size() != 2) continue;   // ch{N}_{startMs} 형식만 (이벤트 클립과 구분)

                const int channel = parts[0].mid(2).toInt();  // "ch1" -> 1
                const qint64 startMs = parts[1].toLongLong();
                if (channel < 0 || channel >= 4) continue;

                NvrSegmentInfo entry;
                entry.channel = channel;
                entry.startMs = startMs;
                entry.url = QStringLiteral("http://%1:%2/%3")
                                .arg(host).arg(kNvrHttpPort).arg(fileName);
                nvrSegments_.push_back(entry);
            }
            refreshTimeline();   // 세그먼트가 들어오면 하단 타임라인을 다시 그린다
        });
    }
}

void MainWindow::onLogRowActivated(int row, int /*column*/)
{
    if (!logTable) return;
    auto* item = logTable->item(row, LogWhen);
    if (!item) return;

    // 우측 재생기 위에 "언제 · 무슨 일 · 누구 · 어디" 한 줄. 영상만 덩그러니
    // 띄우면 지금 보는 게 어느 사건인지 표에서 눈을 떼는 순간 잊는다.
    if (eventContextLabel) {
        const QString who = logTable->item(row, LogResident)
                                ? logTable->item(row, LogResident)->text() : QString();
        const QString what = logTable->item(row, LogType)
                                 ? logTable->item(row, LogType)->text() : QString();
        const QString place = logTable->item(row, LogPlace)
                                  ? logTable->item(row, LogPlace)->text() : QString();
        eventContextLabel->setText(
            QStringLiteral("%1  ·  %2  ·  %3  ·  %4")
                .arg(item->text(), what, who, place));
        eventContextLabel->show();
    }

    const QString url = item->data(LogClipUrl).toString();

    // 이벤트↔NVR 연결용 — 이 행의 채널/시각을 기억해뒀다가 "NVR에서 이어보기"에서 쓴다.
    // 클립이 없어도(저장 실패·보관기간 경과) 연속녹화에는 남아 있을 수 있으므로
    // 이 값은 URL 유무와 무관하게 채운다.
    selectedEventChannel_ = item->data(LogChannel).toInt();
    selectedEventTimestampMs_ = item->data(LogTimestamp).toLongLong();
    if (nvrJumpButton)
        nvrJumpButton->setEnabled(selectedEventChannel_ >= 0 && selectedEventTimestampMs_ > 0);

    if (url.isEmpty()) {
        qDebug() << "블랙박스 재생 요청 — row" << row << "(클립 URL 없음)";
        if (blackboxPlaceholder)
            blackboxPlaceholder->setText(
                QStringLiteral("이 이벤트에는 저장된 클립이 없습니다.\n"
                               "아래 [이 시점 NVR에서 이어보기]를 눌러 보세요."));
        if (blackboxStack) blackboxStack->setCurrentIndex(0);
        return;
    }

    // 영상을 열었으므로 이 이벤트는 '확인' 처리
    markLogConfirmed(row);

    qDebug() << "블랙박스 재생 요청 —" << url;
    // 인라인 플레이어(이 페이지 우측)에서 바로 재생 — 페이지 이동 없음.
    playBlackboxClip(url);
}


// 상태 컬럼(3번)을 '미확인' → '확인'으로 바꾸고 초록색으로 표시.
void MainWindow::markLogConfirmed(int row)
{
    if (!logTable || row < 0 || row >= logTable->rowCount()) return;

    auto* statusItem = logTable->item(row, LogStatus);
    if (!statusItem) return;
    if (statusItem->text() == QStringLiteral("확인")) return;   // 이미 확인됨

    // 원장에도 남긴다 — 누가 언제 확인했는지는 사고 이후 되짚을 때 필요한 기록이다.
    // 화면에만 '확인'을 칠하면 앱을 껐다 켠 순간 그 사실이 사라진다.
    auto* whenItem = logTable->item(row, LogWhen);
    const qint64 eventId = whenItem ? whenItem->data(LogEventId).toLongLong() : -1;
    if (eventId > 0 && QSqlDatabase::database().isOpen()) {
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "UPDATE events SET confirmed_at = NOW(), confirmed_by = ? "
            "WHERE event_id = ? AND confirmed_at IS NULL"));
        q.addBindValue(currentUser.name);
        q.addBindValue(eventId);
        if (!q.exec())
            qDebug() << "이벤트 확인 기록 실패:" << q.lastError().text();
    }
    // eventId < 0 = 방금 소켓으로 온 행. 서버가 같은 이벤트를 원장에 쓰는 중이라
    // 여기서 UPDATE할 대상이 아직 없다 — 다음 조회 때 DB 행으로 대체되며,
    // 그때 확인 표시는 풀린다. 화면 표시만 먼저 바꿔 둔다.

    // 텍스트 변경 중 자동 재정렬로 행이 움직여 엉뚱한 셀을 건드리는 것 방지
    const bool wasSorting = logTable->isSortingEnabled();
    logTable->setSortingEnabled(false);

    statusItem->setText(QStringLiteral("확인"));
    if (!currentUser.name.isEmpty())
        statusItem->setToolTip(QStringLiteral("%1 확인").arg(currentUser.name));

    logTable->setSortingEnabled(wasSorting);
    refreshEventLog();   // 상태 배지 색 + 요약(미확인 수) 갱신
}


// 로그가 바뀔 때마다 호출 — 이벤트/상태 셀을 색으로 구분한다.
//  · 이벤트: 낙상=빨강, 침상이탈=주황   · 상태: 미확인=빨강, 확인=초록
void MainWindow::refreshEventLog()
{
    if (!logTable) return;

    const QColor cCritical(QString::fromLatin1(kCritical));
    const QColor cHigh(QString::fromLatin1(kHigh));
    const QColor cWarn(QString::fromLatin1(kWarn));
    const QColor cNormal(QString::fromLatin1(kNormal));
    const QColor cSub(QString::fromLatin1(kTextSub));

    for (int r = 0; r < logTable->rowCount(); ++r) {
        auto* typeItem = logTable->item(r, LogType);
        auto* stItem = logTable->item(r, LogStatus);
        if (!typeItem || !stItem) continue;

        // 종류별 색은 관제 화면의 경보색·타임라인 마커색과 같은 토큰을 쓴다 —
        // 같은 사건이 화면마다 다른 색이면 색으로 종류를 외울 수 없다.
        const QString t = typeItem->text();
        typeItem->setForeground(t == QStringLiteral("낙상")     ? cCritical
                                : t == QStringLiteral("침상이탈") ? cHigh
                                                                   : cWarn);
        typeItem->setTextAlignment(Qt::AlignCenter);

        const bool confirmed = (stItem->text() == QStringLiteral("확인"));
        stItem->setForeground(confirmed ? cNormal : cCritical);
        stItem->setTextAlignment(Qt::AlignCenter);

        if (auto* dt = logTable->item(r, LogWhen)) dt->setForeground(cSub);
        if (auto* src = logTable->item(r, LogSource)) {
            src->setForeground(cSub);
            src->setTextAlignment(Qt::AlignCenter);
        }
        if (auto* res = logTable->item(r, LogResident))
            res->setTextAlignment(Qt::AlignCenter);
    }

    refreshLogSummary();
}



// 실시간으로 얹힌 행이 현재 조건에 맞는지 걸러 낸다(행을 지우지 않고 숨김).
// 조건 필터링의 본체는 SQL(reloadEventLog)이며 이 함수는 그 보조다.
void MainWindow::applyLogFilters(bool withDates)
{
    if (!logTable) return;

    // 조건 필터링의 본체는 SQL(reloadEventLog)이다. 이 함수는 그 뒤에 실시간으로
    // 얹힌 행이 현재 조건에 맞는지만 걸러 낸다 — 방금 도착한 다른 채널 이벤트가
    // 조건을 무시하고 표에 남아 있으면 관제사가 필터를 믿지 못하게 된다.
    const QString evtSel = filterEventType ? filterEventType->currentText() : QString();
    const bool evtAll = evtSel.isEmpty() || evtSel == QStringLiteral("전체 이벤트");

    const QString roomSel = filterRoom ? filterRoom->currentText() : QString();
    const bool roomAll = roomSel.isEmpty() || roomSel == QStringLiteral("전체 병실");

    const int chSel = filterChannel ? filterChannel->currentData().toInt() : -1;

    const QString confSel = filterConfirmed ? filterConfirmed->currentText() : QString();

    for (int row = 0; row < logTable->rowCount(); ++row) {
        bool show = true;

        if (!evtAll) {
            auto* evtItem = logTable->item(row, LogType);
            show = evtItem && evtItem->text() == evtSel;
        }

        // 병실 — 위치 문구("{room} · 채널 N[ · 침대 M]")가 선택된 room으로 시작하는지.
        // room이 하나뿐인 지금은 늘 참이지만, 카메라가 늘어 room이 여럿이 되면
        // 이 매칭이 바로 실제 필터로 동작한다(로직 변경 불필요).
        if (show && !roomAll) {
            auto* placeItem = logTable->item(row, LogPlace);
            show = placeItem && placeItem->text().startsWith(roomSel);
        }

        if (show && chSel >= 0) {
            auto* whenItem = logTable->item(row, LogWhen);
            show = whenItem && whenItem->data(LogChannel).toInt() == chSel;
        }

        if (show && !confSel.isEmpty() && confSel != QStringLiteral("전체")) {
            auto* st = logTable->item(row, LogStatus);
            const bool confirmed = st && st->text() == QStringLiteral("확인");
            show = (confSel == QStringLiteral("확인만")) ? confirmed : !confirmed;
        }

        // 날짜 범위 — 0열 "yyyy-MM-dd HH:mm:ss"의 앞 10글자만 파싱
        if (show && withDates && filterDateFrom && filterDateTo) {
            auto* dtItem = logTable->item(row, LogWhen);
            const QDate d = dtItem
                                ? QDate::fromString(dtItem->text().left(10), QStringLiteral("yyyy-MM-dd"))
                                : QDate();
            show = d.isValid() && d >= filterDateFrom->date() && d <= filterDateTo->date();
        }

        logTable->setRowHidden(row, !show);
    }

    refreshEventLog();   // 필터/삽입 후 행 색 갱신
}
