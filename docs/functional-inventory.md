# 기능 인벤토리 — v2 전면 재작성 안전망

**작성:** 2026-08-15 · **근거:** `client/` 전 소스 직접 열람 + `connect(` 전수 grep
**목적:** UI를 밑바닥부터 다시 짜는 동안 **기능이 하나도 유실되지 않았음을 증명하는 체크리스트.** 이 프로젝트엔 UI 테스트가 없어서 `connect()` 하나가 빠지면 조용히 실패하고 사람이 그 버튼을 누를 때까지 아무도 모른다. 이 문서의 정확도가 유일한 안전망이다.

---

## 0. 이 조사가 뒤집은 두 가지

### 0.1 "기존 13개 기능"은 사실이 아니다 — 실제로는 **33개**

`REQUIREMENTS.md`의 VERIFY-01은 *"기존 13개 기능을 모두 직접 조작해 동작이 유지됨을 확인한다"*고 되어 있다. 코드에서 세어 보면 사용자가 실제로 클릭할 수 있는 개별 기능은 **33개**다. 13이라는 숫자의 근거는 코드 어디에도 없다 — 검증되지 않은 구전 수치다.

**함의:** Phase 5의 VERIFY-01을 그대로 수행했다면 **20개 기능이 확인 없이 지나갔을 것이다.** 아래 §1의 F-01~F-33이 실제 체크리스트다.

### 0.2 `connect()`는 75개가 아니라 **98개**

75는 `mainwindow.cpp` 한 파일 기준이었다. 전체는 다음과 같다.

| 파일 | 개수 |
|---|---|
| `mainwindow.cpp` | 75 |
| `signupdialog.cpp` | 11 |
| `logindialog.cpp` | 6 |
| `mqttqtmanager.cpp` | 5 (grep은 6이지만 `:89`의 `m_client->disconnect(this)`는 false positive) |
| `videoview.cpp` | 1 |
| `clickslider.cpp` / `alertbanner.cpp` / `vitaltile.cpp` | 0 (신호·슬롯 없는 순수 표시 위젯) |
| **합계** | **98** |

---

## 1. 기능 체크리스트 (F-01 ~ F-33)

재작성 후 이 33개를 전부 직접 클릭해 동작을 확인한다.

| ID | 기능 | 근거 | 화면 |
|---|---|---|---|
| F-01 | 로그인 | `logindialog.cpp:194` | LoginDialog |
| F-02 | 회원가입 | `signupdialog.cpp:258` | SignupDialog |
| F-03 | 로그아웃(재로그인 루프) | `mainwindow.cpp:938`, `main.cpp:83-99` | 헤더 |
| F-04 | 4채널 실시간 영상 관제(2×2) | `mainwindow.cpp:1210-1241` | 관제(0) |
| F-05 | 채널 스포트라이트(감지 채널 자동 확대) | `mainwindow.cpp:1243~` | 관제(0) |
| F-06 | 침대 ROI 그리기·제거·표시토글 | `mainwindow.cpp:4600-4650`, `videoview.cpp:407-465` | 장치설정›카메라›ROI |
| F-07 | 침대–입소자 매핑 | `mainwindow.cpp:4700-4726` | 장치설정›카메라›ROI |
| F-08 | 방송(누르는 동안만) | `mainwindow.cpp:840-841` | 헤더 |
| F-09 | 경보 배너 표시 및 해제 | `mainwindow.cpp:950-1035`, `972` | 관제(0) |
| F-10 | 경보 시 화면 테두리 펄스 | `mainwindow.cpp:551-568`, `5240-5244` | 전역 오버레이 |
| F-11 | 상시 경보 요약 배너(다건 승격) | `alertbanner.cpp:45-118`, `mainwindow.cpp:5237` | 관제(0) 상단 |
| F-12 | 바이탈 실시간 모니터링 | `vitaltile.cpp`, `mainwindow.cpp:420` | 관제(0) 우측 |
| F-13 | 이벤트 기록 조회 | `mainwindow.cpp:1719-1742` | 이벤트기록(1) |
| F-14 | 이벤트 필터(날짜·병실·유형) | `mainwindow.cpp:1680-1716` | 이벤트기록(1) |
| F-15 | 블랙박스 과거 영상 인라인 재생 | `mainwindow.cpp:1744-1860` | 이벤트기록(1) |
| F-16 | 일일 리포트(달력+지표4+24h 그래프) | `mainwindow.cpp:1490-1594` | 리포트(2) |
| F-17 | 입소자 검색·상태필터(재원/전체/퇴원) | `mainwindow.cpp:1990-2025`, `2166-2261` | 입소자(3) |
| F-18 | 입소자 신규 등록 | `mainwindow.cpp:2019` | 입소자(3) |
| F-19 | 입소자 저장(수정) | `mainwindow.cpp:2127` | 입소자(3) |
| F-20 | 퇴원 처리 | `mainwindow.cpp:2131-2132` | 입소자(3) |
| F-21 | 재입소 처리 | `mainwindow.cpp:2133` | 입소자(3) |
| F-22 | 입퇴원 이력·변경내역 조회 | `mainwindow.cpp:2470-2611` | 입소자(3) |
| F-23 | 카메라 연결(IP/계정/비번) | `mainwindow.cpp:4400-4453` | 장치설정›카메라›연결 |
| F-24 | 카메라 검색(ONVIF WS-Discovery) | `mainwindow.cpp:4437-4540` | 장치설정›카메라›연결 |
| F-25 | 카메라 해제 | `mainwindow.cpp:4445-4448` | 장치설정›카메라›연결 |
| F-26 | 이미지 파라미터(밝기·대비·채도) | `mainwindow.cpp:3960-4001` | 장치설정›카메라›이미지 |
| F-27 | 클릭 지점 자동초점 | `mainwindow.cpp:3972-3984` | 장치설정›카메라›이미지 |
| F-28 | 카메라 채널 선택(썸네일 스트립) | `mainwindow.cpp:4287-4345` | 장치설정›카메라 |
| F-29 | 알림 노드 설정(밝기·음량, 테스트·적용) | `mainwindow.cpp:4091-4258` | 장치설정›알림 |
| F-30 | 알림 노드 온라인 상태 표시 | `mainwindow.cpp:434` | 장치설정›알림, 헤더 |
| F-31 | 테마 토글 | `mainwindow.cpp:911`, `thememanager.cpp` | 헤더 |
| F-32 | 도움말 | `mainwindow.cpp:903, 1040-1088` | 헤더→helpDialog |
| F-33 | 좌측 네비 접기·펼치기 | `mainwindow.cpp:676-713` | 좌측 네비 |

---

## 2. 화면 구조

### 2.1 `contentStack` — 좌측 네비로 전환되는 5개 페이지

| idx | 화면 | 빌드 | 구성 |
|---|---|---|---|
| 0 | 실시간 관제 | `dashboardTab`(인라인) | `alertBanner_` + `buildVideoWall()` + `buildVitalsPanel()` |
| 1 | 이벤트 기록 | `buildEventLogTab()` | 필터바 + 로그표 + 블랙박스 인라인 플레이어 |
| 2 | 일일 리포트 | `buildReportPage()` | 좌 달력 / 우 입소자탭+지표+그래프 |
| 3 | 입소자 관리 | `buildDbTab()` | 좌 카드목록 / 우 인라인 편집기(`residentDetailStack`) |
| 4 | 장치 설정 | `buildDeviceSettingsTab()` | `deviceStack_`(카메라/알림) → 카메라 안에 또 `camControlStack`(연결/ROI/이미지) |

### 2.2 진짜 `QDialog`는 4개뿐

`LoginDialog` · `SignupDialog` · `helpDialog`(`mainwindow.cpp:1049`) · 입퇴원 변경내역 팝업(`mainwindow.cpp:2556`).

⚠️ **카메라 설정·ROI 편집은 코드 주석이 "팝업"이라 부르지만 실제로는 `QDialog`가 아니다.** `contentStack` 4번 페이지 안에 중첩된 `QStackedWidget`이다. 재작성 때 "팝업이니까 별도 창으로 빼자"고 판단하면 동작이 달라진다.

---

## 3. 재작성이 조용히 깨뜨리기 쉬운 것 (최우선 주의)

| # | 항목 | 위치 | 무엇이 깨지는가 |
|---|---|---|---|
| H-1 | **`ClickSlider::kValueMargin`** | `clickslider.h:17`, `clickslider.cpp:16-38`(QSS 생성), `:44`(클릭 비율), `:61`(그리기) | 이 상수를 QSS로 옮기면 클릭 좌표와 그려지는 숫자 위치가 어긋난다. 이미 알려진 사례 |
| H-2 | **`VideoView::zoneColor()` 8색 배열 ↔ 침대 배지 인라인 스타일** | `videoview.cpp:16-25,32-35` ↔ `mainwindow.cpp:4691-4697` | 목록 배지 색과 영상 위 침대 오버레이 색이 **같은 출처를 공유해야 한다.** QSS로 분리하면 어긋난다. 코드 주석이 "clickslider와 같은 이유로 의도적 잔류"라 명시 |
| H-3 | **`kVideoSurface` 고정 흑색** | `theme.h:59-64`, `videoview.cpp:224-238`, `:499-500` | 두 개의 다른 `paintEvent()`가 반드시 같은 `QColor`를 공유해야 한다는 명시적 설계 제약 |
| H-4 | **전역 가변 팔레트 포인터** (`kTextMain`, `kAccent` …) | `theme.h:44-71`, 사용례 `mainwindow.cpp:4695-4697` | 위젯 생성 시점에 `QString::fromLatin1(kAccent)`로 색을 "구워 넣는" 코드가 다수. 지금은 대부분 재생성 패턴으로 우회 중인데, **재작성 때 위젯 캐싱·재사용 최적화를 넣으면 테마 토글이 안 먹는 위젯이 생긴다** |
| H-5 | **위험도 이중 부호화** `riskBar`(색) + `riskChip`(색+도형) | `mainwindow.cpp:2223-2229`, `2145-2161`, 주석 `:2224` | 둘 중 하나만 남기면 색맹 사용자에게 위험도 구분이 사라진다. 주석이 직접 "이 짝짓기를 깨지 말 것(D-09)"이라 경고 |
| H-6 | **레이아웃 밖 절대좌표 오버레이 2개** | `AlarmOverlay`(`mainwindow.cpp:551-568`, `794-796`, `810`, `5240-5244`) · `alarmBanner_` 토스트(`:950-1035`) | `ui->centralwidget` 위에 얹혀 `setGeometry()`/`move()`/`raise()`로 수동 위치 추적. `resizeEvent()`(`:806-816`)가 재계산 담당 — **레이아웃 트리가 바뀌면 이 절대좌표 계산이 조용히 깨진다** |
| H-7 | **MQTT TLS 인증서 CN 검증** | `mqttqtmanager.cpp:166` 인라인 람다 | 보안 로직이 람다 안에 숨어 있어 파일을 나누다 놓치기 쉽다 |
| H-8 | **`FlowLayout` 커스텀 레이아웃** | `mainwindow.cpp:95-128` | 입소자 카드 그리드용. 정식 `QLayout` API를 따르므로 비교적 안전하지만 **클래스 자체를 누락하지 말 것** |
| H-9 | **동적 property + `unpolish`/`polish` 관용구** | `alertbanner.cpp:113-117`, `vitaltile.cpp:110-172`, `signupdialog.cpp:60-73` | 순서를 지키지 않거나 repolish를 빠뜨리면 값은 바뀌었는데 색만 안 바뀐다. 주석들이 "생성 직후 속성 설정만으로 충분"이라 말하는 건 **화면에 붙기 전 위젯이라는 타이밍 전제** — 재사용 위젯으로 바꾸면 이 전제가 깨진다 |

**`parentWidget()` 연쇄 탐색과 `mapTo`/`mapFrom`은 코드베이스에 없다** — 그건 안심해도 된다.

---

## 4. 크로스커팅 런타임 배선

### 4.1 네트워크

| 항목 | 값 / 위치 |
|---|---|
| TCP 영상 소켓 2개 | `kNumServers=2`(`mainwindow.h:238`), `kServerPort=5500`(`mainwindow.cpp:291`), 생성 `:402-406` |
| 재접속 타이머 | `kReconnectDelayMs=3000`(`mainwindow.cpp:292`) |
| 블랙박스 HTTP | `kClipHttpPort=5501`(`:295`), `/list` 조회 `:463-512` |
| MQTT 구독 | `subscribeAll()`(`mqttqtmanager.cpp:345-364`) — clean session이라 **재연결마다 매번 재구독 필요** |
| 카메라 검색 UDP | `discoverySocket`(`mainwindow.cpp:4492-4494`), ONVIF WS-Discovery |

### 4.2 타이머

| 타이머 | 주기 | 용도 |
|---|---|---|
| `clockTimer` | 1000ms | 헤더 시계 |
| `vitalsTimer` | 2000ms | 바이탈 갱신 |
| `careTimeTimer` | 10000ms | 케어시간·리포트 지표 |
| `reconnectTimer` | 3000ms(singleShot) | 영상 서버 재접속 |
| `MqttQtManager::m_retryTimer` | 5000ms | MQTT 재연결 |
| `AlarmOverlay::timer_` | 33ms | 경보 테두리 숨쉬기 |
| `VideoView::alertTimer_` | 450ms | 경보 마커 점멸 |

⚠️ **"watchdog" 타이머는 존재하지 않는다** — 다른 문서가 언급했지만 grep 결과 없음. 위 7개가 전부.

---

## 5. `connect()` 전수 인벤토리 (C-001 ~ C-098)

### 5.1 `mainwindow.cpp` — 75건

| ID | line | 소스 → 시그널 → 수신 | 사용자가 관찰하는 것 |
|---|---|---|---|
| C-001 | 404 | `sockets[i]` `readyRead` → `onReadyRead` | 서버가 프레임을 보내면 해당 채널에 그려진다 |
| C-002 | 405 | `sockets[i]` `stateChanged` → `onSocketStateChanged` | 연결이 끊기거나 붙으면 상태 배지·"미연결" 표시가 바뀐다 |
| C-003 | 412 | `reconnectTimer` `timeout` → `connectToServer` | 끊긴 뒤 3초 후 자동 재접속 |
| C-004 | 416 | `clockTimer` `timeout` → `updateClock` | 헤더 시계가 매초 갱신 |
| C-005 | 420 | `vitalsTimer` `timeout` → `updateVitals` | 2초마다 바이탈 수치 갱신 |
| C-006 | 429 | `mqtt` `wearableDataReceived` → `onWearableData` | 웨어러블 신호 도착 시 바이탈·낙상 상태 갱신 |
| C-007 | 430 | `mqtt` `alarmCommandReceived` → `onMqttAlarm` | 알림 노드로 나간 경보 명령을 엿들어 화면 반영 |
| C-008 | 431 | `mqtt` `connected` → `onMqttConnected` | MQTT 연결됨 반영 |
| C-009 | 432 | `mqtt` `disconnected` → `onMqttDisconnected` | MQTT 끊김 반영 |
| C-010 | 433 | `mqtt` `connectionError` → `onMqttError` | MQTT 오류 표시 |
| C-011 | 434 | `mqtt` `nodeOnlineChanged` → `onAlarmNodeStatus` | 알림 노드 온·오프라인 배지 갱신 |
| C-012 | 435 | `mqtt` `payloadRejected` → 람다 | 콘솔 로그만(화면 영향 없음) |
| C-013 | 459 | `careTimeTimer` `timeout` → `updateCareTime` | 10초마다 케어시간·리포트 갱신 |
| C-014 | 472 | `QNetworkReply` `finished` → 람다 | 과거 클립 목록 도착 시 이벤트 표에 채워짐 |
| C-015 | 556 | `AlarmOverlay::timer_` `timeout` → 람다 | 경보 중 가장자리 빨강 글로우 점멸 |
| C-016 | 680 | `navToggle` `clicked` → 람다 | ☰ 누르면 좌측 네비 접힘·펼침 |
| C-017 | 703 | `navBtns[i]` `clicked` → 람다 | 네비 클릭 시 화면 전환 |
| C-018 | 840 | `micButton` `pressed` → `onMicPressed` | 🎤 누르는 순간 방송 시작 |
| C-019 | 841 | `micButton` `released` → `onMicReleased` | 손 떼면 방송 종료 |
| C-020 | 903 | `helpButton` `clicked` → `onHelpClicked` | 도움말 창 열림 |
| C-021 | 911 | `themeToggleButton` `clicked` → `toggleTheme` | 라이트·다크 전환 |
| C-022 | 938 | `logoutButton` `clicked` → `onLogoutClicked` | 확인 후 로그인 화면으로 |
| C-023 | 972 | `alarmClearButton` `clicked` → `onAlarmClearClicked` | 사이렌·LED 꺼지고 배너 사라짐 |
| C-024 | 1080 | `helpList` `currentRowChanged` → `renderHelpTopic` | 목록 클릭 시 우측 설명 전환 |
| C-025 | 1230 | `VideoView` `roiCompleted` → `onRoiCompleted` | 그리기 완료 시 침대 등록·목록 반영 |
| C-026 | 1231 | `VideoView` `drawModeChanged` → 람다 | "침대 추가"/"취소" 문구 전환 |
| C-027 | 1515 | `today` `clicked` → 람다 | "오늘" 클릭 시 달력 이동 |
| C-028 | 1523 | `reportCalendar` `selectionChanged` → 람다 | 날짜 클릭 시 지표·그래프 갱신 |
| C-029 | 1621 | 입소자 탭 `clicked` → 람다 | 이름 탭 클릭 시 해당 입소자 지표로 전환 |
| C-030 | 1690 | `filterDateFrom` `dateChanged` → 람다 | 시작 날짜 변경 즉시 재필터링 |
| C-031 | 1692 | `filterDateTo` `dateChanged` → 람다 | 종료 날짜 변경 즉시 재필터링 |
| C-032 | 1704 | `filterEventType` `currentTextChanged` → 람다 | 유형 선택 시 즉시 재필터링 |
| C-033 | 1739 | `logTable` `cellDoubleClicked` → `onLogRowActivated` | 더블클릭 시 블랙박스 재생 + "확인" 전환 |
| C-034 | 1802 | `blackboxPlayPauseButton` `clicked` → 람다 | 재생·일시정지 토글 |
| C-035 | 1808 | `blackboxPlayer` `playbackStateChanged` → 람다 | 상태에 맞춰 버튼 아이콘 전환 |
| C-036 | 1816 | `blackboxPlayer` `durationChanged` → 람다 | 길이 확정 시 탐색바 범위·총 시간 채워짐 |
| C-037 | 1823 | `blackboxPlayer` `positionChanged` → 람다 | 진행에 따라 탐색바·시간 이동 |
| C-038 | 1832 | `blackboxSeek` `sliderPressed` → 람다 | 누르면 그 지점으로 즉시 점프 |
| C-039 | 1836 | `blackboxSeek` `sliderReleased` → 람다 | 떼면 그 위치로 재생 확정 |
| C-040 | 1840 | `blackboxSeek` `sliderMoved` → 람다 | 드래그 중 시간 표시 갱신 |
| C-041 | 1845 | `blackboxPlayer` `errorOccurred` → 람다 | 저장 미완료 클립은 "저장 중…" 후 자동 재시도 |
| C-042 | 1998 | `residentFilterBtns[i]` `clicked` → 람다 | 재원·전체·퇴원 탭 필터링 |
| C-043 | 2010 | `residentSearchEdit` `textChanged` → 람다 | 입력 즉시 이름 기준 필터링 |
| C-044 | 2019 | `newBtn` `clicked` → 람다 | 빈 등록 폼 열림 |
| C-045 | 2127 | `saveBtn` `clicked` → `onSaveResident` | 저장 시 DB 반영 + 목록 갱신 |
| C-046 | 2131 | `dlgDischargeBtn` `clicked` → 람다 | 재원자는 퇴원, 퇴원자는 재입소(상태 의존) |
| C-047 | 2217 | 목록 행 `clicked` → 람다 | 우측에 해당 입소자 편집 폼 열림 |
| C-048 | 2486 | `admissionTable` `cellDoubleClicked` → `onAdmissionRowActivated` | 변경내역 팝업 |
| C-049 | 2607 | `closeBox` `rejected` → `QDialog::close` | 팝업 닫힘 |
| C-050 | 3982 | `afBtn` `clicked` → 람다 | "전체 자동초점" 실행 |
| C-051 | 3985 | 이미지 `apply` `clicked` → 람다 | 밝기·대비·채도 전송 + 전/후 프리뷰 갱신 |
| C-052 | 3993 | 이미지 `reset` `clicked` → 람다 | 슬라이더 50으로 리셋 |
| C-053 | 4080 | `deviceModeBtns_[i]` `clicked` → 람다 | [카메라]/[알림] 전환 |
| C-054 | 4217 | `alertBright_` `valueChanged` → 람다 | 미리보기 LED 밝기 즉시 변화(전송 전) |
| C-055 | 4231 | `alertNode_` `currentTextChanged` → 람다 | 노드 변경 시 저장값·온라인 배지 로드 |
| C-056 | 4235 | `testBtn` `clicked` → 람다 | 실제 노드에서 소리·LED 잠깐 동작 |
| C-057 | 4243 | `applyBtn` `clicked` → 람다 | 설정 전송 + "마지막 적용" 갱신 |
| C-058 | 4279 | `camModeBtns[i]` `clicked` → 람다 | [연결]/[ROI]/[이미지] 전환 |
| C-059 | 4339 | `camChannelBtns[i]` `clicked` → 람다 | 채널 썸네일 클릭 시 편집 대상 전환 |
| C-060 | 4391 | `camControlStack` `currentChanged` → 람다 | 페이지 전환 시 카드 높이 보정 |
| C-061 | 4440 | `searchCameraButton` `clicked` → `onSearchCameraClicked` | 탐색 시작, 결과가 표에 나타남 |
| C-062 | 4444 | `addCameraButton` `clicked` → `onAddCameraClicked` | 입력 정보로 카메라 연결 시도 |
| C-063 | 4448 | `clearCameraButton` `clicked` → `onCameraClearClicked` | 해당 채널 연결 해제 |
| C-064 | 4483 | `discoveryTable` `cellClicked` → 람다 | 행 클릭 시 IP 자동 채움 |
| C-065 | 4484 | `discoveryTable` `cellDoubleClicked` → 람다 | 더블클릭도 동일 |
| C-066 | 4494 | `discoverySocket` `readyRead` → 람다 | 카메라 응답 도착 시 결과 표에 행 추가 |
| C-067 | 4530 | `arp`(QProcess) `finished` → 람다 | MAC 조회 완료 시 표의 MAC·ID 채워짐 |
| C-068 | 4609 | `roiButton` `clicked` → `onRoiButtonClicked` | 그리기 시작·취소 |
| C-069 | 4616 | `roiClearButton` `clicked` → `onRoiClearClicked` | 선택(또는 전체) 침대 삭제 |
| C-070 | 4622 | `roiToggleButton` `toggled` → `onRoiVisibilityToggled` | 영상 위 침대 오버레이 표시·숨김 |
| C-071 | 4719 | 침대목록 콤보 `currentIndexChanged` → 람다 | 입소자 선택 시 침대 배정 + 서버 전송 |
| C-072 | 4733 | `del` `clicked` → 람다 | 해당 침대 선택 후 제거 |
| C-073 | 4757 | `roiEditorView` `roiCompleted` → `onRoiCompleted` | 편집 스테이지에서 그리기 완료 시 등록 |
| C-074 | 4758 | `roiEditorView` `drawModeChanged` → 람다 | 버튼 문구·강조 전환 |
| C-075 | 4769 | `roiEditorView` `zoneSelected` → `onRoiZoneSelected` | 영상에서 침대 클릭 시 우측 목록 강조 |

### 5.2 `logindialog.cpp` — 6건

| ID | line | 배선 | 관찰 |
|---|---|---|---|
| C-076 | 160 | `loginButton_` `clicked` → `attemptLogin` | 검증 후 성공 시 관제 화면 전환 |
| C-077 | 161 | `signupButton` `clicked` → `openSignup` | 회원가입 창 열림 |
| C-078 | 163 | `idEdit_` `textEdited` → `clearError` | 재입력 시작 시 오류 메시지 사라짐 |
| C-079 | 164 | `pwEdit_` `textEdited` → `clearError` | 동일 |
| C-080 | 166 | `idEdit_` `returnPressed` → 람다 | Enter 시 비밀번호 칸으로 포커스 |
| C-081 | 167 | `pwEdit_` `returnPressed` → `attemptLogin` | Enter 시 로그인 시도 |

### 5.3 `signupdialog.cpp` — 11건

| ID | line | 배선 | 관찰 |
|---|---|---|---|
| C-082 | 150 | `submitButton_` `clicked` → `attemptSignup` | 검증 후 계정 생성 |
| C-083 | 151 | `cancelButton` `clicked` → `reject` | 창 닫힘 |
| C-084 | 156 | `idEdit_` `textEdited` → 람다 | 입력 중 형식 오류 즉시 힌트 |
| C-085 | 163 | `idEdit_` `editingFinished` → `checkLoginId` | 포커스 아웃 시 DB 중복 확인 |
| C-086 | 165 | `nameEdit_` `textEdited` → 람다 | 이름 형식 오류 힌트 |
| C-087 | 172 | `pwEdit_` `textEdited` → 람다 | 비번 오류 힌트 + 확인란 재검사 |
| C-088 | 180 | `pwConfirm_` `textEdited` → 람다 | 일치 여부 즉시 표시 |
| C-089 | 187 | `idEdit_` `returnPressed` → 람다 | 이름 칸으로 이동 |
| C-090 | 188 | `nameEdit_` `returnPressed` → 람다 | 비밀번호 칸으로 이동 |
| C-091 | 189 | `pwEdit_` `returnPressed` → 람다 | 확인 칸으로 이동 |
| C-092 | 190 | `pwConfirm_` `returnPressed` → `attemptSignup` | Enter 시 가입 시도 |

### 5.4 `videoview.cpp` — 1건

| ID | line | 배선 | 관찰 |
|---|---|---|---|
| C-093 | 181 | `alertTimer_` `timeout` → 람다 | 경보 중 영상 카드 빨간 테두리가 0.45초 간격 점멸 |

### 5.5 `mqttqtmanager.cpp` — 5건

| ID | line | 배선 | 관찰 |
|---|---|---|---|
| C-094 | 60 | `m_client` `stateChanged` → `onStateChanged` | 구독 재등록·재연결 타이머 제어 |
| C-095 | 61 | `m_client` `errorChanged` → `onErrorChanged` | 오류 문구 상위 전달 |
| C-096 | 62 | `m_client` `messageReceived` → `onMessageReceived` | 웨어러블·알람·노드상태로 해석되어 각 시그널로 전달 |
| C-097 | 65 | `m_retryTimer` `timeout` → 람다 | 끊김 5초 후 자동 재접속 |
| C-098 | 166 | `QSslSocket` `sslErrors` → 람다 | **TLS 인증서 CN 검증** — IP-이름 불일치를 조건부 무시. 보안 로직 |

---

## 6. 이 조사가 다루지 않은 것

`activitychart.cpp` · `sparkline.cpp` · `alertmatrixpreview.cpp` · `thememanager.cpp` · `wintheme.cpp` · `auth.cpp`는 `connect()` 전수 조사에서 제외했다. 이 중 **`thememanager.cpp`는 §3의 H-4(전역 가변 팔레트)와 직접 연관**되므로 재작성 착수 전 별도로 훑을 것.

---

*이 문서는 v2 전면 재작성의 안전망이다. 재작성 후 §1의 F-01~F-33을 전부 클릭하고, §3의 H-1~H-9가 살아 있는지 확인한다.*
