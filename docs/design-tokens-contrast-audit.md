# Design Tokens Contrast Audit

이 파일은 `client/tools/theme_audit.py --write`가 생성한다. 손으로 고치지 말 것.

## 측정 방법

- **상대휘도:** WCAG 2.1 공식 `L = 0.2126R + 0.7152G + 0.0722B` (각 sRGB 채널을 먼저
  선형화한 뒤 계산)
- **대비비:** `(L_밝음 + 0.05) / (L_어두움 + 0.05)`
- **기준:** 본문(일반 텍스트) 4.5:1, 큰 텍스트·UI 컴포넌트 경계 3:1(이 감사는 보수적으로
  전부 4.5:1을 기준으로 삼는다)

## accent hex와 AA 요구사항의 충돌 해소 방식

accent(`#12B5A6`)는 사용자 결정으로 hex가 고정돼 있다(D-01). 계산 결과 이 색은 라이트
배경 위 텍스트로도, 흰 글씨를 얹은 채우기로도 AA(4.5:1)를 통과하지 못한다(2.4~2.6:1).
hex는 바꾸지 않고 **용법 규칙**으로 해결한다 — accent는 항상 "채우기 + 어두운 텍스트"
형태로만 쓴다(`onAccent` 토큰, 아래 (b) 표 참조). accent를 텍스트 색으로 쓰거나 흰
글씨를 그 위에 얹는 용법은 금지한다.

## 감사 범위 밖으로 남긴 예외 3건

1. **`VideoView::zoneColor()`의 침대 식별용 8색** — Palette 소속이 아니고, 심각도가
   아니라 "어느 침대인지" 식별용이라 이 감사 대상이 아니다.
2. **영상 캔버스 위 안내 문구 색** — `videoSurface`(고정 어두운 표면) 전용 전경색이라
   팔레트 조합이 아니다.
3. **다크 팔레트 전반(D-08)** — 다크 테마는 이 단계에서 재설계하지 않는다. 회귀 방어를
   위해 `onAccent on accent` 한 조합만 다크에서도 검사한다.

---

## (a) Palette 필드 순서 검증

- PASS: 필드 13개, kLight/kDark 값 개수 일치 (['bgDeep', 'panel', 'card', 'border', 'text', 'sub', 'accent', 'onAccent', 'normal', 'warn', 'high', 'critical', 'info'])

## (b) WCAG 2.1 대비비 (라이트 팔레트, 필수 통과)

| 조합 | 배경 | 전경 | 대비비 | 기준 | 결과 |
|------|------|------|--------|------|------|
| text on bgDeep | `#F4F7FA` | `#1E2A32` | 13.63:1 | 4.5 | PASS |
| text on panel | `#FFFFFF` | `#1E2A32` | 14.66:1 | 4.5 | PASS |
| text on card | `#F0F4F8` | `#1E2A32` | 13.26:1 | 4.5 | PASS |
| sub on bgDeep | `#F4F7FA` | `#5C6B78` | 5.10:1 | 4.5 | PASS |
| sub on panel | `#FFFFFF` | `#5C6B78` | 5.48:1 | 4.5 | PASS |
| sub on card | `#F0F4F8` | `#5C6B78` | 4.96:1 | 4.5 | PASS |
| normal as text on bgDeep | `#F4F7FA` | `#257A4A` | 4.93:1 | 4.5 | PASS |
| normal as text on panel | `#FFFFFF` | `#257A4A` | 5.30:1 | 4.5 | PASS |
| warn as text on bgDeep | `#F4F7FA` | `#8A6400` | 5.00:1 | 4.5 | PASS |
| warn as text on panel | `#FFFFFF` | `#8A6400` | 5.38:1 | 4.5 | PASS |
| high as text on bgDeep | `#F4F7FA` | `#A05000` | 5.36:1 | 4.5 | PASS |
| high as text on panel | `#FFFFFF` | `#A05000` | 5.77:1 | 4.5 | PASS |
| critical as text on bgDeep | `#F4F7FA` | `#C23934` | 4.97:1 | 4.5 | PASS |
| critical as text on panel | `#FFFFFF` | `#C23934` | 5.35:1 | 4.5 | PASS |
| info as text on bgDeep | `#F4F7FA` | `#1D5FA8` | 6.00:1 | 4.5 | PASS |
| info as text on panel | `#FFFFFF` | `#1D5FA8` | 6.45:1 | 4.5 | PASS |
| white on normal fill | `#257A4A` | `#FFFFFF` | 5.30:1 | 4.5 | PASS |
| white on warn fill | `#8A6400` | `#FFFFFF` | 5.38:1 | 4.5 | PASS |
| white on high fill | `#A05000` | `#FFFFFF` | 5.77:1 | 4.5 | PASS |
| white on critical fill | `#C23934` | `#FFFFFF` | 5.35:1 | 4.5 | PASS |
| white on info fill | `#1D5FA8` | `#FFFFFF` | 6.45:1 | 4.5 | PASS |
| onAccent on accent (light) | `#12B5A6` | `#1E2A32` | 5.71:1 | 4.5 | PASS |

## (b-2) WCAG 2.1 대비비 (다크 팔레트, 회귀 방어용 1건만 필수)

| 조합 | 배경 | 전경 | 대비비 | 기준 | 결과 |
|------|------|------|--------|------|------|
| onAccent on accent (dark) | `#17C7B6` | `#0E141B` | 8.71:1 | 4.5 | PASS |

D-08: 다크 전체 감사는 이 단계의 범위가 아니다. onAccent만 이 단계가 새로 도입하는 토큰이라 회귀 방어를 위해 넣는다.

## (b-3) 참고 항목 — 금지 용법의 근거 (종료 코드에 영향 없음)

| 조합 | 배경 | 전경 | 대비비 | 비고 |
|------|------|------|--------|------|
| accent as text on panel | `#FFFFFF` | `#12B5A6` | 2.57:1 | 금지 용법 근거 |
| accent as text on bgDeep | `#F4F7FA` | `#12B5A6` | 2.39:1 | 금지 용법 근거 |
| white on accent fill | `#12B5A6` | `#FFFFFF` | 2.57:1 | 금지 용법 근거 |
| dark text on accent fill (light) | `#12B5A6` | `#1E2A32` | 5.71:1 | 허용 용법 — 5.7x 근처가 나와야 정상 |

## (c) 심각도 5색 — 그레이스케일 명도 수렴 (D-09 근거)

| 등급 | 색 | 상대휘도(L) |
|------|-----|--------------|
| normal | `#257A4A` | 0.1481 |
| warn | `#8A6400` | 0.1452 |
| high | `#A05000` | 0.1321 |
| critical | `#C23934` | 0.1464 |
| info | `#1D5FA8` | 0.1127 |

**명도 폭:** 최댓값 0.1481 − 최솟값 0.1127 = **0.0353**

**판정: 색 단독 구분 불가 — 도형 병행 필수 (D-09).** 폭이 0.04 미만이라 그레이스케일로 캡처하면 5등급이 거의 같은 회색으로 보인다.

## (d) 토큰 배선 감사 — QSS %(token) ↔ 치환 목록

- base.qss가 쓰는 토큰 15개, substitute()의 치환 목록 15개
- PASS: base.qss의 모든 %(token)이 substitute()에서 치환된다
- dialogs.qss가 쓰는 토큰 10개, dialogStyleSheet()의 치환 목록 10개
- PASS: dialogs.qss의 모든 %(token)이 dialogStyleSheet()에서 치환된다
