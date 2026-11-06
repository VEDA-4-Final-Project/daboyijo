// MainWindow — 웨어러블 바이탈·MQTT 수신
// (mainwindow.cpp를 기능별로 나눈 파일 중 하나. 공용 헬퍼는 mainwindow_p.h)
#include "mainwindow_p.h"

// ═══════════════════════════════════════════════════════════
//  웨어러블 바이탈 — MQTT(veda/wearable/data)로 들어온 값을 표시한다.
//
//  값이 없거나 오래된 채널은 "--" 로 둔다. 그럴듯한 숫자를 대신 띄우면
//  관제사가 멀쩡한 줄 알기 때문에, 모를 때는 모른다고 표시한다.
//
//  이 함수는 "표시만" 담당한다. 값 저장과 그래프 점 추가는 데이터가 실제로
//  도착한 순간(onWearableData)에 하고, 여기 타이머는 신호가 끊긴 걸 시간이
//  지나 알아채는 역할이다. (팔레트 전환 때도 색을 다시 입히려고 호출된다)
// ═══════════════════════════════════════════════════════════
void MainWindow::updateVitals()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // 판정(무신호 3종 구분·등급·라벨·도형 접두)은 전부 여기서 한다 — VitalTile은
    // 완성된 표시값만 setLive()/setStale()로 받는다(D-02). 위젯 갱신의 4단계
    // repolish 관용구도 VitalTile 내부로 옮겨갔다(client/vitaltile.cpp).

    // 카드 단위로 돈다. 키는 입소자면 resident_id, 미배정 채널이면 음수 —
    // 음수 키는 vitals_ 에 값이 없어 기본값(received=false)이 잡히고 "대기"로 뜬다.
    for (auto it = vitalTiles_.constBegin(); it != vitalTiles_.constEnd(); ++it) {
        const int key = it.key();
        VitalTile* tile = it.value();
        if (!tile) continue;

        const VitalSample v = vitals_.value(key);
        const bool fresh = v.received && (now - v.arrivedAtMs) <= kVitalStaleMs;
        // 값은 오는데 전부 0 = 웨어러블 미착용. '신호 끊김'과도, '이상'과도 다른
        // 제3의 상태라 따로 표시한다(요양사 대응이 "기기를 채우세요"로 달라진다).
        const bool worn = vitalWorn(v.spo2, v.heartRate);

        if (!fresh || !worn) {
            // 세 가지를 구분한다 — 대응이 각각 다르다.
            //   대기      : 한 번도 안 옴 (등록/배선 문제)
            //   신호 끊김 : 오다가 멈춤 (기기 방전·중계 노드 다운)
            //   미착용    : 값은 오는데 전부 0 (기기를 안 차고 있음)
            // 도형(○)은 색 없이도 "이 카드는 심각도 등급이 아니라 신호가
            // 없는 상태"임을 알리는 채널 — 세 상태를 구분하는 건 텍스트다.
            const QString label = !fresh ? (v.received ? QStringLiteral("신호 끊김")
                                                        : QStringLiteral("대기"))
                                          : QStringLiteral("미착용");
            const QString badgeText = severityGlyph(QString(), /*stale=*/true)
                                       + QStringLiteral(" ") + label;
            // 커스텀 페인트 위젯(Sparkline)은 QSS를 못 받으므로 색 헬퍼를 거쳐
            // 중립색을 직접 받는다 — 전역 상수를 여기서 다시 읽지 않는다.
            tile->setStale(badgeText, severityColor(QString()));
            continue;
        }

        const int spo2 = v.spo2;
        const int hr   = v.heartRate;
        const QString severity = vitalSeverity(spo2, hr);
        // 상태 배지에만 등급 도형을 접두한다 — SpO2/심박 값 라벨엔 붙이지
        // 않는다(숫자 판독 방해 + 배지가 이미 도형을 들고 있어 중복 부호화는
        // 이미 충족된다).
        const QString status = vitalStatusLabel(spo2, hr);
        const QString badgeText = severityGlyph(severity) + QStringLiteral(" ") + status;
        tile->setLive(spo2, hr, severity, badgeText, severityColor(severity));
    }
}

// ═══════════════════════════════════════════════════════════
//  MQTT 수신 — 웨어러블 생체·낙상 (veda/wearable/data)
// ═══════════════════════════════════════════════════════════
void MainWindow::onWearableData(const WearableData& data)
{
    // 브로커는 기기 id 로만 알려준다. 그 기기가 누구 것인지로 건너간다.
    // ★ 채널이 아니라 입소자로 잇는다 — 한 채널에 여러 명이 있으면 채널로는
    //   누구 값인지 구분이 안 돼 나중에 온 값이 앞 값을 덮어쓴다.
    // 등록되지 않은 기기는 누구 것인지 알 수 없어 버린다 — 엉뚱한 사람 자리에
    // 남의 심박수를 띄우느니 안 띄우는 편이 낫다.
    const QString id = QString::fromStdString(data.device_id).trimmed();
    const auto it = wearableToResident.constFind(id);
    if (it == wearableToResident.constEnd()) {
        qDebug() << "[MQTT] 미등록 웨어러블 무시:" << id
                 << "(residents.wearable_id 에 등록하면 해당 입소자에게 표시됩니다)";
        return;
    }

    const int rid = it.value();

    VitalSample& v = vitals_[rid];
    v.received    = true;
    v.heartRate   = data.heart_rate;
    v.spo2        = data.spo2;
    v.arrivedAtMs = QDateTime::currentMSecsSinceEpoch();

    // 그래프 점은 값이 실제로 도착했을 때만 찍는다.
    // 위젯 밖(hrHistory_)에도 같이 쌓아둬야 카드를 다시 만들 때 추세가 살아난다.
    QVector<double>& hist = hrHistory_[rid];
    hist.append(data.heart_rate);
    while (hist.size() > kHrHistoryMax) hist.removeFirst();
    if (VitalTile* tile = vitalTiles_.value(rid)) tile->pushHeartRateSample(data.heart_rate);

    // 웨어러블이 낙상을 감지한 경우. 카메라 낙상(TCP 0xDB4D)과는 별개 경로라
    // 같은 사건이 두 번 들어올 수 있다 — 이미 경보 중인 채널은 다시 울리지 않는다.
    // ★ 웨어러블은 기기가 사람마다 달라서 "누가" 넘어졌는지까지 알 수 있다.
    //   (카메라 낙상은 채널까지만 알 수 있어 이름을 붙일 수 없다)
    const ResidentInfo info = residentInfo_.value(rid);
    if (data.is_fall_detected && info.channel >= 0 && info.channel < 4
        && !fallActive[info.channel]) {
        qDebug() << "[MQTT] 웨어러블 낙상 감지 —" << info.name
                 << "채널" << (info.channel + 1) << "기기" << id;

        // 카메라 낙상(handleFallEvent)과 같은 상태로 만든다. 다만 비상 로그에는
        // 넣지 않는다 — 그쪽 행은 블랙박스 클립 URL 을 달고 있는데, 웨어러블만
        // 감지한 낙상은 녹화된 클립이 없어서 눌러도 열리지 않는 행이 된다.
        fallActive[info.channel] = true;
        if (channelViews[info.channel]) {
            // 웨어러블은 기기가 사람마다 달라 "누가" 넘어졌는지까지 띄울 수 있다.
            channelViews[info.channel]->setAlert(
                true, QStringLiteral("🚨 %1 낙상 감지").arg(info.name));
        }
        refreshAlarmButton();          // → updateAlarmBanner() 로 상단 배너가 내려온다
        setVideoFocus(info.channel);   // 감지 채널을 크게(스포트라이트)
    }

    updateVitals();   // 도착 즉시 화면 반영 (2초 타이머를 기다리지 않는다)
}

// ═══════════════════════════════════════════════════════════
//  MQTT 수신 — 알림 노드로 나간 제어 명령 (veda/alarm/control)
//  우리가 보낸 것도 되돌아오므로, 로그에 쌓을 때는 걸러야 한다.
// ═══════════════════════════════════════════════════════════
void MainWindow::onMqttAlarm(const AlarmCommand& cmd)
{
    qDebug() << "[MQTT] 알림 명령:"
             << QString::fromStdString(cmd.type)
             << cmd.room                             // AlarmCommand::room 은 정수(호실 번호)
             << QString::fromStdString(cmd.message);
}

void MainWindow::onMqttConnected()
{
    qInfo() << "[MQTT] 브로커 연결됨 —" << brokerHost() << ":" << brokerPort();
}

void MainWindow::onMqttDisconnected()
{
    // 값이 끊긴 건 updateVitals 가 30초 뒤 "신호 끊김" 으로 알려준다.
    qWarning() << "[MQTT] 브로커 연결 끊김";
}

void MainWindow::onMqttError(const QString& message)
{
    qWarning() << "[MQTT]" << message;
}

void MainWindow::onAlarmNodeStatus(const QString& node, bool online)
{
    alertNodeOnline_[node] = online;
    if (alertNode_ && alertNode_->currentText() == node)
        refreshAlertStatusBadge();
}

// 배지 3상태 — severity 속성 + #alertNodeBadge QSS 규칙.
//   미확인(도형 ○, severity 없음) / 온라인(✓, normal) / 오프라인(✖, critical)
void MainWindow::refreshAlertStatusBadge()
{
    if (!alertStatusBadge_ || !alertNode_) return;
    const QString node = alertNode_->currentText();
    QString text, severity;
    if (!alertNodeOnline_.contains(node)) {
        text = QStringLiteral("상태 미확인");
        severity = QString();  // 심각도 어느 값에도 걸리지 않는 중립 상태
    } else if (alertNodeOnline_.value(node)) {
        text = QStringLiteral("온라인");
        severity = QStringLiteral("normal");
    } else {
        text = QStringLiteral("오프라인");
        severity = QStringLiteral("critical");
    }
    alertStatusBadge_->setText(severityGlyph(severity, severity.isEmpty())
                                + QStringLiteral(" ") + text);
    alertStatusBadge_->setProperty("severity", severity);
    alertStatusBadge_->style()->unpolish(alertStatusBadge_);
    alertStatusBadge_->style()->polish(alertStatusBadge_);
    alertStatusBadge_->update();
}
