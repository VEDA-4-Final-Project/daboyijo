# server — 중앙 서버 (Raspberry Pi 4)

담당: 박민용, 홍성준

| 하위 폴더 | 역할 |
| --- | --- |
| `video/` | RTSP 4채널 수신·디코딩, ONVIF 연동, OpenCV 영상처리(저조도 보정·침상 ROI 마스킹), WiseAI 메타데이터 파싱, 요양보호사 진입 감지 |
| `core/` | 다중 교차 검증 룰엔진(최종 낙상 판정), 입소자 DB·케어 로그, OpenSSL 서버측 보안 통신, MQTT 발행 |

빌드: `make` (추후 작성)
