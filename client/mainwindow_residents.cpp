// MainWindow — DB 관리 탭 — 입소자 등록·수정·퇴원·재입원, 변경 로그
// (mainwindow.cpp를 기능별로 나눈 파일 중 하나. 공용 헬퍼는 mainwindow_p.h)
#include "mainwindow_p.h"

// ═══════════════════════════════════════════════════════════
//  TAB3: DB 관리
// ═══════════════════════════════════════════════════════════
QWidget* MainWindow::buildDbTab()
{
    auto* panel = new QFrame();
    panel->setObjectName("panel");

    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(18, 16, 18, 16);
    outer->setSpacing(12);

    // ── 제목 + DB 상태 ──
    auto* titleRow = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("입소자 관리"));
    title->setObjectName("panelTitle");
    titleRow->addWidget(title);
    titleRow->addStretch();
    dbStatusDot = new QLabel();
    // 헤더 연결 상태등(7×7, #statusDot)과 objectName을 공유하면 그쪽의
    // border-radius:3px 규칙이 이 9×9 위젯에도 번진다 — 02-03 정정 B로 분리.
    dbStatusDot->setObjectName("dbStatusDot");
    dbStatusDot->setFixedSize(9, 9);
    // 사전 존재 결함(DYNAMIC-STYLE-INVENTORY.md #1, A-9): 이 점은 생성 시
    // 한 번 "정상" 색으로 칠해질 뿐, 실제 DB 연결 상태를 반영해 갱신하는
    // 호출부가 코드에 없다. 이 계획은 표시 메커니즘만 속성 기반으로 옮긴다
    // — 실제 DB 이벤트 배선은 새 기능이라 이 단계 요구사항 범위 밖이다.
    dbStatusDot->setProperty("severity", "normal");
    dbStatusDot->style()->unpolish(dbStatusDot);
    dbStatusDot->style()->polish(dbStatusDot);
    dbStatusDot->update();
    dbStatusText = new QLabel(QStringLiteral("DB 연결됨 · daboijo"));
    dbStatusText->setObjectName("statusText");
    titleRow->addWidget(dbStatusDot);
    titleRow->addWidget(dbStatusText);
    outer->addLayout(titleRow);

    // ── 상단 요약 통계 ──
    outer->addWidget(buildResidentSummary());

    // ── 본문: 좌 목록(마스터) | 우 상세/편집(디테일) ──
    auto* body = new QHBoxLayout();
    body->setSpacing(16);
    body->addWidget(buildResidentList(), 0);
    body->addWidget(buildResidentDetail(), 1);
    outer->addLayout(body, 1);

    return panel;
}

// 상단 요약 통계 바 — 재원 수 · 위험도 분포 · 채널 배정.
QWidget* MainWindow::buildResidentSummary()
{
    auto* host = new QWidget();
    auto* row = new QHBoxLayout(host);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(12);

    // category는 심각도가 아니라 통계 카테고리다 — resStatVal[category=...]
    // 규칙(base.qss)이 색을 결정한다. "재원" 카드는 이 전환으로 accent 텍스트
    // 색(FOUND-06 위반, 3:1 큰 글씨 기준조차 미달)에서 %(text)로 바뀐다 —
    // 의도된 시각 변화다.
    auto makeStat = [&](const QString& caption, const QString& category, QLabel*& ref) {
        auto* card = new QFrame();
        card->setObjectName("resStat");
        auto* v = new QVBoxLayout(card);
        v->setContentsMargins(16, 13, 16, 12);
        v->setSpacing(3);
        ref = new QLabel(QStringLiteral("0"));
        ref->setObjectName("resStatVal");
        ref->setProperty("category", category);
        auto* c = new QLabel(caption);
        c->setObjectName("resStatCap");
        v->addWidget(ref);
        v->addWidget(c);
        row->addWidget(card, 1);
    };

    makeStat(QStringLiteral("재원"),     QStringLiteral("active"),      resSumActive);
    makeStat(QStringLiteral("위험 상"),  QStringLiteral("danger-high"), resSumHigh);
    makeStat(QStringLiteral("위험 중"),  QStringLiteral("danger-mid"),  resSumMid);
    makeStat(QStringLiteral("위험 하"),  QStringLiteral("danger-low"),  resSumLow);
    makeStat(QStringLiteral("채널 배정"), QStringLiteral("channel"),    resSumCam);
    return host;
}

// 좌측 마스터 — 필터 세그먼트 + 검색 + 신규 + 세로 목록.
QWidget* MainWindow::buildResidentList()
{
    auto* wrap = new QFrame();
    wrap->setObjectName("listPanel");
    wrap->setFixedWidth(344);
    auto* v = new QVBoxLayout(wrap);
    v->setContentsMargins(12, 12, 12, 12);
    v->setSpacing(10);

    // 재원 / 전체 / 퇴원 세그먼트
    auto* seg = new QHBoxLayout();
    seg->setSpacing(0);
    const QString modes[3] = {QStringLiteral("재원"), QStringLiteral("전체"),
                              QStringLiteral("퇴원")};
    for (int i = 0; i < 3; ++i) {
        residentFilterBtns[i] = new QPushButton(modes[i]);
        residentFilterBtns[i]->setObjectName("segTab");
        residentFilterBtns[i]->setCheckable(true);
        residentFilterBtns[i]->setChecked(i == 0);
        residentFilterBtns[i]->setCursor(Qt::PointingHandCursor);
        const QString m = modes[i];
        connect(residentFilterBtns[i], &QPushButton::clicked, this,
                [this, m] { setResidentFilter(m); });
        seg->addWidget(residentFilterBtns[i]);
    }
    v->addLayout(seg);

    // 검색창
    residentSearchEdit = new QLineEdit();
    residentSearchEdit->setObjectName("searchEdit");
    residentSearchEdit->setPlaceholderText(QStringLiteral("🔍  이름 검색"));
    residentSearchEdit->setClearButtonEnabled(true);
    residentSearchEdit->setFixedHeight(34);
    connect(residentSearchEdit, &QLineEdit::textChanged, this,
            [this](const QString& t) { refreshResidentCards(t); });
    v->addWidget(residentSearchEdit);

    // 신규 등록
    auto* newBtn = new QPushButton(QStringLiteral("＋ 신규 등록"));
    newBtn->setObjectName("primaryButton");
    newBtn->setCursor(Qt::PointingHandCursor);
    newBtn->setFixedHeight(34);
    connect(newBtn, &QPushButton::clicked, this, [this] { openResidentEditor(-1); });
    v->addWidget(newBtn);

    // 개수 캡션
    residentCountLabel = new QLabel(QStringLiteral("재원 목록"));
    residentCountLabel->setObjectName("segCaption");
    v->addWidget(residentCountLabel);

    // 세로 목록(스크롤)
    auto* scroll = new QScrollArea();
    scroll->setObjectName("vitalScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    residentCardHost = new QWidget();
    residentCardHost->setObjectName("cardHost");
    auto* listLay = new QVBoxLayout(residentCardHost);
    listLay->setContentsMargins(0, 0, 6, 0);
    listLay->setSpacing(8);
    scroll->setWidget(residentCardHost);
    v->addWidget(scroll, 1);
    return wrap;
}

// 재원/전체/퇴원 필터 전환 → 세그먼트 강조 + 목록 재조회.
void MainWindow::setResidentFilter(const QString& mode)
{
    residentFilter_ = mode;
    const QString modes[3] = {QStringLiteral("재원"), QStringLiteral("전체"),
                              QStringLiteral("퇴원")};
    for (int i = 0; i < 3; ++i)
        if (residentFilterBtns[i]) residentFilterBtns[i]->setChecked(modes[i] == mode);
    refreshResidentCards(residentSearchEdit ? residentSearchEdit->text() : QString());
}

// 우측 디테일 — 플레이스홀더/편집기를 스택으로 전환. (예전 팝업 다이얼로그 내용을 내장)
QWidget* MainWindow::buildResidentDetail()
{
    residentDetailStack = new QStackedWidget();

    // 0) 플레이스홀더
    auto* ph = new QFrame();
    ph->setObjectName("detailPanel");
    auto* pv = new QVBoxLayout(ph);
    pv->addStretch();
    auto* phLabel = new QLabel(
        QStringLiteral("왼쪽 목록에서 입소자를 선택하거나\n＋ 신규 등록을 눌러 시작하세요."));
    phLabel->setObjectName("detailPlaceholder");
    phLabel->setAlignment(Qt::AlignCenter);
    pv->addWidget(phLabel);
    pv->addStretch();
    residentDetailStack->addWidget(ph);

    // 1) 편집기 (프로필 헤더 + 폼|이력 + 액션바)
    auto* editor = new QFrame();
    editor->setObjectName("detailPanel");
    auto* root = new QVBoxLayout(editor);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(14);

    // 상단 프로필 헤더
    auto* header = new QFrame();
    header->setObjectName("dlgHeader");
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(16, 12, 16, 12);
    hl->setSpacing(14);
    auto* nameCol = new QVBoxLayout();
    nameCol->setSpacing(2);
    dlgNameBig = new QLabel(QStringLiteral("신규 입소자"));
    dlgNameBig->setObjectName("dlgName");
    dlgSubMeta = new QLabel(QString());
    dlgSubMeta->setObjectName("dlgSub");
    nameCol->addWidget(dlgNameBig);
    nameCol->addWidget(dlgSubMeta);
    dlgRiskBadge = new QLabel();
    dlgRiskBadge->setObjectName("dlgRiskBadge");
    dlgStatusBadge = new QLabel();
    dlgStatusBadge->setObjectName("dlgStatusBadge");
    hl->addLayout(nameCol);
    hl->addStretch();
    hl->addWidget(dlgRiskBadge);
    hl->addWidget(dlgStatusBadge);
    root->addWidget(header);

    // 본문: 좌 폼(스크롤) | 우 입원이력
    auto* bodyLay = new QHBoxLayout();
    bodyLay->setSpacing(16);
    auto* formScroll = new QScrollArea();
    formScroll->setObjectName("vitalScroll");
    formScroll->setWidgetResizable(true);
    formScroll->setFrameShape(QFrame::NoFrame);
    formScroll->setWidget(buildResidentFormBody());
    bodyLay->addWidget(formScroll, 3);
    bodyLay->addWidget(buildAdmissionHistory(), 2);
    root->addLayout(bodyLay, 1);

    // 하단 액션바
    auto* footer = new QHBoxLayout();
    footer->setSpacing(8);
    auto* saveBtn = new QPushButton(QStringLiteral("저장"));
    saveBtn->setObjectName("primaryButton");
    saveBtn->setCursor(Qt::PointingHandCursor);
    saveBtn->setMinimumWidth(96);
    connect(saveBtn, &QPushButton::clicked, this, &MainWindow::onSaveResident);
    dlgDischargeBtn = new QPushButton(QStringLiteral("퇴원 처리"));
    dlgDischargeBtn->setObjectName("dangerButton");
    dlgDischargeBtn->setCursor(Qt::PointingHandCursor);
    connect(dlgDischargeBtn, &QPushButton::clicked, this, [this] {
        if (editStatus->currentText() == QStringLiteral("재원")) onDischargeResident();
        else                                                     onReadmitResident();
    });
    footer->addWidget(saveBtn);
    footer->addStretch();
    footer->addWidget(dlgDischargeBtn);
    root->addLayout(footer);

    residentDetailStack->addWidget(editor);
    residentDetailStack->setCurrentIndex(0);
    return residentDetailStack;
}

// 위험도/상태 텍스트를 색상 칩으로 만드는 헬퍼(파일 로컬).
// severity가 비어 있으면(예: "퇴원") 등급이 아니므로 도형을 붙이지 않고
// #riskChip 기본 규칙(중립색)에 맡긴다 — 상태 칩은 심각도가 아니다.
// 위젯이 아직 화면에 붙기 전에 만들어지므로(목록 재렌더마다 새로 생성) repolish
// 없이 속성만 설정해도 첫 표시 시 QSS가 적용된다 — #resRow의 inactive/selected
// 선례와 같다.
namespace {
QLabel* makeChip(const QString& text, const QString& severity) {
    const QString label = severity.isEmpty()
        ? text
        : severityGlyph(severity) + QStringLiteral(" ") + text;
    auto* chip = new QLabel(label);
    chip->setObjectName("riskChip");
    chip->setAttribute(Qt::WA_TransparentForMouseEvents);
    chip->setProperty("severity", severity);
    return chip;
}
}  // namespace

// 좌측 목록을 DB에서 다시 채운다. 현재 필터(재원/전체/퇴원) + 이름 검색을 함께 적용.
// 각 행 클릭 → 우측 인라인 편집기 표시. 선택된 행은 강조.
void MainWindow::refreshResidentCards(const QString& nameFilter)
{
    if (!residentCardHost || !residentCardHost->layout()) return;
    auto* box = qobject_cast<QVBoxLayout*>(residentCardHost->layout());
    if (!box) return;

    // 기존 행 제거(스트레치 포함)
    QLayoutItem* old;
    while ((old = box->takeAt(0))) {
        if (old->widget()) old->widget()->deleteLater();
        delete old;
    }

    const QString trimmed = nameFilter.trimmed();
    const bool searching = !trimmed.isEmpty();

    QString sql = QStringLiteral(
        "SELECT resident_id, name, room, bed, camera_id, wearable_id, risk_level, status "
        "FROM residents WHERE 1=1");
    if (residentFilter_ == QStringLiteral("재원"))      sql += QStringLiteral(" AND status='재원'");
    else if (residentFilter_ == QStringLiteral("퇴원")) sql += QStringLiteral(" AND status='퇴원'");
    if (searching) sql += QStringLiteral(" AND name LIKE ?");
    sql += QStringLiteral(" ORDER BY status DESC, camera_id, resident_id");

    QSqlQuery q;
    q.prepare(sql);
    if (searching) q.addBindValue(QStringLiteral("%%1%").arg(trimmed));
    if (!q.exec()) {
        qDebug() << "입소자 목록 조회 실패:" << q.lastError().text();
        return;
    }

    int n = 0;
    while (q.next()) {
        const int     id      = q.value(0).toInt();
        const QString name    = q.value(1).toString();
        const QVariant camVar  = q.value(4);
        const QString risk    = q.value(6).toString();
        const QString status  = q.value(7).toString();
        const bool    active  = (status == QStringLiteral("재원"));
        const QString riskSeverity = risk == QStringLiteral("상") ? QStringLiteral("critical")
                                   : risk == QStringLiteral("중") ? QStringLiteral("medium")
                                                                  : QStringLiteral("normal");

        // 행 = 클릭 가능한 버튼. 좌측에 위험도 색 띠(riskSeverity)로 위험도를 시각화.
        auto* rowBtn = new QPushButton();
        rowBtn->setObjectName("resRow");
        rowBtn->setProperty("inactive", !active);
        rowBtn->setProperty("selected", id == selectedResidentCardId);
        rowBtn->setCursor(Qt::PointingHandCursor);
        rowBtn->setFixedHeight(64);
        connect(rowBtn, &QPushButton::clicked, this, [this, id] { openResidentEditor(id); });

        auto* rl = new QHBoxLayout(rowBtn);
        rl->setContentsMargins(8, 8, 12, 8);
        rl->setSpacing(10);

        // 위험도 색 띠 — 글자가 없어 도형을 못 붙인다. 이 띠의 색 외 채널은
        // 같은 행의 위험도 칩(아래) 도형이다 — 이 짝짓기를 깨지 말 것(D-09).
        auto* riskBar = new QLabel();
        riskBar->setObjectName("riskBar");
        riskBar->setAttribute(Qt::WA_TransparentForMouseEvents);
        riskBar->setFixedWidth(4);
        riskBar->setProperty("severity", riskSeverity);
        rl->addWidget(riskBar);

        // 이름 + 채널
        auto* nameCol = new QVBoxLayout();
        nameCol->setSpacing(1);
        auto* nameLbl = new QLabel(name.isEmpty() ? QStringLiteral("(이름 없음)") : name);
        nameLbl->setObjectName("resName");
        nameLbl->setAttribute(Qt::WA_TransparentForMouseEvents);
        const QString chStr = camVar.isNull()
            ? QStringLiteral("채널 미지정")
            : QStringLiteral("채널 %1").arg(camVar.toInt() + 1);
        auto* metaLbl = new QLabel(chStr);
        metaLbl->setObjectName("resMeta");
        metaLbl->setAttribute(Qt::WA_TransparentForMouseEvents);
        nameCol->addWidget(nameLbl);
        nameCol->addWidget(metaLbl);
        rl->addLayout(nameCol, 1);

        // 우측: 위험도 칩(퇴원 행은 상태 칩 — 심각도가 아니므로 severity를 비운다)
        if (active)
            rl->addWidget(makeChip(QStringLiteral("위험 %1")
                                       .arg(risk.isEmpty() ? QStringLiteral("-") : risk),
                                   riskSeverity));
        else
            rl->addWidget(makeChip(QStringLiteral("퇴원"), QString()));

        box->addWidget(rowBtn);
        ++n;
    }
    box->addStretch(1);

    if (residentCountLabel) {
        const QString scope = searching ? QStringLiteral("검색 결과")
                                        : QStringLiteral("%1 목록").arg(residentFilter_);
        residentCountLabel->setText(QStringLiteral("%1 · %2명").arg(scope).arg(n));
    }
    refreshResidentSummary();
}

// 상단 요약 통계 — 재원 수 · 위험도(상/중/하) 분포 · 채널 배정 수.
void MainWindow::refreshResidentSummary()
{
    if (!resSumActive) return;
    int active = 0, high = 0, mid = 0, low = 0, cams = 0;

    QSqlQuery q;
    if (q.exec(QStringLiteral(
            "SELECT COUNT(*), "
            " SUM(risk_level='상'), SUM(risk_level='중'), SUM(risk_level='하'), "
            " COUNT(DISTINCT CASE WHEN camera_id BETWEEN 0 AND 3 THEN camera_id END) "
            "FROM residents WHERE status='재원'")) && q.next()) {
        active = q.value(0).toInt();
        high   = q.value(1).toInt();
        mid    = q.value(2).toInt();
        low    = q.value(3).toInt();
        cams   = q.value(4).toInt();
    }
    resSumActive->setText(QStringLiteral("%1명").arg(active));
    resSumHigh->setText(QString::number(high));
    resSumMid->setText(QString::number(mid));
    resSumLow->setText(QString::number(low));
    resSumCam->setText(QStringLiteral("%1/4").arg(cams));
}

// 편집 다이얼로그에 들어갈 폼(그룹들) — 버튼/스크롤은 다이얼로그 쪽에서 감싼다.
QWidget* MainWindow::buildResidentFormBody()
{
    auto* inner = new QWidget();
    auto* lay = new QVBoxLayout(inner);
    lay->setSpacing(12);
    lay->setContentsMargins(0, 0, 6, 0);

    auto makeGroup = [](const QString& title) {
        auto* g = new QGroupBox(title);
        g->setObjectName("formGroup");
        return g;
    };
    auto makeField = [](const QString& placeholder, QLineEdit*& ref) {
        ref = new QLineEdit();
        ref->setObjectName("formEdit");
        ref->setPlaceholderText(placeholder);
        return ref;
    };

    // ── 기본정보 ──
    auto* basicGroup = makeGroup(QStringLiteral("기본 정보"));
    auto* basicForm = new QFormLayout(basicGroup);
    basicForm->setSpacing(8);
    basicForm->setLabelAlignment(Qt::AlignLeft);
    basicForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    basicForm->addRow(QStringLiteral("이름"),        makeField("홍길동", editName));
    basicForm->addRow(QStringLiteral("호실"),        makeField("예: 101", editRoom));
    basicForm->addRow(QStringLiteral("카메라 채널"), makeField("1~4", editCameraId));
    basicForm->addRow(QStringLiteral("웨어러블 ID"), makeField("기기 번호", editWearableId));
    lay->addWidget(basicGroup);

    // ── 케어 정보 ──
    auto* careGroup = makeGroup(QStringLiteral("케어 정보"));
    auto* careForm = new QFormLayout(careGroup);
    careForm->setSpacing(8);
    careForm->setLabelAlignment(Qt::AlignLeft);
    careForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    editCaregiver = new QComboBox();
    editCaregiver->setObjectName("formEdit");
    editCaregiver->addItem(QStringLiteral("(미지정)"));
    // TODO(core): SELECT name FROM caregivers WHERE status='재직' 으로 채우기
    careForm->addRow(QStringLiteral("담당 요양사"), editCaregiver);

    editRiskLevel = new QComboBox();
    editRiskLevel->setObjectName("formEdit");
    editRiskLevel->addItems({QStringLiteral("상"), QStringLiteral("중"), QStringLiteral("하")});
    careForm->addRow(QStringLiteral("위험도"), editRiskLevel);

    editAdmittedAt = new QDateEdit(QDate::currentDate());
    editAdmittedAt->setCalendarPopup(true);
    editAdmittedAt->setObjectName("formEdit");
    careForm->addRow(QStringLiteral("입원일"), editAdmittedAt);

    editDischargeDue = new QDateEdit(QDate::currentDate().addMonths(1));
    editDischargeDue->setCalendarPopup(true);
    editDischargeDue->setObjectName("formEdit");
    careForm->addRow(QStringLiteral("퇴원 예정일"), editDischargeDue);

    editStatus = new QComboBox();
    editStatus->setObjectName("formEdit");
    // 재원을 먼저 둬서 신규 등록 시 기본값이 재원이 되도록 한다.
    editStatus->addItems({QStringLiteral("재원"), QStringLiteral("퇴원")});
    careForm->addRow(QStringLiteral("상태"), editStatus);
    lay->addWidget(careGroup);

    // ── 특이사항 ──
    auto* notesGroup = makeGroup(QStringLiteral("특이사항"));
    auto* notesLay = new QVBoxLayout(notesGroup);
    editNotes = new QTextEdit();
    editNotes->setObjectName("formEdit");
    editNotes->setPlaceholderText(QStringLiteral("지병, 알레르기, 거동 가능 여부 등"));
    editNotes->setMaximumHeight(80);
    notesLay->addWidget(editNotes);
    lay->addWidget(notesGroup);
    lay->addStretch();

    return inner;
}

// 편집기 상단 프로필(아바타·이름·병상·배지·퇴원버튼)을 폼 현재값 기준으로 갱신.
void MainWindow::refreshResidentDialogHeader()
{
    if (!dlgNameBig) return;

    const bool isNew  = (selectedResidentId < 0);
    const QString name = editName->text().trimmed();
    const bool active  = (editStatus->currentText() == QStringLiteral("재원"));

    dlgNameBig->setText(isNew ? QStringLiteral("신규 입소자")
                              : (name.isEmpty() ? QStringLiteral("(이름 없음)") : name));

    const QString cam = editCameraId->text().trimmed();
    dlgSubMeta->setText(cam.isEmpty() ? QStringLiteral("채널 미지정")
                                      : QStringLiteral("채널 %1").arg(cam));

    // 이 함수는 콤보박스 값이 바뀔 때마다 다시 불리므로(생성 시 1회가 아니다)
    // 매번 repolish가 필요하다.
    const QString risk = editRiskLevel->currentText();
    const QString riskSeverity = risk == QStringLiteral("상") ? QStringLiteral("critical")
                                : risk == QStringLiteral("중") ? QStringLiteral("medium")
                                                                : QStringLiteral("normal");
    dlgRiskBadge->setText(severityGlyph(riskSeverity) + QStringLiteral(" ")
                           + QStringLiteral("위험 %1").arg(risk));
    dlgRiskBadge->setProperty("severity", riskSeverity);
    dlgRiskBadge->style()->unpolish(dlgRiskBadge);
    dlgRiskBadge->style()->polish(dlgRiskBadge);
    dlgRiskBadge->update();

    // 상태 배지(재원/퇴원)는 심각도가 아니다 — 재원 여부는 등급이 아니라
    // 분류다. severity를 쓰지 않고 별도 속성 residency로만 다루며 도형도
    // 붙이지 않는다.
    dlgStatusBadge->setText(active ? QStringLiteral("재원") : QStringLiteral("퇴원"));
    dlgStatusBadge->setProperty("residency", active ? "active" : "discharged");
    dlgStatusBadge->style()->unpolish(dlgStatusBadge);
    dlgStatusBadge->style()->polish(dlgStatusBadge);
    dlgStatusBadge->update();

    dlgRiskBadge->setVisible(!isNew);
    dlgStatusBadge->setVisible(!isNew);

    // 신규는 퇴원/재입원 불가 → 버튼 숨김. 기존은 상태에 따라 라벨 토글.
    dlgDischargeBtn->setVisible(!isNew);
    dlgDischargeBtn->setText(active ? QStringLiteral("퇴원 처리")
                                    : QStringLiteral("재입원"));
    // 신규 등록은 이력이 있을 수 없으므로 패널 자체를 숨겨 폼이 전체 폭을 쓰게 한다.
    if (admissionBox) admissionBox->setVisible(!isNew);
}

// 목록 행 클릭/신규 버튼 → 폼을 채우고 우측 인라인 편집기를 보여준다(팝업 없음).
void MainWindow::openResidentEditor(int residentId)
{
    if (residentId < 0) onNewResident();
    else                loadResidentIntoForm(residentId);
    refreshResidentDialogHeader();
    selectedResidentCardId = residentId;             // 목록에서 강조할 대상
    if (residentDetailStack) residentDetailStack->setCurrentIndex(1);
    refreshResidentCards(residentSearchEdit ? residentSearchEdit->text() : QString());
}

QWidget* MainWindow::buildAdmissionHistory()
{
    auto* box = new QGroupBox(QStringLiteral("입원 이력"));
    box->setObjectName("formGroup");
    auto* lay = new QVBoxLayout(box);
    lay->setSpacing(6);

    admissionInfo = new QLabel(
        QStringLiteral("입소자를 선택하면 입원 이력이 표시됩니다. "
                       "행을 더블클릭하면 그 기간의 변경 내역이 열립니다."));
    admissionInfo->setObjectName("segCaption");
    admissionInfo->setWordWrap(true);
    lay->addWidget(admissionInfo);

    admissionTable = new QTableWidget(0, 4);
    admissionTable->setObjectName("logTable");
    admissionTable->setHorizontalHeaderLabels(
        {QStringLiteral("입원일"), QStringLiteral("퇴원일"),
         QStringLiteral("상태"),   QStringLiteral("변경")});
    admissionTable->horizontalHeader()->setStretchLastSection(true);
    admissionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    admissionTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    admissionTable->verticalHeader()->setVisible(false);
    connect(admissionTable, &QTableWidget::cellDoubleClicked,
            this, &MainWindow::onAdmissionRowActivated);
    lay->addWidget(admissionTable, 1);
    admissionBox = box;   // 신규 등록 시 숨기기 위해 보관
    return box;
}

void MainWindow::refreshAdmissionTable(int residentId)
{
    if (!admissionTable) return;
    admissionTable->setRowCount(0);
    if (residentId < 0) {
        admissionInfo->setText(QStringLiteral("입소자를 선택하세요."));
        return;
    }

    QSqlQuery q;
    q.prepare(QStringLiteral(
        "SELECT a.admission_id, a.admitted_at, a.discharged_at, a.status, "
        " (SELECT COUNT(*) FROM resident_changes c "
        "   WHERE c.admission_id = a.admission_id) AS chg "
        "FROM admissions a WHERE a.resident_id=? "
        "ORDER BY a.admitted_at DESC, a.admission_id DESC"));
    q.addBindValue(residentId);
    if (!q.exec()) {
        qDebug() << "입원 이력 조회 실패:" << q.lastError().text();
        return;
    }

    while (q.next()) {
        const int row = admissionTable->rowCount();
        admissionTable->insertRow(row);

        auto* inItem = new QTableWidgetItem(q.value(1).toDate().toString("yyyy-MM-dd"));
        inItem->setData(Qt::UserRole, q.value(0).toInt());   // admission_id 숨겨둠
        admissionTable->setItem(row, 0, inItem);

        const QVariant out = q.value(2);
        admissionTable->setItem(row, 1, new QTableWidgetItem(
                                            out.isNull() ? QStringLiteral("—") : out.toDate().toString("yyyy-MM-dd")));
        admissionTable->setItem(row, 2, new QTableWidgetItem(q.value(3).toString()));
        admissionTable->setItem(row, 3, new QTableWidgetItem(
                                            QStringLiteral("%1건").arg(q.value(4).toInt())));
    }

    admissionInfo->setText(QStringLiteral("입원 %1건 — 행 더블클릭 시 변경 내역")
                               .arg(admissionTable->rowCount()));
}

void MainWindow::onAdmissionRowActivated(int row, int /*column*/)
{
    auto* item = admissionTable ? admissionTable->item(row, 0) : nullptr;
    if (!item) return;
    showChangeLogDialog(item->data(Qt::UserRole).toInt());
}

// 선택한 입원 에피소드의 변경 내역만 팝업으로 — [수정 전] → [수정 후] 형식.
void MainWindow::showChangeLogDialog(int admissionId)
{
    QSqlQuery q;
    q.prepare(QStringLiteral(
        "SELECT changed_at, change_type, field_label, old_value, new_value, changed_by "
        "FROM resident_changes WHERE admission_id=? "
        "ORDER BY changed_at DESC, change_id DESC"));
    q.addBindValue(admissionId);
    if (!q.exec()) {
        QMessageBox::critical(this, QStringLiteral("조회 실패"), q.lastError().text());
        return;
    }

    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);   // 닫으면 자동 정리
    dlg->setObjectName("panel");
    dlg->setWindowTitle(QStringLiteral("변경 내역"));
    dlg->resize(820, 480);
    enableDarkTitleBar(dlg);

    auto* lay = new QVBoxLayout(dlg);
    lay->setContentsMargins(16, 16, 16, 16);
    lay->setSpacing(10);

    auto* tbl = new QTableWidget(0, 5);
    tbl->setObjectName("logTable");
    tbl->setHorizontalHeaderLabels(
        {QStringLiteral("시각"), QStringLiteral("구분"), QStringLiteral("항목"),
         QStringLiteral("변경 내용"), QStringLiteral("작업자")});
    tbl->horizontalHeader()->setStretchLastSection(false);
    tbl->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    tbl->setSelectionBehavior(QAbstractItemView::SelectRows);
    tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tbl->verticalHeader()->setVisible(false);

    auto shown = [](const QString& s) {
        return s.isEmpty() ? QStringLiteral("(없음)") : s;
    };

    while (q.next()) {
        const int row = tbl->rowCount();
        tbl->insertRow(row);
        tbl->setItem(row, 0, new QTableWidgetItem(
                                 q.value(0).toDateTime().toString("yyyy-MM-dd HH:mm:ss")));
        tbl->setItem(row, 1, new QTableWidgetItem(q.value(1).toString()));
        tbl->setItem(row, 2, new QTableWidgetItem(q.value(2).toString()));
        tbl->setItem(row, 3, new QTableWidgetItem(
                                 QStringLiteral("[수정 전] %1   →   [수정 후] %2")
                                     .arg(shown(q.value(3).toString()), shown(q.value(4).toString()))));
        tbl->setItem(row, 4, new QTableWidgetItem(q.value(5).toString()));
    }
    tbl->resizeColumnsToContents();
    tbl->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);

    if (tbl->rowCount() == 0) {
        auto* empty = new QLabel(QStringLiteral("이 입원 기간에 기록된 변경 내역이 없습니다."));
        empty->setObjectName("segCaption");
        lay->addWidget(empty);
    }
    lay->addWidget(tbl, 1);

    auto* closeBox = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    closeBox->button(QDialogButtonBox::Close)->setText(QStringLiteral("닫기"));
    closeBox->button(QDialogButtonBox::Close)->setObjectName(QStringLiteral("roiButton"));
    connect(closeBox, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    lay->addWidget(closeBox);

    dlg->show();
}



void MainWindow::loadPatientsFromDb()
{
    const QString room = currentRoomName();
    for (int ch = 0; ch < 4; ++ch) {
        patients[ch] = { QStringLiteral("미배정"),
                         QStringLiteral("%1 · 채널 %2").arg(room).arg(ch + 1),
                         QString() };
        residentsByChannel_[ch].clear();
    }
    wearableToResident.clear();
    residentInfo_.clear();

    QSqlQuery q;
    if (!q.exec(QStringLiteral(
            "SELECT resident_id, camera_id, name, room, wearable_id FROM residents "
            "WHERE status='재원' AND camera_id BETWEEN 0 AND 3 "
            "ORDER BY camera_id, resident_id"))) {
        qDebug() << "채널 환자 매핑 조회 실패:" << q.lastError().text();
        return;
    }

    bool assigned[4] = {};
    while (q.next()) {
        const int rid = q.value(0).toInt();
        const int ch  = q.value(1).toInt();
        if (ch < 0 || ch >= 4) continue;

        ResidentInfo info;
        info.name    = q.value(2).toString();
        info.room    = q.value(3).toString();
        // 위치는 room+채널로 표기. 침상 구분이 생기기 전까지는 같은 채널이면 같은 값.
        info.bed     = QStringLiteral("%1 · 채널 %2").arg(room).arg(ch + 1);
        info.channel = ch;
        residentInfo_.insert(rid, info);
        residentsByChannel_[ch].append(rid);

        // 웨어러블은 그 채널의 모든 입소자 것을 등록한다. 브로커는 기기 id로만
        // 알려주므로, 여기 등록이 빠지면 그 기기가 보낸 값은 주인을 몰라 버려진다.
        const QString wearable = q.value(4).toString().trimmed();
        if (!wearable.isEmpty()) wearableToResident.insert(wearable, rid);

        if (assigned[ch]) continue;   // 오버레이·케어 카드는 채널당 대표 1명만
        assigned[ch] = true;
        patients[ch].name = info.name;
        patients[ch].bed  = info.bed;
        patients[ch].room = info.room;
    }

    // 목록에서 빠진 입소자(퇴원 등)의 값·이력은 버린다. 남겨두면 나중에 재입원했을 때
    // 몇 달 전 그래프가 되살아나고, 계속 쌓이기만 해서 줄어들 일이 없다.
    for (auto it = vitals_.begin(); it != vitals_.end(); ) {
        if (residentInfo_.contains(it.key())) ++it;
        else it = vitals_.erase(it);
    }
    for (auto it = hrHistory_.begin(); it != hrHistory_.end(); ) {
        if (residentInfo_.contains(it.key())) ++it;
        else it = hrHistory_.erase(it);
    }
}

// 입소자 정보를 실제 화면에 다시 반영한다.
// 영상 오버레이는 채널당 한 줄이라 대표 1명(patients[])을 쓰고,
// 바이탈 카드는 입소자 수만큼 개수가 달라져 통째로 다시 만든다.
// 케어 타임 대시보드는 updateCareTime()에서 patients[]를 직접 읽으므로 여기선 제외.
void MainWindow::refreshPatientLabels()
{
    for (int ch = 0; ch < 4; ++ch) {
        // 타일 좌상단 이름(LIVE 옆)도 같은 소스에서 만든다.
        if (channelViews[ch]) channelViews[ch]->setDisplayName(tileDisplayName(ch));
        // 침대 이름표·매핑 콤보도 새 입소자 구성으로 다시 그린다 — 이름을 고치거나
        // 퇴원시켰는데 침대 라벨만 옛 이름으로 남으면 관제사가 오판한다.
        refreshRoiZones(ch);
    }
    rebuildVitalCards();
    refreshResourceTree();
}

void MainWindow::onResidentSearch()
{
    // 검색창 텍스트로 필터. 비어 있으면 재원자 카드만 다시 그린다.
    refreshResidentCards(residentSearchEdit ? residentSearchEdit->text()
                                            : QString());
}

// 선택한 입소자를 편집 폼 필드에 채운다(다이얼로그 열기 전 호출).
void MainWindow::loadResidentIntoForm(int id)
{
    QSqlQuery q;
    q.prepare(QStringLiteral(
        "SELECT name, room, bed, camera_id, wearable_id, risk_level, "
        "admitted_at, discharge_due, status, notes "
        "FROM residents WHERE resident_id = ?"));
    q.addBindValue(id);
    if (!q.exec() || !q.next()) {
        qDebug() << "입소자 상세 조회 실패:" << q.lastError().text();
        return;
    }

    selectedResidentId = id;

    // 콤보는 텍스트로 매칭해 선택
    auto setCombo = [](QComboBox* c, const QString& text) {
        const int i = c->findText(text);
        if (i >= 0) c->setCurrentIndex(i);
    };

    editName->setText(q.value(0).toString());
    editRoom->setText(q.value(1).toString());
    // value(2)=bed 는 침대 항목 제거로 폼에 반영하지 않는다.
    // DB는 0~3으로 저장, 화면엔 1~4로 보여준다(사람이 읽기 쉬운 채널 번호).
    editCameraId->setText(q.value(3).isNull() ? QString()
                                              : QString::number(q.value(3).toInt() + 1));
    editWearableId->setText(q.value(4).isNull() ? QString() : q.value(4).toString());
    setCombo(editRiskLevel, q.value(5).toString());
    if (q.value(6).toDate().isValid())  editAdmittedAt->setDate(q.value(6).toDate());
    if (q.value(7).toDate().isValid())  editDischargeDue->setDate(q.value(7).toDate());
    setCombo(editStatus, q.value(8).toString());
    editNotes->setPlainText(q.value(9).toString());

    refreshAdmissionTable(selectedResidentId);

    qDebug() << "입소자 선택 — ID:" << selectedResidentId;
}

void MainWindow::onNewResident()
{
    selectedResidentId = -1;
    editName->clear();
    editRoom->clear();
    editCameraId->clear();
    editWearableId->clear();
    editNotes->clear();
    editRiskLevel->setCurrentIndex(1);   // 위험도 기본 '중'
    editStatus->setCurrentText(QStringLiteral("재원"));  // 신규는 항상 재원으로 시작
    editAdmittedAt->setDate(QDate::currentDate());
    editDischargeDue->setDate(QDate::currentDate().addMonths(1));
    refreshAdmissionTable(-1);   // 아직 저장 안 된 신규 → 이력 없음
    editName->setFocus();
}

// 저장 직전 DB의 값 — 라벨→문자열 맵. (변경 "전" 스냅샷)
QMap<QString, QString> MainWindow::snapshotResident(int id)
{
    QMap<QString, QString> m;
    QStringList cols;
    for (const auto& f : kLoggedFields) cols << QLatin1String(f.column);

    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT %1 FROM residents WHERE resident_id=?")
                  .arg(cols.join(QLatin1Char(','))));
    q.addBindValue(id);
    if (!q.exec() || !q.next()) return m;

    int i = 0;
    for (const auto& f : kLoggedFields) {
        const QVariant v = q.value(i++);
        QString text = v.isNull() ? QString() : v.toString();
        // camera_id는 DB(0~3) → 표시(1~4)로 맞춰 폼 스냅샷과 같은 기준으로 비교/기록.
        if (!v.isNull() && QLatin1String(f.column) == QLatin1String("camera_id"))
            text = QString::number(v.toInt() + 1);
        m.insert(QString::fromUtf8(f.label), text);
    }
    return m;
}

// 현재 폼에 입력된 값 — 위와 같은 라벨 체계로 (변경 "후" 스냅샷)
QMap<QString, QString> MainWindow::formSnapshot() const
{
    QMap<QString, QString> m;
    m[QStringLiteral("이름")]        = editName->text().trimmed();
    m[QStringLiteral("호실")]        = editRoom->text().trimmed();
    m[QStringLiteral("카메라 채널")] = editCameraId->text().trimmed();
    m[QStringLiteral("웨어러블 ID")] = editWearableId->text().trimmed();
    m[QStringLiteral("위험도")]      = editRiskLevel->currentText();
    m[QStringLiteral("입원일")]      = editAdmittedAt->date().toString(Qt::ISODate);
    m[QStringLiteral("퇴원 예정일")] = editDischargeDue->date().toString(Qt::ISODate);
    m[QStringLiteral("상태")]        = editStatus->currentText();
    m[QStringLiteral("특이사항")]    = editNotes->toPlainText();
    return m;
}

// 그 입소자의 "가장 최근" 입원 에피소드 id (없으면 -1)
int MainWindow::currentAdmissionId(int residentId)
{
    QSqlQuery q;
    q.prepare(QStringLiteral(
        "SELECT admission_id FROM admissions WHERE resident_id=? "
        "ORDER BY admitted_at DESC, admission_id DESC LIMIT 1"));
    q.addBindValue(residentId);
    return (q.exec() && q.next()) ? q.value(0).toInt() : -1;
}

// before↔after를 비교해 달라진 필드만 resident_changes에 남긴다.
void MainWindow::logChanges(int residentId, int admissionId,
                            const QMap<QString, QString>& before,
                            const QMap<QString, QString>& after,
                            const QString& changeType)
{
    QSqlQuery q;
    q.prepare(QStringLiteral(
        "INSERT INTO resident_changes "
        "(resident_id, admission_id, field_label, old_value, new_value, "
        " change_type, changed_by) VALUES (?,?,?,?,?,?,?)"));

    for (auto it = after.constBegin(); it != after.constEnd(); ++it) {
        const QString oldV = before.value(it.key());
        if (oldV == it.value()) continue;      // 안 바뀐 건 로그 남기지 않음
        q.bindValue(0, residentId);
        q.bindValue(1, admissionId > 0 ? QVariant(admissionId) : QVariant());
        q.bindValue(2, it.key());
        q.bindValue(3, oldV);
        q.bindValue(4, it.value());
        q.bindValue(5, changeType);
        q.bindValue(6, currentUser.name);      // 로그인 사용자 = 작업자
        if (!q.exec())
            qDebug() << "변경 로그 기록 실패:" << q.lastError().text();
    }
}

void MainWindow::onSaveResident()
{
    if (editName->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("입력 오류"),
                             QStringLiteral("이름을 입력해주세요."));
        return;
    }

    // 카메라 채널은 화면에서 1~4로 입력받되 DB엔 0~3으로 저장한다.
    const QString camText = editCameraId->text().trimmed();
    if (!camText.isEmpty()) {
        bool ok = false;
        const int camNo = camText.toInt(&ok);
        if (!ok || camNo < 1 || camNo > 4) {
            QMessageBox::warning(this, QStringLiteral("입력 오류"),
                                 QStringLiteral("카메라 채널은 1~4 사이로 입력해주세요."));
            return;
        }
    }

    // 빈 칸은 NULL로 저장 (wearable_id는 문자열)
    // 화면 채널(1~4) → DB 채널(0~3). 빈 칸은 NULL.
    auto channelOrNull = [](const QString& s) -> QVariant {
        const QString t = s.trimmed();
        return t.isEmpty() ? QVariant() : QVariant(t.toInt() - 1);
    };
    auto textOrNull = [](const QString& s) -> QVariant {
        const QString t = s.trimmed();
        return t.isEmpty() ? QVariant() : QVariant(t);
    };

    const bool isNew = (selectedResidentId < 0);

    // 변경 전 값은 UPDATE 실행 전에 읽어둬야 한다 (실행 후엔 이미 덮어써짐).
    const QMap<QString, QString> before =
        isNew ? QMap<QString, QString>() : snapshotResident(selectedResidentId);

    QSqlQuery q;
    if (isNew) {
        // caregiver_id는 요양사 테이블 연동 전이라 제외(기본 NULL)
        q.prepare(QStringLiteral(
            "INSERT INTO residents "
            "(name, room, bed, camera_id, wearable_id, risk_level, admitted_at, "
            " discharge_due, status, notes) "
            "VALUES (?,?,?,?,?,?,?,?,?,?)"));
    } else {
        q.prepare(QStringLiteral(
            "UPDATE residents SET name=?, room=?, bed=?, camera_id=?, "
            " wearable_id=?, risk_level=?, admitted_at=?, discharge_due=?, "
            " status=?, notes=? WHERE resident_id=?"));
    }

    q.addBindValue(editName->text().trimmed());
    // room/bed 컬럼은 NOT NULL이라 빈 칸도 빈 문자열로 채운다(침대는 UI에서 제거).
    // 주의: QString()은 SQL NULL로 들어가 NOT NULL 위반 → 반드시 non-null 빈 문자열.
    q.addBindValue(editRoom->text().trimmed());
    q.addBindValue(QStringLiteral(""));
    q.addBindValue(channelOrNull(editCameraId->text()));
    q.addBindValue(textOrNull(editWearableId->text()));
    q.addBindValue(editRiskLevel->currentText());
    q.addBindValue(editAdmittedAt->date());
    q.addBindValue(editDischargeDue->date());
    q.addBindValue(editStatus->currentText());
    q.addBindValue(editNotes->toPlainText());
    if (!isNew)
        q.addBindValue(selectedResidentId);

    if (!q.exec()) {
        QMessageBox::critical(this, QStringLiteral("저장 실패"), q.lastError().text());
        qDebug() << "입소자 저장 실패:" << q.lastError().text();
        return;
    }

    if (isNew) {
        selectedResidentId = q.lastInsertId().toInt();
        // 신규 등록이면 첫 입원 에피소드를 만든다.
        QSqlQuery a;
        a.prepare(QStringLiteral(
            "INSERT INTO admissions "
            "(resident_id, admitted_at, discharge_due, status, room, bed) "
            "VALUES (?,?,?,'재원','','')"));
        a.addBindValue(selectedResidentId);
        a.addBindValue(editAdmittedAt->date());
        a.addBindValue(editDischargeDue->date());
        if (!a.exec()) qDebug() << "입원 에피소드 생성 실패:" << a.lastError().text();
    }

    const int admId = currentAdmissionId(selectedResidentId);
    logChanges(selectedResidentId, admId, before, formSnapshot(),
               isNew ? QStringLiteral("등록") : QStringLiteral("수정"));




    // 화면 채널(1~4) → 내부 채널(0~3). 빈 칸이면 -1이 되어 전송 조건에서 걸러진다.
    // camText는 함수 앞부분(입력 검증)에서 이미 editCameraId 값으로 잡아둠.
    int cameraId = camText.isEmpty() ? -1 : camText.toInt() - 1;

    // 4채널 중 올바른 채널이고, 서버 소켓이 정상 연결된 상태일 때만 전송
    if (cameraId >= 0 && cameraId < 4 && stream_->isChannelConnected(cameraId))
    {
        // 1. 위험도 텍스트를 서버가 이해하는 숫자 코드로 매칭 (하:1, 중:2, 상:3)
        int32_t statusVal = 1; 
        QString risk = editRiskLevel->currentText();
        if (risk == QStringLiteral("상")) statusVal = 3;
        else if (risk == QStringLiteral("중")) statusVal = 2;
        else if (risk == QStringLiteral("하")) statusVal = 1;

        // 2. 대상 침대 결정.
        //    위험도는 사람에게 붙는 값이라 채널이 아니라 침대 단위로 보낸다 —
        //    한 채널에 '상'과 '하'가 같이 누워 있으면 채널 일괄 적용은 둘 중
        //    하나를 반드시 틀리게 만든다. 이 입소자에게 배정된 침대들만 골라
        //    보내고, 아직 침대가 매핑돼 있지 않으면 예전처럼 채널 일괄로 보낸다.
        QVector<int> targetBeds;
        for (auto it = roiResident_[cameraId].constBegin();
             it != roiResident_[cameraId].constEnd(); ++it) {
            if (it.value() == selectedResidentId && selectedResidentId > 0)
                targetBeds.push_back(it.key());
        }
        if (targetBeds.isEmpty()) targetBeds.push_back(kRoiIdAll);

        for (int bed : targetBeds) {
            // 3. 침대 단위 위험도 제어 메시지 (조립·로그는 StreamClient)
            stream_->sendRiskLevel(cameraId, bed, statusVal);
        }
    }

    refreshResidentCards(residentSearchEdit ? residentSearchEdit->text() : QString());
    refreshAdmissionTable(selectedResidentId);
    refreshResidentDialogHeader();   // 편집 다이얼로그 상단 프로필도 갱신
    // 채널↔환자 매핑을 DB 기준으로 다시 읽어 영상/바이탈 이름을 갱신한다.
    // (채널 재배정·이름 변경·camera_id 해제 등 모든 경우를 한 번에 반영)
    loadPatientsFromDb();
    refreshPatientLabels();
    QMessageBox::information(this, QStringLiteral("저장"),
                             isNew ? QStringLiteral("신규 입소자가 등록되었습니다.")
                                   : QStringLiteral("수정 내용이 저장되었습니다."));
}

void MainWindow::onDischargeResident()
{
    if (selectedResidentId < 0) {
        QMessageBox::warning(this, QStringLiteral("선택 없음"),
                             QStringLiteral("퇴원 처리할 입소자를 먼저 선택해주세요."));
        return;
    }
    const auto reply = QMessageBox::question(
        this, QStringLiteral("퇴원 처리"),
        QStringLiteral("선택한 입소자를 퇴원 처리하시겠습니까?\n(기록은 보존됩니다)"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    QSqlQuery q;
    q.prepare(QStringLiteral("UPDATE residents SET status='퇴원' WHERE resident_id=?"));
    q.addBindValue(selectedResidentId);
    if (!q.exec()) {
        QMessageBox::critical(this, QStringLiteral("퇴원 처리 실패"), q.lastError().text());
        qDebug() << "퇴원 처리 실패:" << q.lastError().text();
        return;
    }

    // 에피소드도 닫는다 — 이 날짜가 이력 테이블의 "퇴원일"로 표시된다.
    const int admId = currentAdmissionId(selectedResidentId);
    QSqlQuery a;
    a.prepare(QStringLiteral("UPDATE admissions SET discharged_at=CURDATE(), "
                             "status='퇴원' WHERE admission_id=?"));
    a.addBindValue(admId);
    a.exec();

    logChanges(selectedResidentId, admId,
               {{QStringLiteral("상태"), QStringLiteral("재원")}},
               {{QStringLiteral("상태"), QStringLiteral("퇴원")}},
               QStringLiteral("퇴원"));
    refreshAdmissionTable(selectedResidentId);

    // 폼의 상태 콤보도 '퇴원'으로 반영
    const int i = editStatus->findText(QStringLiteral("퇴원"));
    if (i >= 0) editStatus->setCurrentIndex(i);

    refreshResidentCards(residentSearchEdit ? residentSearchEdit->text() : QString());
    refreshResidentDialogHeader();
    loadPatientsFromDb();    // 퇴원 → 그 채널을 "미배정"으로 되돌린다
    refreshPatientLabels();
    QMessageBox::information(this, QStringLiteral("퇴원 처리"),
                             QStringLiteral("퇴원 처리되었습니다."));
    qDebug() << "퇴원 처리 완료 — ID:" << selectedResidentId;
}

void MainWindow::onReadmitResident()
{
    if (selectedResidentId < 0) {
        QMessageBox::warning(this, QStringLiteral("선택 없음"),
                             QStringLiteral("재입원 처리할 입소자를 먼저 선택해주세요."));
        return;
    }
    if (QMessageBox::question(
            this, QStringLiteral("재입원 처리"),
            QStringLiteral("새 입원 기록을 만들까요?\n(이전 입원 이력은 그대로 보존됩니다)"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    QSqlQuery a;
    a.prepare(QStringLiteral(
        "INSERT INTO admissions (resident_id, admitted_at, status, room, bed) "
        "VALUES (?, CURDATE(), '재원', '', '')"));
    a.addBindValue(selectedResidentId);
    if (!a.exec()) {
        QMessageBox::critical(this, QStringLiteral("재입원 실패"), a.lastError().text());
        return;
    }

    QSqlQuery r;
    r.prepare(QStringLiteral("UPDATE residents SET status='재원', admitted_at=CURDATE() "
                             "WHERE resident_id=?"));
    r.addBindValue(selectedResidentId);
    r.exec();

    logChanges(selectedResidentId, currentAdmissionId(selectedResidentId),
               {{QStringLiteral("상태"), QStringLiteral("퇴원")}},
               {{QStringLiteral("상태"), QStringLiteral("재원")}},
               QStringLiteral("재입원"));

    const int i = editStatus->findText(QStringLiteral("재원"));
    if (i >= 0) editStatus->setCurrentIndex(i);
    editAdmittedAt->setDate(QDate::currentDate());
    refreshResidentCards(residentSearchEdit ? residentSearchEdit->text() : QString());
    refreshAdmissionTable(selectedResidentId);
    refreshResidentDialogHeader();
    loadPatientsFromDb();    // 재입원 → 그 채널에 이름 복원
    refreshPatientLabels();
}
