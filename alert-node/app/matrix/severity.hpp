// 경보 등급 정의 — 패널이 색과 깜빡임을 결정하는 데 쓴다
#ifndef ALERT_EVENT_HPP
#define ALERT_EVENT_HPP

enum severity {
	SEV_INFO,	// 정상 감시중 — 차분한 초록
	SEV_WARN,	// 웨어러블 이상(체온/심박/산소) — 주황
	SEV_CRIT,	// 낙상/침상 이탈 — 빨강, 테두리 깜빡임
};

#endif // ALERT_EVENT_HPP
