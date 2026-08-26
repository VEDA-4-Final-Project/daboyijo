# docs — 문서

설계 근거와 실측 기록을 남기는 곳입니다. **"왜 이렇게 만들었는가"와 "실제로 재보니
어땠는가"** 가 여기 있고, 모듈 사용법은 각 폴더의 `README.md`에 있습니다.

## 실측 명세 · 리포트

| 문서 | 내용 |
| --- | --- |
| [wiseai-메타데이터-명세.md](wiseai-메타데이터-명세.md) | **한화비전 WiseAI 메타데이터 실측 명세.** PNM-C16083RVQ에서 직접 덤프한 XML 구조, 좌표 정규화(ONVIF y축 반전 함정), 실측으로 확인된 함정 5가지, 낙상 판정에 쓸 신호 목록 |
| [cctv-연결확인-리포트.md](cctv-연결확인-리포트.md) | 2주차 산출물 — 카메라 연결·RTSP 4채널 수신 검증, 프로파일 조정(4MP@30fps → 720p@15fps), 트러블슈팅 |
| [design-tokens-contrast-audit.md](design-tokens-contrast-audit.md) | Qt 테마 색상 대비 감사 결과. ⚠️ `client/tools/theme_audit.py --write`가 생성 — **손으로 고치지 말 것** |

## 설계 근거

| 문서 | 내용 |
| --- | --- |
| [architecture-timer-vs-interrupt.md](architecture-timer-vs-interrupt.md) | 웨어러블 펌웨어 아키텍처 전환 — 타이머 폴링에서 센서 인터럽트 + FIFO 블록 처리로 바꾼 이유와 결과 |
| [caregiver_detection_algorithm.md](caregiver_detection_algorithm.md) | 요양보호사 인식 — 옷 색(HSV) 기반 판정 알고리즘과 임계값 근거 |
| [qt-mqtt-integration.md](qt-mqtt-integration.md) | Qt 관제 앱 MQTT 연동 — `QMqttClient`와 `libmosquitto`가 같은 JSON 계약을 쓰는 구조 |
| [videoview-coordinate-mapping.md](videoview-coordinate-mapping.md) | VideoView 좌표 매핑 기준선 — "무엇이 바뀌면 침대 ROI 클릭이 깨지는가" |

## 다른 곳에 있는 문서

| 문서 | 위치 |
| --- | --- |
| 서버 모듈 구조 · 협업 규칙 | [`server/ARCHITECTURE.md`](../server/ARCHITECTURE.md) |
| 영상 스트림 프로토콜 명세 | [`protocol/video_stream.md`](../protocol/video_stream.md) |
| 통신 구간별 규격 총람 | [`protocol/README.md`](../protocol/README.md) |
| 모듈별 빌드·설정·트러블슈팅 | 각 폴더의 `README.md` |

---

> 📌 문서를 새로 쓸 때는 **결론뿐 아니라 근거와 실측값**을 같이 남겨 주세요.
> 임계값 하나에도 "왜 이 숫자인가"가 붙어 있어야 나중에 튜닝할 수 있습니다.
