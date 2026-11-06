// MainWindow — 관제 화면 — 리소스 패널, 타임라인·재생바, 영상 표시, 낙상·이탈·생체 경보, 인터콤, 경보 해제
// (mainwindow.cpp를 기능별로 나눈 파일 중 하나. 공용 헬퍼는 mainwindow_p.h)
#include "mainwindow_p.h"

// ═════════════════════════════════════════════════════════
//  좌측 리소스 패널 — Wisenet Viewer의 Resource 패널에 대응.
//  네비 레일이 "어느 화면을 볼까"를 고른다면, 이 패널은 관제 화면 안에서
//  "어느 카메라를 다룰까"를 고른다 — 둘을 합치면 카메라가 늘어날 때
//  페이지 전환과 카메라 선택이 같은 줄에 섞여 길어진다.
// ═════════════════════════════════════════════════════════
QWidget* MainWindow::buildResourcePanel()
{
    resourcePanel_ = new QFrame();
    resourcePanel_->setObjectName("resourcePanel");

    auto* outer = new QVBoxLayout(resourcePanel_);
    outer->setContentsMargins(10, 12, 10, 12);
    outer->setSpacing(8);

    // 헤더 — "리소스" + 접기 토글
    auto* head = new QHBoxLayout();
    head->setContentsMargins(0, 0, 0, 0);
    head->setSpacing(6);
    resourceHead_ = new QLabel(QStringLiteral("리소스"));
    resourceHead_->setObjectName("resourceHead");
    head->addWidget(resourceHead_, 1);
    resourceToggle_ = new QPushButton(QStringLiteral("‹"));
    resourceToggle_->setObjectName("resourceToggle");
    resourceToggle_->setCursor(Qt::PointingHandCursor);
    resourceToggle_->setFixedSize(22, 22);
    resourceToggle_->setToolTip(QStringLiteral("리소스 패널 접기/펼치기"));
    connect(resourceToggle_, &QPushButton::clicked, this,
            [this] { setResourceCollapsed(!resourceCollapsed_); });
    head->addWidget(resourceToggle_);
    outer->addLayout(head);

    // 접었을 때 통째로 숨길 부분(검색 + 트리). 헤더만 남아 다시 펼 수 있다.
    resourceBody_ = new QWidget();
    auto* body = new QVBoxLayout(resourceBody_);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(8);

    resourceSearch_ = new QLineEdit();
    resourceSearch_->setObjectName("resourceSearch");
    resourceSearch_->setPlaceholderText(QStringLiteral("카메라 · 입소자 검색"));
    resourceSearch_->setClearButtonEnabled(true);
    connect(resourceSearch_, &QLineEdit::textChanged, this,
            [this](const QString&) { refreshResourceTree(); });
    body->addWidget(resourceSearch_);

    resourceTree_ = new QTreeWidget();
    resourceTree_->setObjectName("resourceTree");
    resourceTree_->setHeaderHidden(true);
    resourceTree_->setIndentation(14);
    resourceTree_->setRootIsDecorated(true);
    resourceTree_->setUniformRowHeights(true);
    resourceTree_->setFrameShape(QFrame::NoFrame);
    resourceTree_->setIconSize(QSize(16, 16));
    resourceTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    body->addWidget(resourceTree_, 1);

    auto* root = new QTreeWidgetItem(resourceTree_,
                                     QStringList(QStringLiteral("Root")));
    root->setData(0, Qt::UserRole, QStringLiteral("root"));
    rebuildResourceRooms();

    // ── 레이아웃 프리셋 ──
    auto* layoutRoot = new QTreeWidgetItem(resourceTree_,
                                           QStringList(QStringLiteral("레이아웃")));
    layoutRoot->setData(0, Qt::UserRole, QStringLiteral("layoutRoot"));
    const QString layoutNames[3] = {QStringLiteral("2×2 전체"),
                                    QStringLiteral("스포트라이트"),
                                    QStringLiteral("단일 채널")};
    for (int i = 0; i < 3; ++i) {
        auto* it = new QTreeWidgetItem(layoutRoot, QStringList(layoutNames[i]));
        it->setData(0, Qt::UserRole, QStringLiteral("layout"));
        it->setData(0, Qt::UserRole + 1, i);
        it->setIcon(0, QIcon(navIconPixmap(0, QColor(QString::fromLatin1(kTextSub)), 16)));
    }
    layoutRoot->setExpanded(true);

    connect(resourceTree_, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem* item, int) {
                if (!item) return;
                const QString kind = item->data(0, Qt::UserRole).toString();
                // 방 그룹을 누르면 영상월이 그 방으로 바뀐다. 빈 방의 채널 자리를
                // 눌러도 같은 뜻으로 받는다 — 사람은 그룹이 아니라 눈에 보이는
                // 채널 줄을 누른다.
                if (kind == QLatin1String("group") || kind == QLatin1String("groupEmpty")) {
                    selectRoom(item->data(0, Qt::UserRole + 1).toInt());
                    return;
                }
                if (kind == QLatin1String("camEmpty")) {
                    QTreeWidgetItem* g = item->parent();
                    if (g) selectRoom(g->data(0, Qt::UserRole + 1).toInt());
                    return;
                }
                if (kind == QLatin1String("cam")) {
                    selectRoom(0);   // 실카메라 방으로 돌아온 뒤 그 채널을 고른다
                    selectChannel(item->data(0, Qt::UserRole + 1).toInt());
                } else if (kind == QLatin1String("layout")) {
                    const int i = item->data(0, Qt::UserRole + 1).toInt();
                    setGridLayout(i == 0   ? GridLayout::Quad
                                  : i == 1 ? GridLayout::Spotlight
                                           : GridLayout::Single);
                }
            });
    // 더블클릭 = 그 카메라만 크게 — Wisenet에서 트리 항목을 더블클릭한 것과 같은 감각.
    connect(resourceTree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) {
                if (!item || item->data(0, Qt::UserRole).toString() != QLatin1String("cam"))
                    return;
                selectRoom(0);
                selectChannel(item->data(0, Qt::UserRole + 1).toInt());
                setGridLayout(GridLayout::Single);
            });

    outer->addWidget(resourceBody_, 1);

    // 접었을 때 보이는 세로 칩 레일 — 채널 번호 + 연결 상태색. 누르면 그 채널 선택.
    resourceRail_ = new QWidget();
    auto* rv = new QVBoxLayout(resourceRail_);
    rv->setContentsMargins(0, 6, 0, 0);
    rv->setSpacing(4);
    for (int ch = 0; ch < 4; ++ch) {
        auto* chip = new QPushButton(QString::number(ch + 1));
        chip->setObjectName("resChip");
        chip->setCheckable(true);
        chip->setAutoExclusive(true);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setFixedSize(30, 26);
        connect(chip, &QPushButton::clicked, this, [this, ch] { selectChannel(ch); });
        resChipBtns_[ch] = chip;
        rv->addWidget(chip, 0, Qt::AlignHCenter);
    }
    rv->addStretch();
    resourceRail_->hide();
    outer->addWidget(resourceRail_, 1);

    QSettings st;
    // 기본은 접힘 — 좌측에 네비 레일과 이 패널이 나란히 펼쳐지면 크롬이
    // 화면 폭의 절반 가까이를 먹는다. 필요할 때만 펴서 쓴다.
    //
    // 키 이름을 _v2로 바꾼 이유: setResourceCollapsed()가 값을 되쓰기 때문에
    // 예전 키(ui/resource_collapsed)는 이미 모든 기존 사용자에게 false로 남아 있다.
    // 같은 키를 그대로 쓰면 "기본 접힘"이 아무에게도 적용되지 않는다. 새 키로 한 번
    // 초기화하고, 이후 사용자가 편 선택은 그대로 기억한다.
    setResourceCollapsed(st.value(QStringLiteral("ui/resource_collapsed_v2"), true).toBool());
    refreshResourceTree();
    return resourcePanel_;
}

// 트리의 표시만 다시 만든다(항목 자체는 재생성하지 않는다).
// 다시 만들면 펼침 상태와 스크롤 위치가 매번 초기화돼 손으로 다시 열어야 한다.
// Root 아래 방 그룹을 처음부터 다시 만든다. 방을 추가·삭제하면 항목 자체가
// 달라지므로 refreshResourceTree(표시 갱신)로는 부족하고 이 함수가 필요하다.
void MainWindow::rebuildResourceRooms()
{
    if (!resourceTree_) return;
    QTreeWidgetItem* root = resourceTree_->topLevelItem(0);
    if (!root) return;
    qDeleteAll(root->takeChildren());
    for (int ch = 0; ch < 4; ++ch) camItems_[ch] = nullptr;

    // ── Root > 방 그룹 > 채널 4개 ──
    // 0번 방만 실제 카메라를 갖는다. 나머지 방은 채널 4칸을 회색으로 비워 두는데,
    // 눌러도 아무 일도 일어나지 않는다(연결된 영상이 없으니 고를 것도 없다).
    const QStringList rooms = roomNames();
    for (int r = 0; r < rooms.size(); ++r) {
        auto* group = new QTreeWidgetItem(root);
        group->setData(0, Qt::UserRole,
                       r == 0 ? QStringLiteral("group") : QStringLiteral("groupEmpty"));
        group->setData(0, Qt::UserRole + 1, r);
        group->setText(0, rooms.at(r));
        if (r == 0) {
            for (int ch = 0; ch < 4; ++ch) {
                camItems_[ch] = new QTreeWidgetItem(group);
                camItems_[ch]->setData(0, Qt::UserRole, QStringLiteral("cam"));
                camItems_[ch]->setData(0, Qt::UserRole + 1, ch);
            }
        } else {
            group->setToolTip(0, QStringLiteral("카메라가 아직 연결되지 않은 방입니다"));
            for (int ch = 0; ch < 4; ++ch) {
                auto* slot = new QTreeWidgetItem(group);
                slot->setData(0, Qt::UserRole, QStringLiteral("camEmpty"));
                slot->setText(0, QStringLiteral("채널 %1").arg(ch + 1));
                slot->setToolTip(0, QStringLiteral("카메라 미연결 — 빈 자리입니다"));
            }
        }
        group->setExpanded(true);
    }
    root->setExpanded(true);
    root->setExpanded(true);
}

// 트리에서 방을 고르면 영상월을 그 방으로 전환한다.
//
// 방을 바꿔도 서버에는 아무 것도 보내지 않는다 — 101호 카메라는 계속 스트리밍하고
// 낙상 감지도 계속 돈다. 바뀌는 건 "이 화면이 무엇을 보여 주는가"뿐이다. 그래서
// 102호를 보다가 101호로 돌아오면 끊긴 자리에서 이어지는 게 아니라 지금 실시간이
// 바로 뜬다(카메라 해제와 결정적으로 다른 점).
void MainWindow::selectRoom(int room)
{
    const int count = roomNames().size();
    room = qBound(0, room, count - 1);
    if (room == selectedRoom_) return;

    // 실카메라 방을 떠나기 직전에 접속 폼을 보관한다 — applyRoomView가 비우기 때문에
    // 여기서 안 챙기면 102호에 한 번 들렀다 오는 것만으로 입력값이 날아간다.
    if (selectedRoom_ == 0 && room != 0) {
        camFormBackup_[0] = camIpEdit   ? camIpEdit->text()   : QString();
        camFormBackup_[1] = camUserEdit ? camUserEdit->text() : QString();
        camFormBackup_[2] = camPwEdit   ? camPwEdit->text()   : QString();
    }

    selectedRoom_ = room;
    applyRoomView();

    if (room == 0) {
        if (camIpEdit)   camIpEdit->setText(camFormBackup_[0]);
        if (camUserEdit) camUserEdit->setText(camFormBackup_[1]);
        if (camPwEdit)   camPwEdit->setText(camFormBackup_[2]);
        if (discoveryStatus)
            discoveryStatus->setText(QStringLiteral(
                "‘검색’을 누르면 같은 망의 카메라가 아래에 나타납니다. 행을 클릭하면 IP가 채워져요."));
        for (int ch = 0; ch < 4; ++ch) refreshRoiZones(ch);   // 침대 오버레이 복구
    }
}

// selectedRoom_에 맞춰 타일·바이탈·트리를 한 번에 맞춘다.
// videoSuppressed_는 이제 "해제했다"가 아니라 "지금 이 화면에 그리면 안 된다"는
// 파생값이다 — (보고 있는 방이 실카메라 방인가) × (그 채널을 해제하지 않았는가).
//
// 여기서 cameraActive_를 쓰지 않는 게 중요하다. 그 플래그는 "해제 버튼을 눌러도
// 되는가"를 나타낼 뿐이고, 서버는 Qt 재시작과 무관하게 계속 스트리밍한다 —
// active_mask가 비어 있어도 프레임은 들어오고 예전엔 그대로 화면에 떴다.
// 조건에 끼워 넣으면 그 경우에 영상이 통째로 사라진다.
void MainWindow::applyRoomView()
{
    const bool liveRoom = (selectedRoom_ == 0);
    for (int ch = 0; ch < 4; ++ch) {
        // draw  = 들어온 프레임을 그릴 것인가 (실제 차단은 이 값 하나로 한다)
        // known = 타일에 "연결됨"으로 적을 것인가. 둘을 나눈 이유: 서버가 이미
        //         스트리밍 중인데 active_mask만 비어 있는 경우가 있고, 그때도
        //         영상은 떠야 한다. 프레임이 도착하면 VideoView가 스스로
        //         cameraConnected_를 켜므로 라벨은 자연히 따라온다.
        const bool draw  = liveRoom && !videoCleared_[ch];
        const bool known = draw && cameraActive_[ch];
        videoSuppressed_[ch] = !draw;
        if (stream_) stream_->setChannelSuppressed(ch, !draw);   // 디코드 자체를 건너뛰게
        if (auto* v = channelViews[ch]) {
            // setCameraConnected(false)가 직전 프레임까지 지운다 — 방을 옮겼는데
            // 이전 방 화면이 정지영상으로 남아 있으면 그게 제일 위험한 오해다.
            v->setCameraConnected(known);
            if (!draw) v->setLive(false);
            // 침대 오버레이는 101호의 것이다 — 빈 방 타일에 남으면 그 방에
            // 침대가 있는 것처럼 보인다. 돌아올 때는 selectRoom()이 되살린다.
            if (!liveRoom) v->setZones({});
            v->setDisplayName(tileDisplayName(ch));
            // 배지는 "빈 방으로 나갈 때"만 지운다. 돌아올 때도 지우면 경보 경로가
            // setAlert(true) → setVideoFocus → selectRoom(0) 순서라서 방금 켠
            // 배지를 스스로 꺼 버린다.
            if (!liveRoom) v->setAlert(false);
        }
        if (!draw) {
            lastFramePix_[ch] = QPixmap();
            if (camThumbs[ch]) camThumbs[ch]->clearFrame();
        }
    }
    // 설정 화면(스테이지 영상)도 같은 방을 따른다.
    if (roiEditorView) {
        const bool stage = liveRoom && cameraActive_[roiEditChannel];
        roiEditorView->setCameraConnected(stage);
        if (!liveRoom) { roiEditorView->setLive(false); roiEditorView->setZones({}); }
    }
    if (imgWipe_ && !liveRoom) imgWipe_->clearFrames();

    // 인스펙터는 101호와 같은 화면을 그대로 보여주되 만질 수 없게 한다.
    // 값까지 남겨두면 102호 화면에 101호 IP가 회색으로 읽혀 더 헷갈리므로 비운다
    // (돌아올 때 camFormBackup_에서 되돌린다).
    if (camControlStack) camControlStack->setEnabled(liveRoom);
    if (!liveRoom) {
        if (camIpEdit)   camIpEdit->clear();
        if (camUserEdit) camUserEdit->clear();
        if (camPwEdit)   camPwEdit->clear();
        if (discoveryTable) { discoveryTable->setRowCount(0); syncDiscoveryTableHeight(); }
        if (discoveryStatus)
            discoveryStatus->setText(QStringLiteral("이 방에는 아직 카메라가 배정되지 않았습니다."));
    }
    rebuildBedList();
    for (int r = 0; r < camRoomBtns_.size(); ++r)
        camRoomBtns_[r]->setChecked(r == selectedRoom_);
    setCamMode(camMode_);          // 빈 방이면 안내 페이지로, 아니면 원래 모드로
    refreshCamChannelStatus();

    rebuildVitalCards();     // 빈 방에서는 카드가 전부 "대기"로 내려간다
    refreshResourceTree();
}

// 타일 좌상단 이름. 빈 방에서는 입소자 이름을 붙이지 않는다 — 102호 타일에
// 101호 입소자 이름이 뜨면 그 사람이 거기 있는 것으로 읽힌다.
QString MainWindow::tileDisplayName(int ch) const
{
    if (selectedRoom_ != 0) return QStringLiteral("CH%1").arg(ch + 1);
    return channelDisplayName(ch);
}

void MainWindow::refreshResourceTree()
{
    if (!resourceTree_) return;

    const QString filter = resourceSearch_ ? resourceSearch_->text().trimmed() : QString();
    const QColor liveColor(QString::fromLatin1(kNormal));
    const QColor offColor(QString::fromLatin1(kTextSub));
    const QColor textColor(QString::fromLatin1(kTextMain));
    const QColor selColor(QString::fromLatin1(kSelect));

    // 방 그룹 갱신. 0번 방만 실제 카메라를 갖고, 나머지는 빈 자리라 회색으로 낮춘다.
    const QStringList rooms = roomNames();
    if (camItems_[0] && camItems_[0]->parent()) {
        QTreeWidgetItem* live = camItems_[0]->parent();
        live->setText(0, rooms.value(0, currentRoomName()));
        // 색을 명시해 둔다 — 기본색에 맡기면 아래 빈 방(회색)과 톤이 비슷해져
        // "카메라가 붙은 방"과 "빈 자리"가 구분되지 않는다.
        live->setForeground(0, selectedRoom_ == 0 ? selColor : textColor);
        QFont lf = live->font(0);
        lf.setBold(selectedRoom_ == 0);
        live->setFont(0, lf);
    }

    if (auto* root = resourceTree_->topLevelItem(0)) {
        for (int i = 0; i < root->childCount(); ++i) {
            QTreeWidgetItem* group = root->child(i);
            if (group->data(0, Qt::UserRole).toString() != QLatin1String("groupEmpty"))
                continue;
            const int r = group->data(0, Qt::UserRole + 1).toInt();
            const bool viewing = (r == selectedRoom_);
            group->setForeground(0, viewing ? selColor : offColor);
            QFont gf = group->font(0);
            gf.setBold(viewing);
            group->setFont(0, gf);
            for (int c = 0; c < group->childCount(); ++c) {
                QTreeWidgetItem* slot = group->child(c);
                slot->setForeground(0, offColor);
                slot->setIcon(0, QIcon(navIconPixmap(5, offColor, 16)));
            }
            // 검색 중이면 이름이 걸리는 빈 방만 남긴다(트리와 같은 규칙).
            group->setHidden(!filter.isEmpty() &&
                             !group->text(0).contains(filter, Qt::CaseInsensitive));
        }
    }

    for (int ch = 0; ch < 4; ++ch) {
        auto* it = camItems_[ch];
        if (!it) continue;

        const bool live = channelViews[ch] && channelViews[ch]->live();
        it->setText(0, channelDisplayName(ch));
        it->setIcon(0, QIcon(navIconPixmap(5, live ? liveColor : offColor, 16)));
        it->setForeground(0, tileHidden_[ch] ? offColor
                                             : (ch == selectedChannel_ ? selColor : textColor));
        QFont f = it->font(0);
        f.setBold(ch == selectedChannel_);
        f.setStrikeOut(tileHidden_[ch]);   // 레이아웃에서 물러난 타일
        it->setFont(0, f);
        it->setToolTip(0, live ? QStringLiteral("실시간 수신 중")
                               : QStringLiteral("영상 신호 없음"));

        // 검색어가 있으면 이름에 안 들어있는 카메라는 감춘다.
        it->setHidden(!filter.isEmpty() &&
                      !channelDisplayName(ch).contains(filter, Qt::CaseInsensitive));

        // 접힘 상태의 칩도 같은 값으로 갱신한다(트리와 어긋나면 안 된다).
        if (auto* chip = resChipBtns_[ch]) {
            chip->setChecked(ch == selectedChannel_);
            chip->setProperty("state", tileHidden_[ch] ? QStringLiteral("hidden")
                                       : live         ? QStringLiteral("live")
                                                      : QStringLiteral("off"));
            chip->setToolTip(QStringLiteral("%1 · %2")
                                 .arg(channelDisplayName(ch),
                                      live ? QStringLiteral("수신 중")
                                           : QStringLiteral("신호 없음")));
            chip->style()->unpolish(chip);
            chip->style()->polish(chip);
        }
    }
}

void MainWindow::setResourceCollapsed(bool on)
{
    resourceCollapsed_ = on;
    if (resourceBody_) resourceBody_->setVisible(!on);
    if (resourceRail_) resourceRail_->setVisible(on);
    // 제목까지 남겨 두면 "리소스" 세 글자가 폭을 못 줄여 토글 버튼이 밖으로
    // 밀려 나간다 — 접힌 패널에 화살표조차 없어 다시 펼 수가 없었다.
    if (resourceHead_) resourceHead_->setVisible(!on);

    if (resourcePanel_) {
        resourcePanel_->setFixedWidth(on ? 48 : 208);
        if (auto* l = qobject_cast<QVBoxLayout*>(resourcePanel_->layout()))
            l->setContentsMargins(on ? 9 : 10, 12, on ? 9 : 10, 12);
    }
    if (resourceToggle_) {
        // 화살표는 "누르면 갈 방향"을 가리킨다: 접힘→오른쪽으로 펼침(›),
        // 펼침→왼쪽으로 접힘(‹).
        resourceToggle_->setText(on ? QStringLiteral("›") : QStringLiteral("‹"));
        resourceToggle_->setToolTip(on ? QStringLiteral("리소스 목록 펼치기")
                                       : QStringLiteral("리소스 목록 접기"));
    }
    // 접히면 토글만 남으므로 가운데로, 펼치면 제목 오른쪽 끝으로.
    if (resourceToggle_ && resourceToggle_->parentWidget()) {
        if (auto* lay = resourceToggle_->parentWidget()->layout())
            lay->setAlignment(resourceToggle_, on ? Qt::AlignHCenter : Qt::AlignRight);
    }

    QSettings st;
    st.setValue(QStringLiteral("ui/resource_collapsed_v2"), on);
    refreshResourceTree();   // 칩 상태도 같은 경로에서 갱신
}

// 타일 오버레이·트리가 공유하는 채널 이름. "CH1 · 김복순" 형식.
QString MainWindow::channelDisplayName(int ch) const
{
    const QString tag = QStringLiteral("CH%1").arg(ch + 1);
    if (ch < 0 || ch >= 4) return tag;

    const QVector<int>& ids = residentsByChannel_[ch];
    if (ids.isEmpty()) return tag;

    // 한 채널(=한 방의 한 시야)에 여러 명이 함께 누워 있는 게 이 시스템의 기본
    // 구성이다. 그런데 대표 한 명만 쓰고 "외 N명"으로 접으면, 정작 그 방에 누가
    // 있는지를 화면에서 알 수 없다 — 두 명까지는 다 적는다.
    // 세 명 이상은 한 줄짜리 이름표가 감당이 안 되므로 그때만 접는다.
    QStringList names;
    for (int rid : ids) {
        const QString n = residentInfo_.value(rid).name;
        if (!n.isEmpty()) names << n;
    }
    if (names.isEmpty()) return tag;

    if (names.size() <= 2)
        return QStringLiteral("%1 · %2").arg(tag, names.join(QStringLiteral(", ")));
    return QStringLiteral("%1 · %2 외 %3명")
        .arg(tag, names.mid(0, 2).join(QStringLiteral(", ")))
        .arg(names.size() - 2);
}

// ═════════════════════════════════════════════════════════
//  하단 트랜스포트 + 타임라인 — Wisenet Viewer 하단 바에 대응.
//  라이브/재생 전환은 영상 영역을 통째로 갈아끼운다(liveOrPlaybackStack_).
// ═════════════════════════════════════════════════════════
QWidget* MainWindow::buildTransportBar()
{
    auto* wrap = new QWidget();
    auto* v = new QVBoxLayout(wrap);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    // ── 컴트롤 줄 ──
    auto* bar = new QFrame();
    bar->setObjectName("transportBar");
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(10, 6, 10, 6);
    h->setSpacing(6);

    // 라이브 / 재생 토글 — Wisenet은 오른쪽 끝에 둔다.
    liveModeBtn_ = new QPushButton(QStringLiteral("라이브"));
    liveModeBtn_->setObjectName("modeBtn");
    liveModeBtn_->setCheckable(true);
    liveModeBtn_->setChecked(true);
    liveModeBtn_->setAutoExclusive(true);
    liveModeBtn_->setCursor(Qt::PointingHandCursor);
    connect(liveModeBtn_, &QPushButton::clicked, this, [this] { setPlaybackMode(false); });

    playbackModeBtn_ = new QPushButton(QStringLiteral("녹화 재생"));
    playbackModeBtn_->setObjectName("modeBtn");
    playbackModeBtn_->setCheckable(true);
    playbackModeBtn_->setAutoExclusive(true);
    playbackModeBtn_->setCursor(Qt::PointingHandCursor);
    connect(playbackModeBtn_, &QPushButton::clicked, this, [this] { setPlaybackMode(true); });

    // 트랜스포트 — 재생 모드에서만 의미가 있다.
    struct TSpec { const char16_t* glyph; const char16_t* tip; int deltaMs; };
    const TSpec specs[5] = {
        {u"⏮", u"10분 이전으로", -600000},
        {u"⏪", u"10초 이전으로", -10000},
        {u"▶", u"재생 / 일시정지", 0},
        {u"⏩", u"10초 이후로", 10000},
        {u"⏭", u"10분 이후로", 600000},
    };
    for (int i = 0; i < 5; ++i) {
        auto* b = new QPushButton(QString::fromUtf16(specs[i].glyph));
        b->setObjectName("transportBtn");
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedSize(30, 26);
        if (i == 2) {
            transportPlayBtn_ = b;
            b->setToolTip(QStringLiteral("재생 / 일시정지"));
            connect(b, &QPushButton::clicked, this, [this] {
                if (!playbackMode_ || !playbackPlayer_) return;
                if (playbackPlayer_->playbackState() == QMediaPlayer::PlayingState)
                    playbackPlayer_->pause();
                else
                    playbackPlayer_->play();
            });
        } else {
            b->setToolTip(QString::fromUtf16(specs[i].tip));
            connect(b, &QPushButton::clicked, this, [this, d = specs[i].deltaMs] {
                if (!playbackMode_ || !timeline_) return;
                seekPlaybackTo(timeline_->playhead() + d);
            });
        }
        h->addWidget(b);
    }

    transportTimeLabel_ = new QLabel();
    transportTimeLabel_->setObjectName("transportTime");
    h->addWidget(transportTimeLabel_);

    h->addStretch();

    transportSpeedCombo_ = new QComboBox();
    transportSpeedCombo_->setObjectName("transportSpeed");
    const double rates[5] = {0.5, 1.0, 2.0, 4.0, 8.0};
    for (double r : rates)
        transportSpeedCombo_->addItem(QStringLiteral("x%1").arg(r), r);
    transportSpeedCombo_->setCurrentIndex(1);
    connect(transportSpeedCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (playbackPlayer_ && transportSpeedCombo_)
                    playbackPlayer_->setPlaybackRate(
                        transportSpeedCombo_->currentData().toDouble());
            });
    h->addWidget(transportSpeedCombo_);

    h->addWidget(liveModeBtn_);
    h->addWidget(playbackModeBtn_);
    v->addWidget(bar);

    // ── 타임라인 ──
    timeline_ = new TimelineBar();
    timeline_->setObjectName("timeline");
    connect(timeline_, &TimelineBar::seekRequested, this, [this](qint64 ms) {
        if (transportTimeLabel_)
            transportTimeLabel_->setText(
                QDateTime::fromMSecsSinceEpoch(ms).toString("yyyy-MM-dd HH:mm:ss"));
    });
    // 실제 탐색은 드래그가 끝난 뒤에만 한다 — 끌고 가는 동안 매 프레임
    // 새 세그먼트를 여는 건 네트워크와 디코더에 과부하다.
    connect(timeline_, &TimelineBar::seekCommitted, this, &MainWindow::seekPlaybackTo);
    v->addWidget(timeline_);

    // 라이브일 때 타임라인 창을 "지금"에 맞춰 계속 민다.
    connect(&timelineTimer_, &QTimer::timeout, this, [this] {
        if (!timeline_) return;
        if (!playbackMode_) {
            timeline_->followNow();
            if (transportTimeLabel_)
                transportTimeLabel_->setText(
                    QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
        }
    });
    timelineTimer_.start(1000);

    setPlaybackMode(false);
    // 세그먼트 목록을 한 번 받아 둔다. 예전엔 이벤트 기록 페이지의 NVR 카드가
    // 이 일을 겸했는데, 그 카드를 없앴으니 타임라인이 직접 챙긴다.
    refreshNvrSegments();
    return wrap;
}

// NVR 세그먼트 목록과 이벤트 이력을 타임라인이 이해하는 모양으로 넘긴다.
// 선택 채널만 보여준다 — 4개 채널을 한 줄에 곹쳐 그리면 어느 게 어느 채널인지 모른다.
void MainWindow::refreshTimeline()
{
    if (!timeline_) return;

    // 세그먼트 1개 길이는 서버 설정(nvr_segment_minutes) 기본값과 같다.
    // 목록은 시작 시각만 주므로 끝은 이 길이로 추정하고, 연속된 것끼리 합친다.
    constexpr qint64 kSegLenMs = 10 * 60 * 1000;
    constexpr qint64 kJoinGapMs = 30 * 1000;   // 이보다 밀착하면 한 구간으로 본다

    QVector<qint64> starts;
    for (const auto& seg : nvrSegments_)
        if (seg.channel == selectedChannel_) starts.push_back(seg.startMs);
    std::sort(starts.begin(), starts.end());

    QVector<TimelineBar::Span> spans;
    for (qint64 st : starts) {
        if (!spans.isEmpty() && st - spans.back().endMs <= kJoinGapMs)
            spans.back().endMs = st + kSegLenMs;
        else
            spans.push_back({st, st + kSegLenMs});
    }
    timeline_->setSpans(spans);

    QVector<TimelineBar::Marker> markers;
    for (const auto& e : timelineEvents_)
        if (e.channel == selectedChannel_) markers.push_back({e.atMs, e.color});
    timeline_->setMarkers(markers);
}

// 라이브 ↔ 녹화 재생 전환. 영상 영역을 통째로 바꿔 두 모드가 섞이지 않게 한다.
void MainWindow::setPlaybackMode(bool on)
{
    playbackMode_ = on;
    if (liveModeBtn_) liveModeBtn_->setChecked(!on);
    if (playbackModeBtn_) playbackModeBtn_->setChecked(on);
    if (timeline_) timeline_->setLiveMode(!on);
    if (liveOrPlaybackStack_) {
        // 재생 모드로 막 넘어왔을 땐 아직 볼 시각을 고르지 않았다 → 안내 화면(2).
        liveOrPlaybackStack_->setCurrentIndex(on ? 2 : 0);
    }
    if (on && playbackPlaceholder_) {
        playbackPlaceholder_->setText(QStringLiteral(
            "아래 타임라인에서 볼 시각을 클릭하세요.\n"
            "초록 구간이 %1 녹화가 남아 있는 시간대입니다.")
                .arg(channelDisplayName(selectedChannel_)));
    }

    if (!on) {
        if (playbackPlayer_) playbackPlayer_->stop();
        if (timeline_) timeline_->followNow();
        return;
    }

    // 재생 모드로 들어오면 최신 녹화 목록을 한 번 받아온다.
    refreshNvrSegments();
}

// 그 시각을 담은 NVR 세그먼트를 찾아 그 안의 상대 위치로 재생한다.
void MainWindow::seekPlaybackTo(qint64 ms)
{
    if (!timeline_) return;
    timeline_->setPlayhead(ms);
    if (transportTimeLabel_)
        transportTimeLabel_->setText(
            QDateTime::fromMSecsSinceEpoch(ms).toString("yyyy-MM-dd HH:mm:ss"));

    if (!playbackMode_) {
        // 라이브 중에 타임라인을 찍으면 재생 모드로 넘어가는 게 자연스럽다.
        setPlaybackMode(true);
    }

    constexpr qint64 kSegLenMs = 10 * 60 * 1000;
    const NvrSegmentInfo* best = nullptr;
    for (const auto& seg : nvrSegments_) {
        if (seg.channel != selectedChannel_) continue;
        if (ms < seg.startMs || ms >= seg.startMs + kSegLenMs) continue;
        if (!best || seg.startMs > best->startMs) best = &seg;
    }
    if (!best) {
        if (transportTimeLabel_)
            transportTimeLabel_->setText(
                QStringLiteral("%1 · 녹화 없음")
                    .arg(QDateTime::fromMSecsSinceEpoch(ms).toString("MM-dd HH:mm:ss")));
        if (playbackPlayer_) playbackPlayer_->stop();
        if (playbackPlaceholder_)
            playbackPlaceholder_->setText(
                QStringLiteral("%1 에는 %2 녹화가 없습니다.\n"
                               "타임라인의 초록 구간을 클릭하세요.")
                    .arg(QDateTime::fromMSecsSinceEpoch(ms).toString("MM-dd HH:mm:ss"),
                         channelDisplayName(selectedChannel_)));
        if (liveOrPlaybackStack_) liveOrPlaybackStack_->setCurrentIndex(2);
        return;
    }
    if (liveOrPlaybackStack_) liveOrPlaybackStack_->setCurrentIndex(1);

    if (!playbackPlayer_) {
        playbackPlayer_ = new QMediaPlayer(this);
        playbackPlayer_->setVideoOutput(playbackVideo_);
        // 재생이 흘러가면 플레이헤드도 같이 움직여야 타임라인이 거짓말을 안 한다.
        connect(playbackPlayer_, &QMediaPlayer::positionChanged, this,
                [this](qint64 pos) {
                    if (!playbackMode_ || !timeline_) return;
                    const qint64 at = playbackSegStartMs_ + pos;
                    timeline_->setPlayhead(at);
                    if (transportTimeLabel_)
                        transportTimeLabel_->setText(
                            QDateTime::fromMSecsSinceEpoch(at)
                                .toString("yyyy-MM-dd HH:mm:ss"));
                });
        // ▶ 버튼 글리프를 실제 재생 상태에 맞춰 토글(블랙박스 재생기의
        // playbackStateChanged 연결과 같은 패턴 — 예전엔 이 트랜스포트 바에만 빠져있었음).
        connect(playbackPlayer_, &QMediaPlayer::playbackStateChanged, this,
                [this](QMediaPlayer::PlaybackState st) {
                    if (transportPlayBtn_)
                        transportPlayBtn_->setText(st == QMediaPlayer::PlayingState
                                                        ? QStringLiteral("⏸")
                                                        : QStringLiteral("▶"));
                });
    }

    const QString url = best->url;
    const qint64 offset = ms - best->startMs;
    playbackSegStartMs_ = best->startMs;

    if (playbackPlayer_->source() != QUrl(url)) {
        playbackPlayer_->setSource(QUrl(url));
        // 길이를 알기 전에 setPosition을 불러도 무시된다 — 한 번만 적용하고 끊는다.
        auto* conn = new QMetaObject::Connection();
        *conn = connect(playbackPlayer_, &QMediaPlayer::durationChanged, this,
                        [this, offset, conn](qint64 dur) {
                            if (dur <= 0) return;
                            playbackPlayer_->setPosition(qBound(qint64(0), offset, dur));
                            disconnect(*conn);
                            delete conn;
                        });
    } else {
        playbackPlayer_->setPosition(offset);
    }
    playbackPlayer_->setPlaybackRate(
        transportSpeedCombo_ ? transportSpeedCombo_->currentData().toDouble() : 1.0);
    playbackPlayer_->play();
}

QWidget* MainWindow::buildVitalsPanel()
{
    auto* panel = new QFrame();
    panel->setObjectName("panel");
    // 바이탈 패널은 폭 고정 → 창을 키우면 남는 폭이 전부 영상 월로 간다.
    panel->setMinimumWidth(300);
    panel->setMaximumWidth(316);

    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(16, 14, 16, 16);
    outer->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("웨어러블 생체신호"));
    title->setObjectName("panelTitle");
    outer->addWidget(title);

    // 스크롤 가능한 바이탈 카드 목록
    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setObjectName("vitalScroll");

    auto* inner = new QWidget();
    vitalListLayout_ = new QVBoxLayout(inner);
    vitalListLayout_->setContentsMargins(0, 0, 6, 0);
    vitalListLayout_->setSpacing(10);
    // 카드 개수는 재원 입소자 수에 따라 달라진다(한 채널에 여러 명일 수 있음).
    // 생성자에서 loadPatientsFromDb() 를 먼저 부르므로 여기서 목록을 알 수 있다.
    rebuildVitalCards();

    scroll->setWidget(inner);
    outer->addWidget(scroll, 1);

    return panel;
}

// 바이탈 카드 목록을 입소자 구성에 맞춰 다시 만든다(D-04 — diff 방식).
// 구성이 안 바뀌면 위젯을 하나도 만들지도 지우지도 않는다. 심박 이력은
// hrHistory_(위젯 밖)에 있어서 타일이 새로 만들어져도 살아남는다.
void MainWindow::rebuildVitalCards()
{
    if (!vitalListLayout_) return;   // 아직 패널을 만들기 전(생성자 초기 단계)

    // ── 목표 키 순서 목록: (키, 이름, 병상표기) 3튜플. 순서가 곧 화면 배치 순서다. ──
    struct TargetEntry { int key; QString name; QString bedText; };
    QVector<TargetEntry> target;
    target.reserve(8);
    const QStringList rooms = roomNames();
    const QString room = rooms.value(selectedRoom_, currentRoomName());
    // 빈 방에는 배정된 사람이 없다 — 여기서 residentsByChannel_를 그대로 쓰면
    // 102호 화면에 101호 입소자의 심박이 뜬다.
    static const QVector<int> kNoResidents;
    for (int ch = 0; ch < 4; ++ch) {
        const QString bedText = QStringLiteral("%1 · 채널 %2").arg(room).arg(ch + 1);
        const QVector<int>& ids =
            (selectedRoom_ == 0) ? residentsByChannel_[ch] : kNoResidents;

        // 아무도 배정되지 않은 채널도 자리를 남긴다 — 카드가 통째로 사라지면
        // 관제사가 그 채널을 잊는다. 음수 키라 값이 안 들어와 "대기"로 뜬다.
        if (ids.isEmpty()) {
            target.append({-(ch + 1), QStringLiteral("미배정"), bedText});
            continue;
        }
        for (int rid : ids)
            target.append({rid, residentInfo_.value(rid).name, bedText});
    }

    // ── 사라진 키를 제거한다. 순회 중 해시를 수정하지 않도록 대상 키를 먼저
    //    모은 뒤 별도 루프에서 지운다. ──
    QSet<int> targetKeys;
    for (const auto& t : target) targetKeys.insert(t.key);
    QVector<int> staleKeys;
    for (auto it = vitalTiles_.constBegin(); it != vitalTiles_.constEnd(); ++it)
        if (!targetKeys.contains(it.key())) staleKeys.append(it.key());
    for (int key : staleKeys) {
        if (VitalTile* tile = vitalTiles_.take(key)) {
            vitalListLayout_->removeWidget(tile);
            tile->setParent(nullptr);
            tile->deleteLater();
        }
    }

    // ── 목표 목록을 순서대로 훑으며 생성 또는 갱신한다. ──
    for (const auto& t : target) {
        VitalTile* tile = vitalTiles_.value(t.key);
        if (!tile) {
            tile = buildVitalCard(t.key, t.name, t.bedText);
            vitalListLayout_->addWidget(tile, 1);
        } else {
            // 조건 없이 호출한다 — PD-01의 멱등 가드가 변화 없을 때를 흡수하고,
            // 같은 입소자가 다른 채널로 옮겨져 병상 표기만 바뀐 경우를 놓치지 않는다.
            tile->setIdentity(t.name, t.bedText);
        }
    }

    // ── 배치 순서를 목표 목록과 일치시킨다. 살아남은 타일이 섞여 있으면
    //    인덱스가 어긋날 수 있다. ──
    for (int i = 0; i < target.size(); ++i) {
        VitalTile* tile = vitalTiles_.value(target[i].key);
        if (!tile) continue;
        if (vitalListLayout_->indexOf(tile) != i) {
            vitalListLayout_->removeWidget(tile);
            vitalListLayout_->insertWidget(i, tile, 1);
        }
    }

    updateVitals();   // 새로 만든 위젯에 현재 값·색을 즉시 반영
}

VitalTile* MainWindow::buildVitalCard(int key, const QString& name, const QString& bedText)
{
    auto* tile = new VitalTile();
    applyCardShadow(tile, 20, 5, 60);   // 바이탈 카드에 은은한 입체감(static이라 타일 밖에서 건다 — PD-02)
    tile->setIdentity(name, bedText);
    // 카드를 다시 만들어도 그래프가 리셋되지 않도록 보관해둔 이력을 다시 부어넣는다.
    // (다른 입소자가 추가·퇴원했다고 이 사람 추세가 사라지면 안 된다)
    for (double v : hrHistory_.value(key)) tile->pushHeartRateSample(v);
    vitalTiles_[key] = tile;
    return tile;
}

// ═══════════════════════════════════════════════════════════
//  영상 표시 — 수신·파싱·디코드는 StreamClient(streamclient.cpp)가 하고,
//  채널별 "가장 최신" 프레임만 이리로 넘어온다.
// ═══════════════════════════════════════════════════════════
void MainWindow::onStreamFrame(int ch, const QImage& image, qint64 serverTimestampMs)
{
    if (ch < 0 || ch >= 4 || image.isNull()) return;
    // 해제한 채널은 서버가 잠깐 더 보내는 프레임을 무시 → 검은 미연결 화면 유지
    // (StreamClient도 디코드 전에 거르지만, 차단 직후 이미 디코드된 1장 대비)
    if (videoSuppressed_[ch]) return;

    // 지연 시간(Latency) 모니터링 — 최신 프레임 기준
    // ※ 서버·클라 PC 시계가 NTP로 동기화돼 있어야 값이 정확하다.
    const qint64 latency = QDateTime::currentMSecsSinceEpoch() - serverTimestampMs;
    Q_UNUSED(latency);
    //qDebug() << "Channel:" << ch << " | Latency:" << latency << "ms";

    const QPixmap pix = QPixmap::fromImage(image);
    lastFramePix_[ch] = pix;           // 이미지 탭 Before/After 프리뷰용 최신본 보관
    channelViews[ch]->setFrame(pix);
    const bool wasLive = channelViews[ch]->live();
    channelViews[ch]->setLive(true);   // 프레임 도착 → LIVE 표시등 점등
    // 죽어 있던 채널이 살아난 순간에만 트리를 다시 칠한다 — 매 프레임 갱신하면
    // 초당 수십 번 트리 전체를 다시 그리게 된다.
    if (!wasLive) refreshResourceTree();
    lastFrameMs_[ch] = QDateTime::currentMSecsSinceEpoch();  // checkChannelHealth() 판정용
    // ROI 편집기가 이 채널을 보고 있고 설정 탭이 열려 있으면 편집 영상도 실시간 갱신.
    if (roiEditorView && roiEditChannel == ch && cameraSettingsVisible()) {
        roiEditorView->setFrame(pix);
        roiEditorView->setLive(true);   // 프레임이 오는 중 → LIVE 점등
    }
    // 좌측 채널 스트립 썸네일 — 작게 그리므로 비용은 출력 픽셀 수에 비례해 미미하다.
    if (camThumbs[ch] && cameraSettingsVisible())
        camThumbs[ch]->setFrame(pix);
    // 이미지 탭 '적용 후(실시간)' 프리뷰도 같은 프레임으로 즉시 갱신 — 라이브 영상과
    // 동일 경로라 지연이 붙지 않는다. (이미지 모드일 때만: 나머지 모드에선 안 보임)
    if (imgWipe_ && roiEditChannel == ch && cameraSettingsVisible() &&
        camMode_ == QStringLiteral("이미지"))
        setImagePreviewFrame(pix);   // 첫 프레임에서 상자 비율도 함께 잡힌다
}

// ═══════════════════════════════════════════════════════════
//  낙상 이벤트 — 빨간색 테두리 활성화 및 로그 추가
// ═══════════════════════════════════════════════════════════
// 하단 타임라인에 이벤트 눈금을 하나 찍는다. 이벤트 로그(logTable)와 별개로
// 가벼운 목록을 따로 들고 있는다 — 타임라인은 "언제 무슨 색"만 알면 되고,
// 표를 매번 훑으면 그릴 때마다 전체 로그를 파싱하게 된다.
void MainWindow::pushTimelineEvent(int channel, qint64 atMs, const QColor& color)
{
    if (channel < 0 || channel >= 4) return;
    timelineEvents_.push_back({atMs, channel, color});
    // NVR 보존기간(12시간)보다 오래된 마커는 타임라인에 나올 일이 없다.
    const qint64 cutoff = QDateTime::currentMSecsSinceEpoch() - 24LL * 3600 * 1000;
    timelineEvents_.erase(
        std::remove_if(timelineEvents_.begin(), timelineEvents_.end(),
                       [cutoff](const TimelineEvent& e) { return e.atMs < cutoff; }),
        timelineEvents_.end());
    refreshTimeline();
}

void MainWindow::handleFallEvent(int channel, int roiId, quint64 timestampMs,
                                 float nx, float ny)
{
    // 서버가 이 사람을 어느 침대 입소자로 귀속했는지 — 못 밝혔으면 roiId<0.
    // 낙상자는 침대 밖이라 위치로는 알 수 없고, 서버 IdentityTracker의 추정이다.
    const QString who = eventWhoLabel(channel, roiId);

    // 1. 빨간 테두리 즉각 활성화!
    if (channel >= 0 && channel < 4) {
        fallActive[channel] = true;
        if (channelViews[channel]) {
            // 서버가 보낸 낙상 발생 위치(정규화 0~1)에 십자 조준점 표시
            channelViews[channel]->setAlert(
                true, QStringLiteral("🚨 낙상 감지 · %1").arg(who), QPointF(nx, ny));
        }
        qDebug() << "🚨 [낙상 감지] 채널" << (channel + 1) << who
                 << "빨간 테두리 켜짐 (모자이크 자동 해제 상태)";
    }
    refreshAlarmButton();       // 경보 활성 → 해제 버튼 빨강 채움으로 강조
    setVideoFocus(channel);     // 감지 채널을 크게, 나머지는 작게(스포트라이트)
    pushTimelineEvent(channel, qint64(timestampMs), QColor(QString::fromLatin1(kCritical)));

    // 2. 비상 로그 조회 탭에 URL 및 정보 등록
    if (logTable)
        appendLiveEvent(channel, roiId, qint64(timestampMs),
                        QStringLiteral("FALL"), QStringLiteral("CAMERA"));
}

// ═══════════════════════════════════════════════════════════
//  침상 이탈 이벤트 — 빨간색 테두리 활성화 및 로그 추가
// ═══════════════════════════════════════════════════════════
void MainWindow::handleBedEgressEvent(int channel, int roiId, quint64 timestampMs,
                                      float nx, float ny)
{
    // 이탈은 "어느 침대에서 나갔는가"가 판정 자체라, 낙상과 달리 추정이 아니라
    // 확정된 침대다. 그 침대에 매핑된 입소자가 곧 누구인지.
    const QString who = eventWhoLabel(channel, roiId);

    // 1. 빨간 테두리 즉각 활성화!
    if (channel >= 0 && channel < 4) {
        bedEgressActive[channel] = true;
        if (channelViews[channel]) {
            channelViews[channel]->setAlert(
                true, QStringLiteral("⚠️ 침대 이탈 · %1").arg(who),
                (nx > 0 || ny > 0) ? QPointF(nx, ny) : QPointF(-1, -1));
        }
        qDebug() << "⚠️ [침상 이탈 감지] 채널" << (channel + 1) << who << "빨간 테두리 켜짐";
    }
    refreshAlarmButton();       // 경보 활성 → 해제 버튼 빨강 채움으로 강조
    setVideoFocus(channel);     // 감지 채널을 크게, 나머지는 작게(스포트라이트)
    pushTimelineEvent(channel, qint64(timestampMs), QColor(QString::fromLatin1(kHigh)));

    // 2. 비상 로그 조회 탭에 블랙박스 URL 및 정보 등록
    if (logTable)
        appendLiveEvent(channel, roiId, qint64(timestampMs),
                        QStringLiteral("EGRESS"), QStringLiteral("CAMERA"));
}

// ═══════════════════════════════════════════════════════════
//  웨어러블 생체신호 이상 — 빨간색 테두리 활성화 및 로그 추가
//
//  카메라가 아니라 웨어러블이 근거인 경보다. 발생 위치라는 개념이 없어
//  십자 조준점을 찍지 않는 것만 낙상/이탈과 다르고, 나머지 흐름은 같다.
// ═══════════════════════════════════════════════════════════
void MainWindow::handleVitalAbnormalEvent(int channel, quint64 timestampMs)
{
    // 1. 빨간 테두리 즉각 활성화
    if (channel >= 0 && channel < 4) {
        vitalAbnormalActive[channel] = true;
        if (channelViews[channel]) {
            channelViews[channel]->setAlert(true, QStringLiteral("🚨 생체신호 이상"));
        }
        qDebug() << "🚨 [생체신호 이상] 채널" << (channel + 1) << "빨간 테두리 켜짐";
    }
    refreshAlarmButton();       // 경보 활성 → 해제 버튼 빨강 채움으로 강조
    setVideoFocus(channel);     // 감지 채널을 크게, 나머지는 작게(스포트라이트)
    pushTimelineEvent(channel, qint64(timestampMs), QColor(QString::fromLatin1(kWarn)));

    // 2. 비상 로그 조회 탭에 블랙박스 URL 및 정보 등록
    if (logTable)
        appendLiveEvent(channel, kRoiIdNone, qint64(timestampMs),
                        QStringLiteral("VITAL_ABNORMAL"), QStringLiteral("WEARABLE"));
}

// ═══════════════════════════════════════════════════════════
//  원격 방송(인터콤)
// ═══════════════════════════════════════════════════════════
// 마이크 버튼 토글 — 켜면 알림 노드에 MIC_ON 을 보내 사이렌/WAV 를 멈추고 마이크만
// 내보내게 하고, 우리 쪽 마이크 캡처(m_transmitter)도 그 노드로 UDP 송신을 시작한다.
// 끄면 반대로 MIC_OFF 를 보내 노드가 WAV 를 다시 재생할 수 있는 상태로 돌아간다.
// 순서: 노드가 사이렌을 먼저 끊게(MQTT) 하고 나서 오디오를 흘려보내기 시작한다 —
// 반대로 하면 노드가 아직 사이렌을 쥔 채로 우리 패킷이 먼저 도착해 초반 소리가 씹힌다.
void MainWindow::onMicToggled(bool on)
{
    micButton->setText(on ? QStringLiteral("🔴 방송 중")
                          : QStringLiteral("🎤 방송"));
    micButton->setProperty("active", on);
    micButton->style()->unpolish(micButton);
    micButton->style()->polish(micButton);

    const QString targetIp   = "172.20.35.40"; // 알림 라즈베리파이 IP
    const quint16 targetPort = 5000;
    const QString node = (alertNode_ && !alertNode_->currentText().isEmpty())
                              ? alertNode_->currentText()
                              : QStringLiteral("alarm_rpi_01");

    if (on) {
        if (mqtt) mqtt->sendBroadcastToggle(node, true);
        if (!m_transmitter->startBroadcast(targetIp, targetPort)) {
            qWarning() << "방송 시작 실패";
        }
        qDebug() << "인터콤 방송 시작";
    } else {
        m_transmitter->stopBroadcast();
        if (mqtt) mqtt->sendBroadcastToggle(node, false);
        qDebug() << "인터콤 방송 종료";
    }
}

// ═══════════════════════════════════════════════════════════
//  [경보 해제] 버튼 클릭 시 동작 (즉각적인 테두리 OFF + 마스크 ON 패킷 송신)
// ═══════════════════════════════════════════════════════════
void MainWindow::onAlarmClearClicked()
{
    bool packetSent = false;

    // 되묻는 팝업 없이 버튼 클릭 즉시 원스톱으로 리셋 처리!
    for (int channel = 0; channel < 4; ++channel) {
        if (fallActive[channel] || bedEgressActive[channel] || vitalAbnormalActive[channel]) {
            // 1. 빨간 테두리 끄고 로컬 경보 상태 클리어 (낙상·침상이탈·생체이상 모두)
            fallActive[channel] = false;
            bedEgressActive[channel] = false;
            vitalAbnormalActive[channel] = false;
            if (channelViews[channel]) {
                channelViews[channel]->setAlert(false);
            }

            // 2. 서버의 PrivacyMasker를 깨워서 다시 모자이크 씌우라고 0x03 바이너리 쏘기
            //    (채널마다 담당 Pi가 다르므로 그 채널의 소켓으로 보낸다)
            if (stream_->sendAlarmConfirm(channel))
                packetSent = true;

            // 서버로 보내는 0x03 은 "모자이크를 다시 씌워라"는 뜻이고, 현장의
            // 사이렌·LED 는 알림 노드가 들고 있어 브로커를 통해 따로 꺼야 한다.
            // 경로가 달라 TCP 성공 여부와 무관하게 보낸다.
            if (mqtt) mqtt->sendAlarmClear(patients[channel].room);
        }
    }

    // (flush는 위 루프에서 채널별 소켓마다 이미 처리)
    refreshAlarmButton();   // 모두 해제됐으니 버튼을 차분한 아웃라인으로 되돌린다
    setVideoFocus(-1);      // 경보 해제 → 균등 2×2로 복귀
    if (!packetSent) {
        // 현재 활성화된 경보가 아예 없을 때만 안내 메시지 표시
        QMessageBox::information(this, QStringLiteral("경보 해제"),
                                 QStringLiteral("현재 활성화된 낙상/침상이탈 경보가 없습니다."));
    }
}


// 낙상/침상이탈이 하나라도 활성이면 경보 버튼을 빨강 채움으로, 아니면 차분한 아웃라인으로.
void MainWindow::refreshAlarmButton()
{
    if (!alarmClearButton) return;
    bool anyActive = false;
    for (int ch = 0; ch < 4; ++ch)
        if (fallActive[ch] || bedEgressActive[ch] || vitalAbnormalActive[ch]) { anyActive = true; break; }
    updateAlarmBanner();   // 경보 토스트 표시/문구 갱신 (활성 시에만 내려온다)

    // 경보가 하나라도 활성이면 창 전체 테두리 빨강 펄스, 아니면 끈다.
    if (alarmOverlay_) {
        auto* ov = static_cast<AlarmOverlay*>(alarmOverlay_);
        if (anyActive) { ov->setGeometry(ui->centralwidget->rect()); ov->start(); }
        else           ov->stop();
    }
}
