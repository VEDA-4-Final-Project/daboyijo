// MainWindow — 도움말 창
// (mainwindow.cpp를 기능별로 나눈 파일 중 하나. 공용 헬퍼는 mainwindow_p.h)
#include "mainwindow_p.h"

// ═══════════════════════════════════════════════════════════
//  도움말 — 앱의 모든 기능을 설명하는 창
//
//  내용은 아래 helpTopics()에 데이터로만 모아 둔다. 기능이 바뀌면 이 목록만
//  고치면 되고, 좌측 목록 구성과 본문 렌더링(renderHelpTopic)은 그대로 둬도
//  된다 — 예전엔 switch 안에 HTML이 통째로 박혀 있어 항목 하나 추가하는 데도
//  렌더링 코드를 건드려야 했다.
// ═══════════════════════════════════════════════════════════
namespace {

// 설명 한 줄 — "용어 + 설명" 카드 하나가 된다.
struct HelpEntry {
    QString term;
    QString desc;
    bool isNew = false;      // 최근에 붙은 기능 — 카드에 NEW 배지를 단다
};
// 한 주제 안의 소제목 묶음.
struct HelpGroup {
    QString heading;         // 비우면 소제목 없이 카드만 이어 붙인다
    QVector<HelpEntry> entries;
};
struct HelpTopic {
    QString section;         // 좌측 목록의 구분 머리글(같은 값이 이어지면 한 묶음)
    QString icon;
    QString title;
    QString summary;         // 제목 아래 한 줄 요약
    QVector<HelpGroup> groups;
    QString tip;             // 하단 팁(비우면 생략)
};

const QVector<HelpTopic>& helpTopics()
{
    static const QVector<HelpTopic> topics = {
    // ── 시작 ────────────────────────────────────────────────
    {
        QStringLiteral("시작"), QStringLiteral("🏠"),
        QStringLiteral("Carenet이란"),
        QStringLiteral("Carenet은 요양원의 낙상과 침상이탈을 실시간으로 감지하고, 그 순간의 "
                       "영상과 기록을 남기는 통합 관제 프로그램입니다."),
        {
          { QStringLiteral("무엇을 하나요"), {
              { QStringLiteral("실시간 감지"),
                QStringLiteral("4채널 CCTV 영상을 서버가 계속 분석해 낙상·침상이탈을 판정하고, "
                               "발생 즉시 관제 화면과 현장 알림 노드(LED·스피커)에 알립니다.") },
              { QStringLiteral("자동 기록"),
                QStringLiteral("모든 채널을 연속 녹화하고, 이벤트가 나면 그 구간을 클립으로 따로 "
                               "남깁니다. 나중에 시각으로도, 말로 물어서도 찾을 수 있습니다.") },
              { QStringLiteral("생체신호"),
                QStringLiteral("웨어러블에서 올라오는 산소포화도·심박을 채널별로 함께 봅니다.") },
              { QStringLiteral("사람 단위 정리"),
                QStringLiteral("침대마다 입소자를 지정해 두면 알림과 기록에 이름이 붙고, "
                               "입소자별 일일 리포트로 쌓입니다.") },
          } },
          { QStringLiteral("화면 이동"), {
              { QStringLiteral("왼쪽 메뉴 6개"),
                QStringLiteral("실시간 관제 · 이벤트 기록 · 영상 검색 · 일일 리포트 · 입소자 관리 · "
                               "장치 설정. 메뉴 위 ☰ 버튼으로 접었다 펼 수 있습니다.") },
              { QStringLiteral("경보가 오면"),
                QStringLiteral("화면 가장자리에 빨간 글로우가 은은하게 켜지고, 위에서 경보 카드가 "
                               "내려오며, 해당 영상 타일이 빨간 테두리로 강조됩니다. 경보 카드의 "
                               "‘경보 해제’를 누르면 현장 사이렌·LED까지 함께 꺼집니다.") },
          } },
        },
        QStringLiteral("처음 설치했다면 <b>장치 설정 → 카메라</b>에서 CCTV를 연결하고, "
                       "<b>ROI(침대)</b>에서 침대 영역을 그리는 것부터 하세요. "
                       "침대가 없으면 낙상·침상이탈 판정이 시작되지 않습니다."),
    },
    {
        QStringLiteral("시작"), QStringLiteral("🧭"),
        QStringLiteral("화면 둘러보기"),
        QStringLiteral("어느 페이지에서나 늘 보이는 상단 헤더와 왼쪽 메뉴입니다."),
        {
          { QStringLiteral("상단 헤더"), {
              { QStringLiteral("🎤 방송"),
                QStringLiteral("한 번 누르면 방송이 시작되고, 다시 누르면 끝납니다. "
                               "현장 스피커로 목소리가 나갑니다."), true },
              { QStringLiteral("실시간 시계"),
                QStringLiteral("관제 기록의 기준이 되는 현재 시각입니다.") },
              { QStringLiteral("도움말"),
                QStringLiteral("지금 보고 있는 이 창을 엽니다.") },
              { QStringLiteral("🌙 / ☀ 테마"),
                QStringLiteral("다크(야간 관제)와 라이트(주간)를 전환합니다. 이 창도 함께 바뀝니다.") },
              { QStringLiteral("계정 · 로그아웃"),
                QStringLiteral("로그인한 사람을 보여 줍니다. 로그아웃하면 관제 화면이 닫히고 "
                               "로그인 화면으로 돌아갑니다.") },
          } },
          { QStringLiteral("경보 카드"), {
              { QStringLiteral("평상시엔 안 보입니다"),
                QStringLiteral("경보가 났을 때만 화면 위에서 내려옵니다. ‘경보 해제’ 버튼도 헤더가 "
                               "아니라 이 카드 안에 있습니다."), true },
          } },
          { QStringLiteral("왼쪽 메뉴"), {
              { QStringLiteral("☰ 접기"),
                QStringLiteral("메뉴를 아이콘만 남기고 접습니다. 영상을 넓게 볼 때 씁니다. "
                               "접힌 상태에서도 아이콘에 마우스를 올리면 이름이 뜹니다.") },
          } },
        },
        QString(),
    },

    // ── 관제 ────────────────────────────────────────────────
    {
        QStringLiteral("관제"), QStringLiteral("📺"),
        QStringLiteral("실시간 관제"),
        QStringLiteral("4채널 영상을 보면서 경보를 받고, 필요하면 그 자리에서 과거 녹화로 되돌려 "
                       "보는 기본 화면입니다."),
        {
          { QStringLiteral("왼쪽 · 리소스 패널"), {
              { QStringLiteral("카메라 · 입소자 검색"),
                QStringLiteral("채널 이름이나 입소자 이름으로 걸러 찾습니다.") },
              { QStringLiteral("수신 상태"),
                QStringLiteral("채널마다 ‘수신 중 / 신호 없음’이 표시됩니다.") },
              { QStringLiteral("접기"),
                QStringLiteral("패널을 접으면 영상 영역이 그만큼 넓어집니다.") },
          } },
          { QStringLiteral("가운데 · 영상"), {
              { QStringLiteral("레이아웃 프리셋"),
                QStringLiteral("2×2 전체 / 스포트라이트(하나를 크게 + 나머지는 작게) / 단일 채널 중에 "
                               "고릅니다."), true },
              { QStringLiteral("경보 강조"),
                QStringLiteral("낙상·침상이탈이 난 채널의 타일이 빨간 테두리로 바뀝니다.") },
              { QStringLiteral("타일 제거"),
                QStringLiteral("타일을 레이아웃에서 빼도 카메라는 그대로 녹화합니다. "
                               "‘타일 모두 표시’로 되돌립니다.") },
              { QStringLiteral("스냅샷 저장"),
                QStringLiteral("지금 화면을 PNG 이미지로 저장합니다."), true },
          } },
          { QStringLiteral("아래 · 타임라인"), {
              { QStringLiteral("라이브 ↔ 녹화 재생"),
                QStringLiteral("‘녹화 재생’으로 바꾸고 타임라인에서 시각을 고르면, 그 시각의 녹화가 "
                               "같은 자리에서 재생됩니다. ‘라이브’로 언제든 돌아옵니다."), true },
              { QStringLiteral("타임라인 읽는 법"),
                QStringLiteral("선택한 채널의 녹화가 있는 구간과 그 채널에서 난 이벤트가 함께 그려집니다. "
                               "4채널을 겹쳐 그리지 않으므로 채널을 바꾸면 타임라인도 바뀝니다.") },
              { QStringLiteral("탐색"),
                QStringLiteral("막대를 끄는 동안에는 화면이 따라오지 않고, 놓는 순간 그 시각으로 "
                               "이동합니다. 끄는 내내 새 구간을 여는 건 서버에 무리라서 그렇습니다.") },
          } },
          { QStringLiteral("오른쪽 · 웨어러블"), {
              { QStringLiteral("생체신호"),
                QStringLiteral("채널별 산소포화도·심박과 심박 추세 그래프. 정상·주의·위험에 따라 색이 "
                               "바뀌고, 신호가 끊기거나 미착용이면 회색으로 표시됩니다.") },
              { QStringLiteral("미배정"),
                QStringLiteral("그 채널에 입소자가 지정되지 않았다는 뜻입니다. "
                               "장치 설정 → 카메라 → 침대·입소자 매핑에서 지정하세요.") },
          } },
        },
        QString(),
    },
    {
        QStringLiteral("관제"), QStringLiteral("📋"),
        QStringLiteral("이벤트 기록"),
        QStringLiteral("지금까지 난 낙상·침상이탈·생체신호 이상을 조건으로 좁혀 보고, "
                       "그 순간의 영상을 바로 재생합니다."),
        {
          { QStringLiteral("검색 조건"), {
              { QStringLiteral("즉시 반영"),
                QStringLiteral("조건을 바꾸면 바로 목록이 갱신됩니다. 따로 검색 버튼을 누르지 않습니다.") },
              { QStringLiteral("좁히는 기준"),
                QStringLiteral("병실 · 채널 · 이벤트 종류(낙상 / 침상이탈 / 생체신호 이상) · "
                               "확인 여부(전체 / 미확인만 / 확인만)."), true },
              { QStringLiteral("기간"),
                QStringLiteral("오늘 · 7일 · 30일 버튼으로 빠르게 잡거나, 시작일·종료일을 직접 고릅니다.") },
              { QStringLiteral("조건 초기화 · 새로고침"),
                QStringLiteral("초기화는 조건을 기본값으로, 새로고침은 서버 원장에서 다시 읽어 옵니다.") },
          } },
          { QStringLiteral("목록"), {
              { QStringLiteral("표에 나오는 것"),
                QStringLiteral("발생시각 · 종류 · 위치 · 입소자 · 출처 · 상태. 출처는 그 이벤트를 무엇이 "
                               "잡았는지(카메라 / 웨어러블)를 뜻합니다."), true },
              { QStringLiteral("색"),
                QStringLiteral("낙상은 빨강, 침상이탈은 주황. 상태는 미확인이 빨강, 확인이 초록입니다.") },
              { QStringLiteral("입소자가 ‘신원 미상’이면"),
                QStringLiteral("그 침대에 입소자가 지정되지 않았거나, 서버가 사람을 특정하지 못한 "
                               "경우입니다. 추적 번호는 신원이 아니라서 이름을 확정할 수 없습니다.") },
              { QStringLiteral("행이 잘려 보이면"),
                QStringLiteral("한 번에 표시하는 행 수에 상한이 있습니다. 안내가 뜨면 기간을 좀 더 "
                               "좁혀 주세요.") },
          } },
          { QStringLiteral("영상 확인"), {
              { QStringLiteral("더블클릭 → 재생"),
                QStringLiteral("행을 더블클릭하면 오른쪽 플레이어에서 그 시점 영상이 재생되고, "
                               "동시에 ‘확인’ 처리됩니다.") },
              { QStringLiteral("클립 저장"),
                QStringLiteral("재생 중인 구간을 mp4 파일로 내려받아 보관할 수 있습니다."), true },
              { QStringLiteral("‘NVR 없음’이 뜨면"),
                QStringLiteral("그 시각의 녹화가 서버에 남아 있지 않다는 뜻입니다. 저장 공간이 차서 "
                               "오래된 구간이 지워졌을 수 있습니다.") },
          } },
        },
        QString(),
    },
    {
        QStringLiteral("관제"), QStringLiteral("🔎"),
        QStringLiteral("영상 검색"),
        QStringLiteral("시각을 몰라도 됩니다. ‘어제 저녁에 낙상 있었어?’처럼 말로 물으면 해당 기록과 "
                       "영상을 찾아 줍니다."),
        {
          { QStringLiteral("질문하기"), {
              { QStringLiteral("말로 묻습니다"),
                QStringLiteral("‘어제 저녁에 낙상 있었어?’, ‘이번 주에 침대에서 나간 적 있어?’, "
                               "‘오늘 새벽에 무슨 일 있었어?’ 같은 문장을 그대로 적고 검색을 누릅니다."), true },
              { QStringLiteral("예시 질문"),
                QStringLiteral("아래 예시를 누르면 질문칸에 바로 채워집니다. 어떤 식으로 물으면 되는지 "
                               "감을 잡을 때 쓰세요.") },
              { QStringLiteral("채널 한정"),
                QStringLiteral("기본값은 전체 채널입니다. 특정 병상만 보고 싶을 때만 채널을 고르세요.") },
              { QStringLiteral("초기화"),
                QStringLiteral("질문과 조건, 결과를 한 번에 지웁니다.") },
          } },
          { QStringLiteral("결과 보기"), {
              { QStringLiteral("결과 목록"),
                QStringLiteral("찾은 기록이 ‘시각 · 채널 · 종류 · 입소자’ 형태로 나열됩니다.") },
              { QStringLiteral("눌러서 바로 재생"),
                QStringLiteral("결과를 누르면 이 페이지 안 오른쪽 재생기에서 바로 틀어 줍니다. "
                               "▶/⏸와 탐색 막대로 구간을 넘겨 볼 수 있습니다. 검색을 이어가려고 "
                               "다른 페이지로 튕겨 나가지 않습니다."), true },
              { QStringLiteral("‘저장된 클립이 없는 기록입니다’"),
                QStringLiteral("기록은 남아 있지만 영상 파일이 없다는 뜻입니다(보관 기간 경과 등).") },
          } },
        },
        QStringLiteral("이 기능은 <b>영상 서버에 연결되어 있어야</b> 동작합니다. "
                       "‘영상 서버에 연결되어 있지 않습니다’가 뜨면 서버 연결부터 확인하세요."),
    },

    // ── 기록·관리 ────────────────────────────────────────────
    {
        QStringLiteral("기록 · 관리"), QStringLiteral("📈"),
        QStringLiteral("일일 리포트"),
        QStringLiteral("하루 동안 한 입소자가 어떻게 지냈는지를 숫자와 그래프로 정리해 봅니다."),
        {
          { QStringLiteral("무엇을 고르나"), {
              { QStringLiteral("날짜"),
                QStringLiteral("왼쪽 달력에서 날짜를 고릅니다. 자료가 없는 미래 날짜는 선택되지 않고, "
                               "‘오늘’ 버튼으로 언제든 돌아옵니다.") },
              { QStringLiteral("입소자"),
                QStringLiteral("위쪽 이름 탭으로 사람을 바꿉니다. 리포트는 ‘날짜 한 개 + 입소자 한 명’ "
                               "단위입니다.") },
          } },
          { QStringLiteral("지표"), {
              { QStringLiteral("누워있는 시간"),
                QStringLiteral("침대 ROI 안에 누워 있던 시간의 합입니다.") },
              { QStringLiteral("활동량"),
                QStringLiteral("웨어러블 만보기 기준 걸음 수입니다.") },
              { QStringLiteral("케어시간"),
                QStringLiteral("요양보호사가 곁에 머문 시간으로 기록된 값입니다.") },
              { QStringLiteral("이벤트"),
                QStringLiteral("그날 그 사람에게 난 낙상·침상이탈 횟수입니다.") },
              { QStringLiteral("시간별 활동량"),
                QStringLiteral("하루를 시간대로 쪼개 활동량을 막대로 보여 줍니다. 밤에 유난히 "
                               "움직임이 많았던 시간대를 찾을 때 유용합니다.") },
          } },
          { QStringLiteral("내보내기"), {
              { QStringLiteral("PDF 내보내기"),
                QStringLiteral("보고 있는 리포트를 그대로 PDF로 저장합니다. 보호자 설명이나 "
                               "인수인계 자료로 씁니다."), true },
          } },
        },
        QString(),
    },
    {
        QStringLiteral("기록 · 관리"), QStringLiteral("👥"),
        QStringLiteral("입소자 관리"),
        QStringLiteral("입소자를 등록·수정·퇴원 처리하고, 위험도와 채널 배정을 관리합니다."),
        {
          { QStringLiteral("한눈에"), {
              { QStringLiteral("상단 요약"),
                QStringLiteral("재원 인원, 위험도 상/중/하 분포, 채널 배정 수를 보여 줍니다.") },
          } },
          { QStringLiteral("찾기"), {
              { QStringLiteral("재원 / 전체 / 퇴원"),
                QStringLiteral("왼쪽 목록을 상태별로 전환합니다.") },
              { QStringLiteral("🔍 이름 검색"),
                QStringLiteral("이름 일부만 입력해도 걸러집니다.") },
              { QStringLiteral("행 왼쪽 색 띠"),
                QStringLiteral("위험도입니다 — 상은 빨강, 중은 주황, 하는 초록.") },
          } },
          { QStringLiteral("편집"), {
              { QStringLiteral("목록 → 상세"),
                QStringLiteral("행을 클릭하면 오른쪽에서 바로 편집합니다. 팝업이 뜨지 않습니다.") },
              { QStringLiteral("＋ 신규 등록 · 저장"),
                QStringLiteral("새 입소자를 추가하거나 고친 내용을 저장합니다.") },
              { QStringLiteral("퇴원 처리"),
                QStringLiteral("퇴원시키거나 다시 재입원시킵니다. 바뀐 내역은 입원 이력에 남습니다.") },
          } },
        },
        QStringLiteral("여기서 등록한 입소자를 <b>장치 설정 → 카메라 → 침대·입소자 매핑</b>에서 "
                       "침대에 지정해야, 경보와 기록에 이름이 붙습니다."),
    },

    // ── 설정 ────────────────────────────────────────────────
    {
        QStringLiteral("설정"), QStringLiteral("🎥"),
        QStringLiteral("장치 설정 · 카메라"),
        QStringLiteral("CCTV를 연결하고, 침대 영역을 그리고, 화질과 초점을 원격으로 맞춥니다."),
        {
          { QStringLiteral("공통"), {
              { QStringLiteral("[카메라] / [알림] 전환"),
                QStringLiteral("위쪽에서 카메라 설정과 알림 노드 설정을 오갑니다.") },
              { QStringLiteral("CH1~4 채널 레일"),
                QStringLiteral("위에서 채널을 고르면 아래 설정과 오른쪽 영상이 그 채널로 함께 묶입니다. "
                               "각 채널에 연결 상태와 지정된 침대 수가 배지로 표시됩니다.") },
          } },
          { QStringLiteral("연결"), {
              { QStringLiteral("직접 입력"),
                QStringLiteral("CCTV IP · 계정 · 비밀번호를 넣고 연결합니다. 포트(554)와 프로파일은 "
                               "고정이라 입력하지 않습니다.") },
              { QStringLiteral("🔍 같은 망 카메라 검색"),
                QStringLiteral("같은 망의 ONVIF 카메라를 자동으로 찾아 모델·IP·MAC을 보여 줍니다. "
                               "행을 클릭하면 IP가 자동으로 채워집니다."), true },
              { QStringLiteral("다른 대역도 찾고 싶다면"),
                QStringLiteral("IP칸에 그 대역의 주소를 하나 적고 검색하면 그 대역까지 함께 훑습니다."), true },
              { QStringLiteral("연결하는 주체는 서버입니다"),
                QStringLiteral("Carenet이 카메라에 직접 붙는 게 아니라, IP를 서버(라즈베리파이)로 "
                               "보내면 서버가 RTSP를 엽니다. 그래서 <b>서버가 닿을 수 있는 대역</b>의 "
                               "카메라여야 영상이 나옵니다.") },
              { QStringLiteral("전체 해제"),
                QStringLiteral("4채널 연결을 한꺼번에 끊고 서버를 대기 상태로 되돌립니다.") },
          } },
          { QStringLiteral("ROI(침대)"), {
              { QStringLiteral("그리는 순서"),
                QStringLiteral("‘침대 추가’를 누르고 → 오른쪽 영상 위를 클릭해 모서리를 찍고 → "
                               "더블클릭(또는 우클릭)으로 완료합니다.") },
              { QStringLiteral("여러 개 그릴 수 있습니다"),
                QStringLiteral("한 채널에 침대를 최대 8개까지 지정할 수 있고, 각 침대가 낙상·침상이탈 "
                               "판정의 기준이 됩니다."), true },
              { QStringLiteral("영상에 표시 · 침대 제거"),
                QStringLiteral("그려 둔 영역을 화면에 겹쳐 볼지 끄고 켭니다. 잘못 그렸으면 제거 후 "
                               "다시 그리세요.") },
          } },
          { QStringLiteral("침대 · 입소자 매핑"), {
              { QStringLiteral("이름 붙이기"),
                QStringLiteral("침대 목록에서 그 침대의 입소자를 지정하면, 그 침대에서 감지된 사람에게 "
                               "이름이 붙어 ‘침대 2 김복순’처럼 알립니다.") },
              { QStringLiteral("현장 LED에도 나옵니다"),
                QStringLiteral("알림 노드 LED에 입소자 이름이 가운데 글자를 가린 형태로 표시됩니다."), true },
              { QStringLiteral("‘신원 미상’"),
                QStringLiteral("서버가 사람을 특정하지 못하면 이렇게 알립니다. 추적 번호는 신원이 "
                               "아니어서 이름을 확정할 수 없습니다.") },
          } },
          { QStringLiteral("이미지"), {
              { QStringLiteral("밝기 · 대비 · 채도"),
                QStringLiteral("슬라이더로 맞춘 뒤 ‘적용’을 누르면 카메라에 바로 반영됩니다. "
                               "‘초기화’로 되돌립니다."), true },
              { QStringLiteral("적용 전 / 적용 후 비교"),
                QStringLiteral("오른쪽에서 원본과 적용 결과를 나란히 비교할 수 있습니다.") },
              { QStringLiteral("초점"),
                QStringLiteral("실시간 영상에서 원하는 지점을 클릭하면 그 지점에 초점을 맞춥니다. "
                               "‘전체 자동초점’으로 카메라에 다시 맡길 수도 있습니다."), true },
          } },
        },
        QString(),
    },
    {
        QStringLiteral("설정"), QStringLiteral("🔔"),
        QStringLiteral("장치 설정 · 알림 노드"),
        QStringLiteral("병실에 달린 LED·스피커 노드의 밝기와 음량을 원격으로 맞춥니다."),
        {
          { QStringLiteral("대상 고르기"), {
              { QStringLiteral("대상 노드"),
                QStringLiteral("설정할 알림 노드를 고릅니다. 아직 응답이 없으면 ‘상태 미확인’으로 "
                               "표시됩니다.") },
              { QStringLiteral("LED 미리보기 (64×32)"),
                QStringLiteral("실제 LED와 같은 해상도로, 지금 밝기가 어떻게 보일지 화면에서 "
                               "미리 확인합니다."), true },
          } },
          { QStringLiteral("값 맞추기"), {
              { QStringLiteral("LED 밝기 · 스피커 음량"),
                QStringLiteral("낮에는 밝게, 밤에는 눈부시지 않게 — 병실 상황에 맞춰 조절합니다.") },
              { QStringLiteral("테스트"),
                QStringLiteral("저장하지 않고 지금 값으로 현장 LED에 문구를 한 번 띄우고 짧은 소리를 "
                               "냅니다. 실제 낙상 안내 음성이 나가지는 않습니다."), true },
              { QStringLiteral("적용"),
                QStringLiteral("지금 값을 노드에 바로 반영하고 저장합니다. 마지막으로 적용한 시각이 "
                               "아래에 남습니다.") },
          } },
          { QStringLiteral("현장에서는"), {
              { QStringLiteral("낙상이 나면"),
                QStringLiteral("LED에 입소자 이름과 호실이 뜨고 스피커로 안내가 나갑니다. 호실을 "
                               "모르면 호실 없이 안내합니다."), true },
          } },
        },
        QString(),
    },

    // ── 도움 ────────────────────────────────────────────────
    {
        QStringLiteral("도움"), QStringLiteral("🛠"),
        QStringLiteral("문제 해결"),
        QStringLiteral("자주 막히는 지점과 확인 순서입니다."),
        {
          { QStringLiteral("영상"), {
              { QStringLiteral("영상이 안 나옵니다"),
                QStringLiteral("① 상단에 서버 연결이 살아 있는지 ② 장치 설정 → 카메라에서 그 채널이 "
                               "연결되어 있는지 ③ CCTV 계정·비밀번호가 맞는지 순서로 확인하세요.") },
              { QStringLiteral("일부 채널만 안 나옵니다"),
                QStringLiteral("채널은 두 대의 서버가 나눠 맡습니다(CH1·2 / CH3·4). 한쪽 서버만 "
                               "끊기면 두 채널만 검게 남습니다.") },
          } },
          { QStringLiteral("카메라 검색"), {
              { QStringLiteral("검색해도 아무것도 안 뜹니다"),
                QStringLiteral("① 관제 PC와 카메라가 같은 공유기·스위치에 물려 있는지 ② 카메라의 "
                               "ONVIF 검색이 켜져 있는지 ③ PC 방화벽이 Carenet의 UDP 수신을 막고 있지 "
                               "않은지 확인하세요.") },
              { QStringLiteral("다른 대역에 있습니다"),
                QStringLiteral("IP칸에 그 대역의 주소를 하나 적고 다시 검색하면 그 대역까지 훑습니다. "
                               "다만 찾더라도 서버가 그 대역에 닿아야 영상이 열립니다."), true },
          } },
          { QStringLiteral("경보"), {
              { QStringLiteral("낙상 알림이 오지 않습니다"),
                QStringLiteral("그 채널에 침대 ROI가 그려져 있는지 먼저 보세요. 침대가 없으면 판정 "
                               "자체가 시작되지 않습니다.") },
              { QStringLiteral("알림에 이름이 안 뜹니다"),
                QStringLiteral("장치 설정 → 카메라 → 침대·입소자 매핑에서 그 침대에 입소자를 "
                               "지정하세요.") },
              { QStringLiteral("경보음이 계속 납니다"),
                QStringLiteral("화면 위 경보 카드의 ‘경보 해제’를 누르면 현장 사이렌·LED까지 함께 "
                               "꺼집니다.") },
          } },
        },
        QStringLiteral("그래도 해결되지 않으면 발생 시각과 채널을 적어 두세요 — "
                       "<b>영상 검색</b>에서 그 시각의 영상을 찾아 원인을 짚을 수 있습니다."),
    },
    };
    return topics;
}

}  // namespace

void MainWindow::onHelpClicked()
{
    if (!helpDialog) {
        helpDialog = new QDialog(this);
        helpDialog->setObjectName("panel");
        helpDialog->setWindowTitle(QStringLiteral("Carenet 도움말"));
        helpDialog->resize(940, 700);
        helpDialog->setMinimumSize(700, 480);
        enableDarkTitleBar(helpDialog);
        auto* h = new QHBoxLayout(helpDialog);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);

        // ── 좌: 제목 + 검색 + 주제 목록 ──
        auto* side = new QFrame();
        side->setObjectName(QStringLiteral("helpSide"));
        side->setFixedWidth(236);
        auto* sv = new QVBoxLayout(side);
        sv->setContentsMargins(14, 16, 14, 12);
        sv->setSpacing(9);

        auto* sideTitle = new QLabel(QStringLiteral("Carenet 도움말"));
        sideTitle->setObjectName(QStringLiteral("helpSideTitle"));
        sv->addWidget(sideTitle);

        auto* search = new QLineEdit();
        search->setObjectName(QStringLiteral("helpSearch"));
        search->setPlaceholderText(QStringLiteral("🔍  기능 검색"));
        search->setClearButtonEnabled(true);
        sv->addWidget(search);

        helpList = new QListWidget();
        helpList->setObjectName(QStringLiteral("helpList"));
        helpList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        sv->addWidget(helpList, 1);
        h->addWidget(side);

        // 주제를 section별로 묶어 채운다 — 머리글 행은 선택되지 않게 두고,
        // 실제 주제 행에만 UserRole로 주제 번호를 심는다(행 번호 ≠ 주제 번호).
        const QVector<HelpTopic>& topics = helpTopics();
        QString lastSection;
        for (int i = 0; i < topics.size(); ++i) {
            const HelpTopic& tp = topics.at(i);
            if (tp.section != lastSection) {
                lastSection = tp.section;
                auto* head = new QListWidgetItem(tp.section, helpList);
                head->setData(Qt::UserRole, -1);
                head->setFlags(Qt::NoItemFlags);          // 선택·호버 대상에서 제외
                head->setForeground(QColor(QString::fromLatin1(kAccent)));
                QFont hf = head->font();
                hf.setPointSizeF(hf.pointSizeF() - 1.0);
                hf.setBold(true);
                head->setFont(hf);
            }
            auto* it = new QListWidgetItem(
                QStringLiteral("%1   %2").arg(tp.icon, tp.title), helpList);
            it->setData(Qt::UserRole, i);
            // 검색이 훑을 건초더미 — 제목뿐 아니라 본문 용어·설명까지 넣어야
            // "스냅샷", "PDF"처럼 본문에만 있는 말로도 주제를 찾을 수 있다.
            QString hay = tp.title + QLatin1Char(' ') + tp.summary;
            for (const HelpGroup& g : tp.groups) {
                hay += QLatin1Char(' ') + g.heading;
                for (const HelpEntry& e : g.entries)
                    hay += QLatin1Char(' ') + e.term + QLatin1Char(' ') + e.desc;
            }
            it->setData(Qt::UserRole + 1, hay);
        }

        // ── 우: 본문 ──
        helpBrowser = new QTextBrowser();
        helpBrowser->setObjectName(QStringLiteral("helpBrowser"));
        helpBrowser->setOpenExternalLinks(false);
        h->addWidget(helpBrowser, 1);

        connect(helpList, &QListWidget::currentRowChanged, this,
                &MainWindow::renderHelpTopic);

        connect(search, &QLineEdit::textChanged, this, [this](const QString& q) {
            const QString needle = q.trimmed();
            for (int i = 0; i < helpList->count(); ++i) {
                QListWidgetItem* it = helpList->item(i);
                if (it->data(Qt::UserRole).toInt() < 0) continue;   // 머리글은 뒤에서
                it->setHidden(!needle.isEmpty() &&
                              !it->data(Qt::UserRole + 1).toString()
                                   .contains(needle, Qt::CaseInsensitive));
            }
            // 머리글은 자기 묶음에 남은 주제가 하나도 없을 때만 숨긴다.
            for (int i = 0; i < helpList->count(); ++i) {
                QListWidgetItem* head = helpList->item(i);
                if (head->data(Qt::UserRole).toInt() >= 0) continue;
                bool any = false;
                for (int j = i + 1; j < helpList->count(); ++j) {
                    QListWidgetItem* n = helpList->item(j);
                    if (n->data(Qt::UserRole).toInt() < 0) break;   // 다음 묶음 시작
                    if (!n->isHidden()) { any = true; break; }
                }
                head->setHidden(!any);
            }
        });

        helpList->setCurrentRow(1);   // 0은 머리글 — 첫 실제 주제는 1행
    }
    renderHelpTopic(helpList ? helpList->currentRow() : 1);  // 현재 테마 색으로 갱신
    helpDialog->show();
    helpDialog->raise();
    helpDialog->activateWindow();
}

// 선택된 도움말 주제를 현재 테마 색으로 렌더한다.
//
// QTextBrowser가 이해하는 건 Qt 리치텍스트(HTML 부분집합)라 flex도 border-radius도
// 없다. 대신 표 셀의 배경색·패딩은 확실히 먹으므로, 카드는 전부 "색 띠 칸 + 내용 칸"
// 2칸짜리 표로 만든다. 색은 매번 현재 팔레트에서 새로 읽어 테마 토글에 따라온다.
void MainWindow::renderHelpTopic(int row)
{
    if (!helpBrowser) return;

    // 좌측 목록엔 머리글 행이 섞여 있어 행 번호가 곧 주제 번호가 아니다.
    // 머리글이 넘어오면(또는 검색으로 선택이 풀리면) 보고 있던 주제를 유지한다.
    int idx = helpTopicShown_;
    if (helpList) {
        if (const QListWidgetItem* it = helpList->item(row)) {
            const int t = it->data(Qt::UserRole).toInt();
            if (t >= 0) idx = t;
        }
    }
    const QVector<HelpTopic>& topics = helpTopics();
    if (topics.isEmpty()) return;
    idx = qBound(0, idx, topics.size() - 1);
    helpTopicShown_ = idx;
    const HelpTopic& tp = topics.at(idx);

    const QString A  = QString::fromLatin1(kAccent);
    const QString T  = QString::fromLatin1(kTextMain);
    const QString S  = QString::fromLatin1(kTextSub);
    const QString BD = QString::fromLatin1(kBorder);
    const QString C  = QString::fromLatin1(kCard);
    const QString OA = QString::fromLatin1(kOnAccent);
    const QString W  = QString::fromLatin1(kWarn);

    // 색 띠(왼쪽 3px) + 내용 카드.
    auto card = [&](const QString& bar, const QString& head, const QString& body) {
        return QStringLiteral(
            "<table width='100%' cellspacing='0' cellpadding='0' style='margin:0 0 7px;'>"
            "<tr><td width='3' bgcolor='%1'></td>"
            "<td bgcolor='%2' style='padding:9px 14px;'>"
            "<div style='font-size:13px; font-weight:700; color:%3;'>%4</div>"
            "<div style='font-size:13px; color:%5; line-height:152%;'>%6</div>"
            "</td></tr></table>").arg(bar, C, T, head, S, body);
    };

    QString html = QStringLiteral(
        "<table width='100%' cellspacing='0' cellpadding='0'><tr>"
        "<td width='46' valign='top'><span style='font-size:28px;'>%1</span></td>"
        "<td valign='top'>"
        "<div style='font-size:21px; font-weight:800; color:%2;'>%3</div>"
        "<div style='font-size:13px; color:%4; line-height:152%;'>%5</div>"
        "</td></tr></table>"
        // 구분선은 <hr>로만 둔다 — 높이 1px짜리 표 행은 Qt가 글자 높이만큼
        // 부풀려 두꺼운 띄로 그려버리고, div의 border-top은 아예 무시된다.
        "<hr style='border:none; border-top:1px solid %6;'>")
        .arg(tp.icon, T, tp.title, S, tp.summary, BD);

    for (const HelpGroup& g : tp.groups) {
        if (!g.heading.isEmpty())
            html += QStringLiteral(
                "<div style='color:%1; font-size:12px; font-weight:800; "
                "margin:15px 0 7px; letter-spacing:1px;'>%2</div>").arg(A, g.heading);
        for (const HelpEntry& e : g.entries) {
            QString head = e.term;
            if (e.isNew)
                head += QStringLiteral(
                    "&nbsp;&nbsp;<span style='background-color:%1; color:%2; "
                    "font-size:10px; font-weight:800;'>&nbsp;NEW&nbsp;</span>").arg(A, OA);
            html += card(A, head, e.desc);
        }
    }
    if (!tp.tip.isEmpty())
        html += QStringLiteral("<div style='margin-top:9px;'></div>")
              + card(W, QStringLiteral("💡 팁"), tp.tip);

    helpBrowser->setHtml(
        QStringLiteral(
            "<div style='font-family:\"Segoe UI\",\"맑은 고딕\",\"Malgun Gothic\","
            "\"Apple SD Gothic Neo\",\"Noto Sans CJK KR\",\"Noto Sans KR\",sans-serif; "
            "font-size:13px; color:%1;'>%2</div>").arg(T, html));
    // 배경/여백은 base.qss의 QTextBrowser#helpBrowser 규칙이 담당한다.
}
