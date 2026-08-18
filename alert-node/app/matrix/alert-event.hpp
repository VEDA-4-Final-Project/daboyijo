// 이벤트 소스와 패널 렌더링 사이의 경계
//
// 패널이 그리는 데 필요한 건 문구와 등급뿐 — MQTT 스키마가 바뀌어도 이 경계는 그대로
// 새 소스는 같은 시그니처로 만들어 main 에서 갈아끼우면 되고,
// 호실·타입·측정값을 문구로 조립하는 일은 그 소스 안에서 끝냄
#ifndef ALERT_EVENT_HPP
#define ALERT_EVENT_HPP

// 경보 등급 — 색과 깜빡임 결정
enum severity {
	SEV_INFO,	// 정상 감시중 — 차분한 초록
	SEV_WARN,	// 웨어러블 이상(체온/심박/산소) — 주황
	SEV_CRIT,	// 낙상/침상 이탈 — 빨강, 테두리 깜빡임
};

#define ALERT_MSG_MAX	128

struct alert_event {
	char		msg[ALERT_MSG_MAX];	// 패널에 흘릴 문구 (UTF-8)
	enum severity	sev;
	int		passes;			// 반복 스크롤 횟수
};

// 이벤트 소스 — ev 를 채우면 1, 알릴 게 없으면 0 반환
// 블로킹 여부는 구현에 맡김 (데모는 즉시 반환)
typedef int (*alert_source_fn)(struct alert_event *ev);

#endif // ALERT_EVENT_HPP
