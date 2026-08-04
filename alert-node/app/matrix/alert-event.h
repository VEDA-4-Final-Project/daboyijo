/*
 * alert-event.h - 이벤트 소스와 패널 렌더링 사이의 경계
 *
 * 패널이 그리는 데 필요한 건 문구와 등급뿐이다. 그래서 MQTT JSON 스키마가
 * 확정되기 전에도 이 경계는 바뀌지 않는다.
 *
 * 지금 소스는 난수 데모(demo_next_event) 하나뿐이다. MQTT 연동은 같은
 * 시그니처로 소스를 하나 더 만들어 main 에서 갈아끼우면 된다. 호실·타입·
 * 측정값을 문구로 조립하는 일은 그 소스 안에서 끝낸다.
 */
#ifndef ALERT_EVENT_H
#define ALERT_EVENT_H

/* 경보 등급 - 색과 깜빡임을 정한다 */
enum severity {
	SEV_INFO,	/* 정상 감시중 - 차분한 초록 */
	SEV_WARN,	/* 웨어러블 이상(체온/심박/산소) - 주황 */
	SEV_CRIT,	/* 낙상/침상 이탈 - 빨강, 테두리 깜빡임 */
};

#define ALERT_MSG_MAX	128

struct alert_event {
	char		msg[ALERT_MSG_MAX];	/* 패널에 흘릴 문구 (UTF-8) */
	enum severity	sev;
	int		passes;			/* 반복 스크롤 횟수 */
};

/*
 * 이벤트 소스 - ev 를 채우고 1, 지금 알릴 게 없으면 0 을 반환한다.
 * 블로킹 여부는 구현에 맡긴다 (데모는 즉시 반환).
 */
typedef int (*alert_source_fn)(struct alert_event *ev);

#endif /* ALERT_EVENT_H */
