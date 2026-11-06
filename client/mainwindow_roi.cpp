// MainWindow — 침대 ROI 지정·표시·전송
// (mainwindow.cpp를 기능별로 나눈 파일 중 하나. 공용 헬퍼는 mainwindow_p.h)
#include "mainwindow_p.h"

// ═══════════════════════════════════════════════════════════
//  침대 ROI 지정 / 표시 / 서버 전송
// ═══════════════════════════════════════════════════════════
void MainWindow::onRoiButtonClicked()
{
    if (!roiEditorView) return;
    // 그리는 중이면 이 버튼은 "취소"로 동작
    if (roiEditorView->drawMode()) {
        roiEditorView->cancelDraft();
        return;
    }
    // 침대가 상한까지 찼으면 그리기를 시작하지 않는다 — 그려 놓고 "못 넣는다"고
    // 하면 그린 노동이 그대로 버려진다.
    if (roiEditorView->nextFreeZoneId() < 0) {
        QMessageBox::information(
            this, QStringLiteral("침대 추가"),
            QStringLiteral("채널 %1에는 침대를 최대 %2개까지 지정할 수 있습니다.\n"
                           "기존 침대를 제거한 뒤 다시 시도하세요.")
                .arg(roiEditChannel + 1)
                .arg(kMaxRoiZones));
        return;
    }
    // 편집기(현재 선택 채널)에 바로 그리기 시작 (좌클릭=점, 더블클릭=완료)
    roiEditorView->setDrawMode(true);
}

// "침대 제거" — 선택된 침대 1개, 선택이 없으면 채널 전체.
void MainWindow::onRoiClearClicked()
{
    const int ch = roiEditChannel;
    if (roiEditorView && roiEditorView->drawMode()) roiEditorView->cancelDraft();

    if (roiZones_[ch].isEmpty()) {
        QMessageBox::information(this, QStringLiteral("침대 제거"),
                                 QStringLiteral("채널 %1에 제거할 침대가 없습니다.").arg(ch + 1));
        return;
    }

    const int sel = roiEditorView ? roiEditorView->selectedZone() : -1;
    const bool all = (sel < 0);
    const QString what = all
        ? QStringLiteral("채널 %1의 침대 %2개를 모두 제거할까요?")
              .arg(ch + 1).arg(roiZones_[ch].size())
        : QStringLiteral("채널 %1의 침대 %2(%3)를 제거할까요?")
              .arg(ch + 1).arg(sel + 1).arg(zoneResidentName(ch, sel));

    if (QMessageBox::question(this, QStringLiteral("침대 제거"), what) != QMessageBox::Yes)
        return;

    // 서버에 삭제 통보 — roi_id에 kRoiIdAll을 실으면 그 채널 전체 삭제다.
    sendRoi(ch, all ? kRoiIdAll : sel, QPolygonF(), true);

    if (all) {
        roiZones_[ch].clear();
        roiResident_[ch].clear();
    } else {
        for (int i = 0; i < roiZones_[ch].size(); ++i) {
            if (roiZones_[ch][i].id != sel) continue;
            roiZones_[ch].removeAt(i);
            break;
        }
        roiResident_[ch].remove(sel);
    }
    refreshRoiZones(ch);
    qDebug() << "침대 제거: ch" << ch << (all ? -1 : sel);
}

// 침대 ROI를 DB(roi_zones)에서 읽어 화면에 복원한다.
// 서버도 같은 표를 읽어 판정을 복원하므로(server/src/main.cpp), Qt를 껐다 켜도
// 관제 화면과 서버가 같은 침대를 본다. 그리기 결과를 Qt 메모리에만 두면
// 재시작 후 화면엔 침대가 없는데 서버는 판정 중인 상태가 되어 서로 어긋난다.
void MainWindow::loadRoiZonesFromDb()
{
    for (int ch = 0; ch < 4; ++ch) {
        roiZones_[ch].clear();
        roiResident_[ch].clear();
    }

    QSqlQuery q;
    if (!q.exec(QStringLiteral(
            "SELECT camera_id, roi_id, resident_id, roi_points FROM roi_zones "
            "ORDER BY camera_id, roi_id"))) {
        qDebug() << "침대 ROI 조회 실패:" << q.lastError().text();
        return;
    }

    while (q.next()) {
        const int ch = q.value(0).toInt();
        const int id = q.value(1).toInt();
        if (ch < 0 || ch >= 4 || id < 0 || id >= kMaxRoiZones) continue;

        const int rid = q.value(2).isNull() ? 0 : q.value(2).toInt();
        if (rid > 0) roiResident_[ch][id] = rid;

        // roi_points는 서버가 쓴 [[x,y],...] JSON. 숫자만 순서대로 긁어 둘씩 묶는다
        // (이 컬럼에 다른 구조가 들어올 경로가 없다 — 쓰는 쪽이 서버 한 곳뿐).
        const QString json = q.value(3).toString();
        QVector<double> nums;
        for (int i = 0; i < json.size();) {
            const QChar c = json[i];
            if (c.isDigit() || c == QLatin1Char('-') || c == QLatin1Char('.')) {
                int j = i;
                while (j < json.size() &&
                       (json[j].isDigit() || json[j] == QLatin1Char('-') ||
                        json[j] == QLatin1Char('.') || json[j] == QLatin1Char('e')))
                    ++j;
                nums.push_back(json.mid(i, j - i).toDouble());
                i = j;
            } else {
                ++i;
            }
        }
        RoiZone z;
        z.id = id;
        for (int i = 0; i + 1 < nums.size(); i += 2)
            z.poly << QPointF(nums[i], nums[i + 1]);
        if (z.poly.size() >= 3) roiZones_[ch].push_back(z);
    }

    for (int ch = 0; ch < 4; ++ch) refreshRoiZones(ch);
    qDebug() << "침대 ROI 복원 완료";
}

// 이벤트 배너에 쓸 "어디서 / 누가" 문구.
//
// 서버가 침대를 특정하지 못하면(roiId<0) 사람 대신 채널만 말한다.
// ★ "신원 미상"이라고 쓰지 않는다 — 요양원에서 그 표현은 "모르는 사람이 들어왔다"
//   (외부인 침입)로 읽힌다. 실제 의미는 "우리가 누군지 못 밝혔다"일 뿐이라
//   알 수 있는 사실(어느 채널)만 말하는 쪽이 정확하고 오해도 없다.
// ★ 반대로 추정한 이름을 억지로 붙이지도 않는다. 추적 ID는 신원이 아니라서
//   오귀속이 원리적으로 가능하고, 틀린 이름은 이름 없는 것보다 나쁘다
//   (엉뚱한 보호자에게 연락이 간다).
QString MainWindow::eventWhoLabel(int channel, int roiId) const
{
    if (roiId < 0) return QStringLiteral("%1 · 채널 %2").arg(currentRoomName()).arg(channel + 1);
    const QString name = zoneResidentName(channel, roiId);
    return name == QStringLiteral("미지정")
               ? QStringLiteral("침대 %1").arg(roiId + 1)
               : QStringLiteral("침대 %1 %2").arg(roiId + 1).arg(name);
}

// 이벤트 로그 '위치' 칸 문구 — 침대를 특정했으면 침대까지, 아니면 채널까지.
QString MainWindow::eventPlaceLabel(int channel, int roiId) const
{
    if (channel < 0 || channel >= 4) return QString();
    if (roiId < 0) return patients[channel].bed;
    return QStringLiteral("%1 · 침대 %2").arg(patients[channel].bed).arg(roiId + 1);
}

// 이 침대에 매핑된 입소자 이름. 미지정이면 "미지정".
QString MainWindow::zoneResidentName(int channel, int roiId) const
{
    if (channel < 0 || channel >= 4) return QStringLiteral("미지정");
    const int rid = roiResident_[channel].value(roiId, 0);
    if (rid <= 0) return QStringLiteral("미지정");
    auto it = residentInfo_.constFind(rid);
    return it == residentInfo_.constEnd() || it->name.isEmpty()
               ? QStringLiteral("입소자 %1").arg(rid)
               : it->name;
}

// 침대 목록(roiZones_/roiResident_)을 화면 전체에 다시 반영한다.
// 오버레이 이름표·4분할 화면·인스펙터 목록·채널 배지가 한 소스를 보게 하는 지점.
void MainWindow::refreshRoiZones(int channel)
{
    if (channel < 0 || channel >= 4) return;
    // 빈 방에는 침대가 없다. 막지 않으면 102호를 보는 중에 채널을 바꿀 때마다
    // 101호 침대 폴리곤이 설정 스테이지에 다시 그려진다.
    if (selectedRoom_ != 0) {
        if (channelViews[channel]) channelViews[channel]->setZones({});
        if (roiEditorView && roiEditChannel == channel) roiEditorView->setZones({});
        return;
    }

    // 이름표는 여기서 만든다 — 침대 번호만으로는 관제사가 누구 자리인지 모른다.
    QVector<RoiZone> withLabels = roiZones_[channel];
    for (auto& z : withLabels) {
        z.label = QStringLiteral("침대 %1 · %2")
                      .arg(z.id + 1)
                      .arg(zoneResidentName(channel, z.id));
    }

    if (channelViews[channel]) channelViews[channel]->setZones(withLabels);
    if (roiEditorView && roiEditChannel == channel) roiEditorView->setZones(withLabels);

    rebuildBedList();            // 인스펙터의 침대 목록(입소자 콤보 포함)
    refreshCamChannelStatus();   // 채널 타일 배지("침대 2")
}

void MainWindow::onRoiVisibilityToggled(bool on)
{
    for (auto* v : channelViews)
        if (v) v->setRoiVisible(on);
    if (roiEditorView) roiEditorView->setRoiVisible(on);
    if (roiToggleButton)
        roiToggleButton->setText(on ? QStringLiteral("영상에 표시")
                                    : QStringLiteral("표시 숨김"));
}

void MainWindow::onRoiCompleted(int channel, int roiId, const QPolygonF& normPts)
{
    if (channel < 0 || channel >= 4) return;

    sendRoi(channel, roiId, normPts);

    // 로컬 목록에도 반영 — 같은 id가 이미 있으면(다시 그린 것) 좌표만 갈아끼운다.
    bool replaced = false;
    for (auto& z : roiZones_[channel]) {
        if (z.id != roiId) continue;
        z.poly = normPts;
        replaced = true;
        break;
    }
    if (!replaced) {
        RoiZone z;
        z.id = roiId;
        z.poly = normPts;
        roiZones_[channel].push_back(z);
    }

    // 방금 그린 걸 볼 수 있도록 표시 토글이 꺼져 있으면 켠다
    if (roiToggleButton && !roiToggleButton->isChecked())
        roiToggleButton->setChecked(true);

    refreshRoiZones(channel);
    if (roiEditorView && roiEditChannel == channel)
        roiEditorView->setSelectedZone(roiId);   // 바로 입소자를 지정하도록 선택 유지

    qDebug() << "침대 ROI 전송: ch" << channel << "침대" << roiId
             << "," << normPts.size() << "점";
}

// 침대를 클릭해 편집 대상으로 고름 — 인스펙터 목록의 강조를 맞춘다.
void MainWindow::onRoiZoneSelected(int channel, int roiId)
{
    Q_UNUSED(channel);
    Q_UNUSED(roiId);
    rebuildBedList();
}

void MainWindow::sendRoi(int channel, int roiId, const QPolygonF& normPts, bool clear)
{
    // 이 채널을 담당하는 Pi로 전송 (패킷 조립은 StreamClient)
    if (!stream_->sendRoi(channel, roiId, normPts, clear))
        QMessageBox::warning(this, QStringLiteral("전송 실패"),
                             QStringLiteral("해당 채널의 영상 서버에 연결되어 있지 않습니다."));
}

// "이 침대는 이 사람 자리" 를 서버에 통보한다.
// 서버는 이 매핑을 roi_zones에 남기고, 그 침대에서 생긴 추적 객체에 입소자를
// 귀속시켜 낙상·이탈 알림에 이름을 붙인다(server/core/identity_tracker.hpp).
void MainWindow::sendRoiBind(int channel, int roiId, int residentId)
{
    if (!stream_->sendRoiBind(channel, roiId, residentId)) {
            QMessageBox::warning(this, QStringLiteral("전송 실패"),
                                 QStringLiteral("해당 채널의 영상 서버에 연결되어 있지 않습니다."));
        return;
    }
    qDebug() << "침대 매핑 전송: ch" << channel << "침대" << roiId
             << "→ 입소자" << residentId;
}
