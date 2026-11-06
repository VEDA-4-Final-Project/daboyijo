// MainWindow — 영상 검색 탭(🔍)
// (mainwindow.cpp를 기능별로 나눈 파일 중 하나. 공용 헬퍼는 mainwindow_p.h)
#include "mainwindow_p.h"

// ═══════════════════════════════════════════════════════════
//  TAB: 영상 검색(🔍) — 좌측 네비의 독립 페이지. 케어봇과 같은 서버 로직
//  (video_search_module)을 관제 화면에서도 쓴다. 질의는 DBJ_CTRL_SEARCH_QUERY로
//  보내고, 응답(DBJ_SEARCH_MAGIC)은 StreamClient가 받아 onSearchResultReceived로
//  넘긴다.
//  재생기는 이 페이지에 없다 — 이벤트 기록 페이지(kNavEventLog)의 인라인
//  재생기를 그대로 쓴다(재생기를 두 곳에 중복해서 두지 않기 위해). 검색
//  결과의 클립 링크를 누르면 그 페이지로 이동해 바로 재생된다.
// ═══════════════════════════════════════════════════════════
QWidget* MainWindow::buildVideoSearchTab()
{
    auto* panel = new QFrame();
    panel->setObjectName("panel");

    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(14, 12, 14, 14);
    outer->setSpacing(10);

    auto* title = new QLabel(QStringLiteral("영상 검색"));
    title->setObjectName("panelTitle");
    outer->addWidget(title);

    // 3단: 조건 | 결과 목록 | 재생기. 결과를 누르면 이 페이지 안에서 바로 재생된다
    // — 예전엔 이벤트 기록 페이지로 튕겨 보내서, 검색을 이어가려면 되돌아와야 했다.
    auto* body = new QHBoxLayout();
    body->setSpacing(12);

    // ── 좌: 검색 조건 ──
    auto* side = new QFrame();
    side->setObjectName("filterBar");
    side->setFixedWidth(248);
    auto* sv = new QVBoxLayout(side);
    sv->setContentsMargins(12, 12, 12, 12);
    sv->setSpacing(6);

    auto* cap = new QLabel(QStringLiteral("검색 조건"));
    cap->setObjectName("filterCap");
    sv->addWidget(cap);
    sv->addSpacing(4);

    auto* chCap = new QLabel(QStringLiteral("채널"));
    chCap->setObjectName("filterFieldCap");
    sv->addWidget(chCap);
    searchChannelCombo = new QComboBox();
    searchChannelCombo->setObjectName("searchChannelCombo");
    // 기본값 = 전체 채널 — 질문할 때 채널을 매번 고르지 않아도 전체에서 찾아준다.
    searchChannelCombo->addItem(QStringLiteral("전체 채널"), -1);
    for (int ch = 0; ch < 4; ++ch)
        searchChannelCombo->addItem(QStringLiteral("채널 %1").arg(ch + 1), ch);
    sv->addWidget(searchChannelCombo);
    sv->addSpacing(6);

    auto* qCap = new QLabel(QStringLiteral("질문"));
    qCap->setObjectName("filterFieldCap");
    sv->addWidget(qCap);
    searchQueryEdit = new QLineEdit();
    searchQueryEdit->setObjectName("searchQueryEdit");
    searchQueryEdit->setPlaceholderText(
        QStringLiteral("예: 어제 저녁에 낙상 있었어?"));
    sv->addWidget(searchQueryEdit);
    sv->addSpacing(8);

    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(6);
    searchButton = new QPushButton(QStringLiteral("검색"));
    searchButton->setObjectName("camPrimary");
    searchButton->setCursor(Qt::PointingHandCursor);
    btnRow->addWidget(searchButton, 1);
    auto* resetBtn = new QPushButton(QStringLiteral("초기화"));
    resetBtn->setObjectName("filterResetBtn");
    resetBtn->setCursor(Qt::PointingHandCursor);
    connect(resetBtn, &QPushButton::clicked, this, [this] {
        searchQueryEdit->clear();
        searchChannelCombo->setCurrentIndex(0);
        searchResultList_->clear();
        searchResultList_->show();
        if (searchPlayer_) searchPlayer_->stop();
        if (searchPlayerStack_) searchPlayerStack_->setCurrentIndex(0);
        if (searchContext_) searchContext_->hide();
        if (searchMessage_) searchMessage_->hide();
        if (searchCountLabel_) searchCountLabel_->clear();
    });
    btnRow->addWidget(resetBtn);
    sv->addLayout(btnRow);
    sv->addSpacing(10);

    // 예시 질문 — 누르면 질문칸이 채워지고 바로 검색된다. 자연어 검색은 "뭐라고
    // 물어야 하는지"가 가장 큰 진입 장벽이라, 실제 통하는 문장을 눌러 보게 한다.
    auto* exCap = new QLabel(QStringLiteral("예시 질문"));
    exCap->setObjectName("filterFieldCap");
    sv->addWidget(exCap);
    const QString examples[3] = {
        QStringLiteral("어제 저녁에 낙상 있었어?"),
        QStringLiteral("이번 주에 침대에서 나간 적 있어?"),
        QStringLiteral("오늘 새벽에 무슨 일 있었어?"),
    };
    for (const QString& ex : examples) {
        auto* chip = new QPushButton(ex);
        chip->setObjectName("exampleChip");
        chip->setCursor(Qt::PointingHandCursor);
        connect(chip, &QPushButton::clicked, this, [this, ex] {
            searchQueryEdit->setText(ex);
            sendSearchQuery();
        });
        sv->addWidget(chip);
    }
    sv->addStretch();
    body->addWidget(side, 0);

    // ── 중: 결과 목록 ──
    auto* mid = new QWidget();
    mid->setFixedWidth(300);
    auto* mv = new QVBoxLayout(mid);
    mv->setContentsMargins(0, 0, 0, 0);
    mv->setSpacing(6);

    searchCountLabel_ = new QLabel();
    searchCountLabel_->setObjectName("logCount");
    mv->addWidget(searchCountLabel_);

    searchResultList_ = new QListWidget();
    searchResultList_->setObjectName("searchResultList");
    searchResultList_->setWordWrap(true);
    searchResultList_->setCursor(Qt::PointingHandCursor);
    // 한 번 클릭으로 재생 — 더블클릭까지 기다리게 하면 "왜 안 나오지"를 매번 겪는다.
    connect(searchResultList_, &QListWidget::itemClicked, this,
            [this](QListWidgetItem* item) {
                if (!item) return;
                playSearchClip(item->data(Qt::UserRole).toString(),
                               item->data(Qt::UserRole + 1).toString());
            });
    mv->addWidget(searchResultList_, 1);

    // 목록으로 볼 수 없는 답변(안내문·오류)은 여기에 원문 그대로 띄운다.
    searchMessage_ = new QLabel();
    searchMessage_->setObjectName("searchMessage");
    searchMessage_->setWordWrap(true);
    searchMessage_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    // 목록이 숨은 자리에서 라벨이 세로로 늘어나 문구가 한가운데 뜨지 않도록
    // 높이는 내용에 고정하고, 남는 공간은 아래 스트레치가 가져간다.
    searchMessage_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    searchMessage_->hide();
    mv->addWidget(searchMessage_);
    mv->addStretch();

    body->addWidget(mid, 0);

    // ── 우: 재생기 ──
    auto* right = new QVBoxLayout();
    right->setSpacing(6);

    searchContext_ = new QLabel();
    searchContext_->setObjectName("eventContext");
    searchContext_->setWordWrap(true);
    searchContext_->hide();
    right->addWidget(searchContext_);

    searchPlayerStack_ = new QStackedWidget();
    auto* hint = new QLabel(
        QStringLiteral("왼쪽에서 질문하고, 가운데 목록에서 기록을 고르면 "
                       "여기서 바로 재생됩니다."));
    hint->setObjectName("camHint");
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    searchPlayerStack_->addWidget(hint);          // 0

    searchVideo_ = new QVideoWidget();
    searchVideo_->setObjectName("playbackVideo");
    searchPlayerStack_->addWidget(searchVideo_);  // 1

    auto* stageCard = new QFrame();
    stageCard->setObjectName("camStage");
    auto* scl = new QVBoxLayout(stageCard);
    scl->setContentsMargins(0, 0, 0, 0);
    scl->addWidget(searchPlayerStack_);
    right->addWidget(stageCard, 1);

    // 재생 컨트롤
    auto* ctl = new QFrame();
    ctl->setObjectName("transportBar");
    auto* cl = new QHBoxLayout(ctl);
    cl->setContentsMargins(10, 6, 10, 6);
    cl->setSpacing(8);
    searchPlayPause_ = new QPushButton(QStringLiteral("▶"));
    searchPlayPause_->setObjectName("transportBtn");
    searchPlayPause_->setCursor(Qt::PointingHandCursor);
    searchPlayPause_->setFixedSize(30, 26);
    cl->addWidget(searchPlayPause_);
    searchSeek_ = new QSlider(Qt::Horizontal);
    searchSeek_->setObjectName("blackboxSeek");
    cl->addWidget(searchSeek_, 1);
    searchTimeLabel_ = new QLabel(QStringLiteral("00:00 / 00:00"));
    searchTimeLabel_->setObjectName("transportTime");
    cl->addWidget(searchTimeLabel_);
    right->addWidget(ctl);

    body->addLayout(right, 1);
    outer->addLayout(body, 1);

    connect(searchButton, &QPushButton::clicked, this, &MainWindow::sendSearchQuery);
    connect(searchQueryEdit, &QLineEdit::returnPressed, this, &MainWindow::sendSearchQuery);

    return panel;
}

// 서버 답변을 결과 목록으로 쪼갠다.
//
// 형식(video_search_module.cpp):
//   🔎 검색 결과 3건
//   (빈 줄)
//   · 2026-08-18 11:06 · 채널 4 · 낙상 · 전승현님
//     http://host:5501/ch3_...mp4
// "· "로 시작하면 새 결과, 들여쓴 http 줄은 직전 결과의 클립이다.
// 목록이 아닌 답변(안내문·"기록을 찾지 못했어요")은 message 로 넘긴다.
QVector<MainWindow::SearchHit> MainWindow::parseSearchReply(const QString& text,
                                                            QString* message)
{
    QVector<SearchHit> hits;
    const QStringList lines = text.split(QLatin1Char('\n'));

    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QString::fromUtf8("· "))) {
            const QString rest = line.mid(2).trimmed();
            SearchHit hit;
            const int sep = rest.indexOf(QString::fromUtf8(" · "));
            if (sep > 0) {
                hit.when = rest.left(sep);
                hit.meta = rest.mid(sep + 3);
            } else {
                hit.when = rest;
            }
            hits.push_back(hit);
        } else if (!hits.isEmpty() && line.startsWith(QLatin1String("http")) &&
                   line.endsWith(QLatin1String(".mp4"))) {
            hits.back().url = line;
        }
    }

    if (hits.isEmpty() && message) *message = text.trimmed();
    return hits;
}

// 검색 결과 클립을 이 페이지 재생기에서 튼다.
void MainWindow::playSearchClip(const QString& url, const QString& context)
{
    if (searchContext_) {
        searchContext_->setText(context);
        searchContext_->show();
    }
    if (url.isEmpty()) {
        // 서버가 public_host 를 모르면 URL 없이 결과만 온다(모듈 주석 참고).
        if (searchPlayerStack_) searchPlayerStack_->setCurrentIndex(0);
        if (searchPlayer_) searchPlayer_->stop();
        return;
    }

    if (!searchPlayer_) {
        searchPlayer_ = new QMediaPlayer(this);
        searchPlayer_->setVideoOutput(searchVideo_);
        connect(searchPlayer_, &QMediaPlayer::positionChanged, this, [this](qint64 pos) {
            if (!searchSeeking_ && searchSeek_) searchSeek_->setValue(int(pos));
            if (searchTimeLabel_ && searchPlayer_) {
                const auto fmt = [](qint64 ms) {
                    return QStringLiteral("%1:%2")
                        .arg(ms / 60000, 2, 10, QLatin1Char('0'))
                        .arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'));
                };
                searchTimeLabel_->setText(QStringLiteral("%1 / %2")
                                              .arg(fmt(pos), fmt(searchPlayer_->duration())));
            }
        });
        connect(searchPlayer_, &QMediaPlayer::durationChanged, this, [this](qint64 dur) {
            if (searchSeek_) searchSeek_->setRange(0, int(qMax<qint64>(0, dur)));
        });
        connect(searchPlayer_, &QMediaPlayer::playbackStateChanged, this,
                [this](QMediaPlayer::PlaybackState st) {
                    if (searchPlayPause_)
                        searchPlayPause_->setText(st == QMediaPlayer::PlayingState
                                                      ? QStringLiteral("⏸")
                                                      : QStringLiteral("▶"));
                });
        connect(searchPlayPause_, &QPushButton::clicked, this, [this] {
            if (!searchPlayer_) return;
            if (searchPlayer_->playbackState() == QMediaPlayer::PlayingState)
                searchPlayer_->pause();
            else
                searchPlayer_->play();
        });
        connect(searchSeek_, &QSlider::sliderPressed, this, [this] { searchSeeking_ = true; });
        connect(searchSeek_, &QSlider::sliderReleased, this, [this] {
            searchSeeking_ = false;
            if (searchPlayer_) searchPlayer_->setPosition(searchSeek_->value());
        });
    }

    if (searchPlayerStack_) searchPlayerStack_->setCurrentIndex(1);
    searchPlayer_->setSource(QUrl(url));
    searchPlayer_->play();
}

void MainWindow::sendSearchQuery()
{
    if (!searchChannelCombo || !searchQueryEdit || !searchButton || !searchResultList_)
        return;

    const QString query = searchQueryEdit->text().trimmed();
    if (query.isEmpty()) return;

    const int channel = searchChannelCombo->currentData().toInt();  // -1 = 전체 채널

    // 전체 채널(-1)이면 StreamClient가 연결된 아무 Pi로 보낸다(DB 공유).
    if (!stream_->sendSearchQuery(channel, query)) {
        searchResultList_->clear();
        searchResultList_->hide();
        searchCountLabel_->clear();
        searchMessage_->setText(QStringLiteral("영상 서버에 연결되어 있지 않습니다."));
        searchMessage_->show();
        return;
    }

    searchButton->setEnabled(false);
    searchResultList_->clear();
    searchResultList_->show();
    searchCountLabel_->setText(QStringLiteral("검색 중…"));
    searchMessage_->hide();

    // Gemini+DB 왕복이 수 초 걸릴 수 있어, 응답이 안 오면 버튼이 영원히 잠기지
    // 않도록 넉넉한 시간 후 자동 복구(서버 curl 타임아웃 20초보다 여유 있게).
    QTimer::singleShot(25000, this, [this]() {
        if (searchButton && !searchButton->isEnabled()) {
            searchButton->setEnabled(true);
            if (searchMessage_)
                searchMessage_->setText(
                    QStringLiteral("응답이 없어요. 다시 시도해 주세요."));
        }
    });
}

void MainWindow::onSearchResultReceived(int /*channel*/, const QString& text)
{
    if (searchButton) searchButton->setEnabled(true);
    if (!searchResultList_) return;

    QString message;
    const QVector<SearchHit> hits = parseSearchReply(text, &message);

    searchResultList_->clear();
    if (hits.isEmpty()) {
        // 목록이 아닌 답변(안내문·"기록을 찾지 못했어요")은 원문 그대로 보여준다.
        // 빈 목록 상자를 남겨 두면 안내문이 그 아래로 밀려 붙어 어색하다 —
        // 둘 중 하나만 자리를 차지한다.
        if (searchCountLabel_) searchCountLabel_->clear();
        searchResultList_->hide();
        if (searchMessage_) {
            searchMessage_->setText(message);
            searchMessage_->show();
        }
        return;
    }

    searchResultList_->show();
    if (searchMessage_) searchMessage_->hide();
    for (const SearchHit& h : hits) {
        auto* item = new QListWidgetItem(
            h.meta.isEmpty() ? h.when
                             : QStringLiteral("%1\n%2").arg(h.when, h.meta));
        item->setData(Qt::UserRole, h.url);
        item->setData(Qt::UserRole + 1,
                      h.meta.isEmpty() ? h.when
                                       : QStringLiteral("%1  ·  %2").arg(h.when, h.meta));
        // 클립이 없는 기록은 눌러도 재생할 게 없다 — 미리 알려 준다.
        if (h.url.isEmpty()) {
            item->setToolTip(QStringLiteral("저장된 클립이 없는 기록입니다"));
            item->setForeground(QColor(QString::fromLatin1(kTextSub)));
        }
        searchResultList_->addItem(item);
    }
    if (searchCountLabel_)
        searchCountLabel_->setText(QStringLiteral("%1건").arg(hits.size()));
}
