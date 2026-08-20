#!/usr/bin/env python3
"""theme_audit.py — Phase 2 디자인 토큰 기계 감사.

이 프로젝트에는 UI 스크린샷·시각 diff 도구가 없다(tools/uicap.py, tools/uidiff.py,
screenshots/ 는 존재하지 않는다). "AA를 만족한다"를 사람이 색을 보고 판단하게 두면
검증이 아니라 의견이 된다. 계산으로 확정 가능한 부분은 전부 이 스크립트로 만든다.

네 가지 감사:
  (a) client/theme.h의 Palette 필드 순서 검증 — 13필드, kLight/kDark 값 개수 일치
  (b) WCAG 2.1 대비비 계산 — 라이트 팔레트 필수 조합 + 다크 onAccent-on-accent 1건
  (c) 심각도 5색의 그레이스케일 명도 수렴 측정(D-09 근거)
  (d) base.qss/dialogs.qss의 %(token) 사용처 ↔ thememanager.cpp 치환 목록 대조

표준 라이브러리만 사용한다. 외부 패키지를 설치하지 않는다.

Usage:
    python client/tools/theme_audit.py            # 감사만 실행, 종료 코드로 결과 표시
    python client/tools/theme_audit.py --write     # docs/design-tokens-contrast-audit.md 갱신
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
THEME_H = REPO_ROOT / "client" / "theme.h"
THEMEMANAGER_CPP = REPO_ROOT / "client" / "thememanager.cpp"
BASE_QSS = REPO_ROOT / "client" / "resources" / "style" / "base.qss"
DIALOGS_QSS = REPO_ROOT / "client" / "resources" / "style" / "dialogs.qss"
OUTPUT_DOC = REPO_ROOT / "docs" / "design-tokens-contrast-audit.md"

EXPECTED_FIELD_COUNT = 13


# ─────────────────────────────────────────────────────────────────────────
#  WCAG 2.1 상대휘도·대비비
# ─────────────────────────────────────────────────────────────────────────

def _linearize(channel: float) -> float:
    c = channel / 255.0
    if c <= 0.03928:
        return c / 12.92
    return ((c + 0.055) / 1.055) ** 2.4


def hex_to_rgb(hex_color: str) -> tuple[int, int, int]:
    h = hex_color.lstrip("#")
    return int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)


def relative_luminance(hex_color: str) -> float:
    r, g, b = hex_to_rgb(hex_color)
    rl, gl, bl = _linearize(r), _linearize(g), _linearize(b)
    return 0.2126 * rl + 0.7152 * gl + 0.0722 * bl


def contrast_ratio(hex_a: str, hex_b: str) -> float:
    la, lb = relative_luminance(hex_a), relative_luminance(hex_b)
    lighter, darker = max(la, lb), min(la, lb)
    return (lighter + 0.05) / (darker + 0.05)


# ─────────────────────────────────────────────────────────────────────────
#  (a) theme.h 파싱 — 필드 순서 검증
# ─────────────────────────────────────────────────────────────────────────

@dataclass
class ParsedTheme:
    field_names: list[str]
    light_values: list[str]
    dark_values: list[str]
    errors: list[str] = field(default_factory=list)


def parse_theme_h(text: str) -> ParsedTheme:
    errors: list[str] = []

    struct_match = re.search(r"struct Palette\s*\{(.*?)\};", text, re.DOTALL)
    if not struct_match:
        errors.append("client/theme.h에서 'struct Palette { ... };' 블록을 찾지 못했다")
        return ParsedTheme([], [], [], errors)

    field_names = re.findall(r"\*(\w+)", struct_match.group(1))

    light_match = re.search(r"inline const Palette kLight\s*\{(.*?)\};", text, re.DOTALL)
    dark_match = re.search(r"inline const Palette kDark\s*\{(.*?)\};", text, re.DOTALL)
    if not light_match:
        errors.append("client/theme.h에서 'kLight { ... };' 리터럴을 찾지 못했다")
    if not dark_match:
        errors.append("client/theme.h에서 'kDark { ... };' 리터럴을 찾지 못했다")

    light_values = re.findall(r'"(#[0-9A-Fa-f]{6})"', light_match.group(1)) if light_match else []
    dark_values = re.findall(r'"(#[0-9A-Fa-f]{6})"', dark_match.group(1)) if dark_match else []

    if len(field_names) != EXPECTED_FIELD_COUNT:
        errors.append(
            f"Palette 필드 수가 {EXPECTED_FIELD_COUNT}이 아니다 (실제 {len(field_names)}: {field_names})"
        )
    if light_match and len(light_values) != len(field_names):
        errors.append(
            f"kLight 값 개수({len(light_values)})가 필드 개수({len(field_names)})와 다르다 — "
            "C++17 위치 기반 초기화라 어긋나도 컴파일은 통과하고 색만 조용히 뒤바뀐다"
        )
    if dark_match and len(dark_values) != len(field_names):
        errors.append(
            f"kDark 값 개수({len(dark_values)})가 필드 개수({len(field_names)})와 다르다"
        )

    return ParsedTheme(field_names, light_values, dark_values, errors)


def palette_dict(field_names: list[str], values: list[str]) -> dict[str, str]:
    return dict(zip(field_names, values))


# ─────────────────────────────────────────────────────────────────────────
#  (d) 토큰 배선 감사 — QSS %(token) ↔ substitute()/dialogStyleSheet() 치환 목록
# ─────────────────────────────────────────────────────────────────────────

def extract_qss_tokens(qss_text: str) -> set[str]:
    return set(re.findall(r"%\((\w+)\)", qss_text))


def extract_substitution_targets(cpp_text: str, func_name: str) -> set[str]:
    # 함수 본문(중괄호로 대략 감싼 구간)만 잘라서 그 안의 .replace("%(token)", ...) 를 찾는다.
    func_match = re.search(
        rf"{re.escape(func_name)}\s*\([^)]*\)\s*\{{(.*?)\n\}}", cpp_text, re.DOTALL
    )
    body = func_match.group(1) if func_match else cpp_text
    return set(re.findall(r'\.replace\("%\((\w+)\)"', body))


# ─────────────────────────────────────────────────────────────────────────
#  메인 감사 로직
# ─────────────────────────────────────────────────────────────────────────

@dataclass
class AuditResult:
    ok: bool
    lines: list[str] = field(default_factory=list)
    required_failures: list[str] = field(default_factory=list)


def run_audit() -> AuditResult:
    lines: list[str] = []
    required_failures: list[str] = []

    if not THEME_H.exists():
        required_failures.append(f"{THEME_H} 파일이 없다")
        return AuditResult(False, lines, required_failures)

    theme_text = THEME_H.read_text(encoding="utf-8")
    parsed = parse_theme_h(theme_text)
    required_failures.extend(parsed.errors)

    lines.append("## (a) Palette 필드 순서 검증\n")
    if parsed.errors:
        for e in parsed.errors:
            lines.append(f"- FAIL: {e}")
        # 필드 순서가 깨졌으면 이후 계산이 무의미하므로 여기서 반환.
        return AuditResult(False, lines, required_failures)
    else:
        lines.append(
            f"- PASS: 필드 {len(parsed.field_names)}개, kLight/kDark 값 개수 일치 "
            f"({parsed.field_names})"
        )

    light = palette_dict(parsed.field_names, parsed.light_values)
    dark = palette_dict(parsed.field_names, parsed.dark_values)

    # ── (b) 대비비 계산 ──────────────────────────────────────────────
    lines.append("\n## (b) WCAG 2.1 대비비 (라이트 팔레트, 필수 통과)\n")
    lines.append("| 조합 | 배경 | 전경 | 대비비 | 기준 | 결과 |")
    lines.append("|------|------|------|--------|------|------|")

    required_combos: list[tuple[str, str, str, float]] = []

    text_bgs = ["bgDeep", "panel", "card"]
    for fg_name in ("text", "sub"):
        for bg_name in text_bgs:
            required_combos.append((f"{fg_name} on {bg_name}", bg_name, fg_name, 4.5))

    severity_names = ["normal", "warn", "high", "critical", "info"]
    for sev in severity_names:
        for bg_name in ("bgDeep", "panel"):
            required_combos.append((f"{sev} as text on {bg_name}", bg_name, sev, 4.5))

    # 흰색을 각 심각도 채우기 위에.
    for sev in severity_names:
        required_combos.append((f"white on {sev} fill", sev, "__white__", 4.5))

    # onAccent on accent — 라이트.
    required_combos.append(("onAccent on accent (light)", "accent", "onAccent", 4.5))

    for label, bg_name, fg_name, threshold in required_combos:
        bg_hex = light[bg_name]
        fg_hex = "#FFFFFF" if fg_name == "__white__" else light[fg_name]
        ratio = contrast_ratio(bg_hex, fg_hex)
        passed = ratio >= threshold
        status = "PASS" if passed else "FAIL"
        lines.append(
            f"| {label} | `{bg_hex}` | `{fg_hex}` | {ratio:.2f}:1 | {threshold} | {status} |"
        )
        if not passed:
            required_failures.append(f"필수 대비 조합 미달: {label} = {ratio:.2f}:1 (기준 {threshold})")

    # 다크 — onAccent on accent 한 조합만 필수.
    lines.append("\n## (b-2) WCAG 2.1 대비비 (다크 팔레트, 회귀 방어용 1건만 필수)\n")
    lines.append("| 조합 | 배경 | 전경 | 대비비 | 기준 | 결과 |")
    lines.append("|------|------|------|--------|------|------|")
    dark_bg = dark["accent"]
    dark_fg = dark["onAccent"]
    dark_ratio = contrast_ratio(dark_bg, dark_fg)
    dark_pass = dark_ratio >= 4.5
    lines.append(
        f"| onAccent on accent (dark) | `{dark_bg}` | `{dark_fg}` | {dark_ratio:.2f}:1 | 4.5 | "
        f"{'PASS' if dark_pass else 'FAIL'} |"
    )
    if not dark_pass:
        required_failures.append(f"다크 onAccent-on-accent 회귀: {dark_ratio:.2f}:1 (기준 4.5)")
    lines.append(
        "\nD-08: 다크 전체 감사는 이 단계의 범위가 아니다. onAccent만 이 단계가 새로 "
        "도입하는 토큰이라 회귀 방어를 위해 넣는다."
    )

    # ── 참고 항목(실패해도 종료 코드에 영향 없음) ──────────────────────
    lines.append("\n## (b-3) 참고 항목 — 금지 용법의 근거 (종료 코드에 영향 없음)\n")
    lines.append("| 조합 | 배경 | 전경 | 대비비 | 비고 |")
    lines.append("|------|------|------|--------|------|")
    ref_combos = [
        ("accent as text on panel", "panel", "accent", "금지 용법 근거"),
        ("accent as text on bgDeep", "bgDeep", "accent", "금지 용법 근거"),
        ("white on accent fill", "accent", "__white__", "금지 용법 근거"),
        ("dark text on accent fill (light)", "accent", "text", "허용 용법 — 5.7x 근처가 나와야 정상"),
    ]
    for label, bg_name, fg_name, note in ref_combos:
        bg_hex = light[bg_name]
        fg_hex = "#FFFFFF" if fg_name == "__white__" else light[fg_name]
        ratio = contrast_ratio(bg_hex, fg_hex)
        lines.append(f"| {label} | `{bg_hex}` | `{fg_hex}` | {ratio:.2f}:1 | {note} |")

    # ── (c) 그레이스케일 명도 수렴 ──────────────────────────────────────
    lines.append("\n## (c) 심각도 5색 — 그레이스케일 명도 수렴 (D-09 근거)\n")
    lines.append("| 등급 | 색 | 상대휘도(L) |")
    lines.append("|------|-----|--------------|")
    lums = []
    for sev in severity_names:
        hex_val = light[sev]
        l = relative_luminance(hex_val)
        lums.append(l)
        lines.append(f"| {sev} | `{hex_val}` | {l:.4f} |")
    l_min, l_max = min(lums), max(lums)
    l_range = l_max - l_min
    lines.append(f"\n**명도 폭:** 최댓값 {l_max:.4f} − 최솟값 {l_min:.4f} = **{l_range:.4f}**")
    if l_range < 0.04:
        lines.append(
            "\n**판정: 색 단독 구분 불가 — 도형 병행 필수 (D-09).** 폭이 0.04 미만이라 "
            "그레이스케일로 캡처하면 5등급이 거의 같은 회색으로 보인다."
        )
    else:
        lines.append("\n판정: 명도 폭이 0.04 이상이라 색만으로도 구분 가능성이 있다.")

    # ── (d) 토큰 배선 감사 ────────────────────────────────────────────
    lines.append("\n## (d) 토큰 배선 감사 — QSS %(token) ↔ 치환 목록\n")

    if not THEMEMANAGER_CPP.exists():
        required_failures.append(f"{THEMEMANAGER_CPP} 파일이 없다")
    else:
        cpp_text = THEMEMANAGER_CPP.read_text(encoding="utf-8")
        substitute_targets = extract_substitution_targets(cpp_text, "QString ThemeManager::substitute")
        dialog_targets = extract_substitution_targets(cpp_text, "QString ThemeManager::dialogStyleSheet")

        # base.qss ↔ substitute()
        if BASE_QSS.exists():
            base_tokens = extract_qss_tokens(BASE_QSS.read_text(encoding="utf-8"))
            missing_in_substitute = sorted(base_tokens - substitute_targets - {"accentHover"})
            lines.append(
                f"- base.qss가 쓰는 토큰 {len(base_tokens)}개, substitute()의 치환 목록 "
                f"{len(substitute_targets)}개"
            )
            if missing_in_substitute:
                lines.append(
                    f"- FAIL: base.qss가 쓰지만 substitute()에 치환이 없는 토큰: {missing_in_substitute}"
                )
                required_failures.append(
                    f"base.qss 미배선 토큰: {missing_in_substitute}"
                )
            else:
                lines.append("- PASS: base.qss의 모든 %(token)이 substitute()에서 치환된다")
        else:
            required_failures.append(f"{BASE_QSS} 파일이 없다")

        # dialogs.qss ↔ dialogStyleSheet()
        if DIALOGS_QSS.exists():
            dialog_tokens = extract_qss_tokens(DIALOGS_QSS.read_text(encoding="utf-8"))
            missing_in_dialog = sorted(dialog_tokens - dialog_targets - {"accentHover"})
            lines.append(
                f"- dialogs.qss가 쓰는 토큰 {len(dialog_tokens)}개, dialogStyleSheet()의 치환 목록 "
                f"{len(dialog_targets)}개"
            )
            if missing_in_dialog:
                lines.append(
                    f"- FAIL: dialogs.qss가 쓰지만 dialogStyleSheet()에 치환이 없는 토큰: {missing_in_dialog}"
                )
                required_failures.append(
                    f"dialogs.qss 미배선 토큰: {missing_in_dialog}"
                )
            else:
                lines.append("- PASS: dialogs.qss의 모든 %(token)이 dialogStyleSheet()에서 치환된다")
        else:
            required_failures.append(f"{DIALOGS_QSS} 파일이 없다")

    ok = len(required_failures) == 0
    return AuditResult(ok, lines, required_failures)


# ─────────────────────────────────────────────────────────────────────────
#  문서 작성
# ─────────────────────────────────────────────────────────────────────────

DOC_HEADER = """# Design Tokens Contrast Audit

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
   아니라 "어느 침대인지" 식별용이라 이 감사 대상이 아니다. 02-04에서 ROI 침대 번호
   배지(`mainwindow.cpp`의 `dot`)의 인라인 스타일을 의도적으로 유지했다
   (`clickslider.cpp`와 같은 이유) — 배경이 이 배열에서 오고 영상 오버레이와 출처를
   공유해야 하므로 QSS `%(token)` 치환으로 옮기면 두 출처가 갈린다. 전경색만 흰색에서
   본문색 토큰(`kTextMain`)으로 바꿔 대비를 개선했다(실측: 흰 글씨 2.38~3.39:1 → 본문색
   4.33~6.15:1). 8색 중 3색(힐링 그린 4.47, 블루 4.44, 로즈 4.33)은 여전히 AA 4.5:1에
   근소하게 못 미친다 — 배경색 자체가 바뀔 수 없어 완전한 AA 보장은 불가능하며, 완전
   해소는 침대 식별 팔레트와 영상 오버레이를 함께 재설계해야 하는 별도 작업이다(A-10).
2. **영상 캔버스 위 안내 문구 색** — `videoSurface`(고정 어두운 표면) 전용 전경색이라
   팔레트 조합이 아니다.
3. **다크 팔레트 전반(D-08)** — 다크 테마는 이 단계에서 재설계하지 않는다. 회귀 방어를
   위해 `onAccent on accent` 한 조합만 다크에서도 검사한다.

---

"""


def write_doc(result: AuditResult) -> None:
    OUTPUT_DOC.parent.mkdir(parents=True, exist_ok=True)
    body = DOC_HEADER + "\n".join(result.lines) + "\n"
    if result.required_failures:
        body += "\n## 감사 실패 항목\n\n"
        for f in result.required_failures:
            body += f"- {f}\n"
    OUTPUT_DOC.write_text(body, encoding="utf-8")


def main() -> int:
    # Windows 콘솔 기본 코드페이지(cp949 등)가 한글·em-dash를 깨뜨리는 것을 막는다.
    # UTF-8을 강제 지원하지 않는 아주 오래된 런타임에서는 조용히 건너뛴다.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            try:
                stream.reconfigure(encoding="utf-8")
            except Exception:
                pass

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--write", action="store_true", help="docs/design-tokens-contrast-audit.md를 생성·갱신한다"
    )
    args = parser.parse_args()

    result = run_audit()

    for line in result.lines:
        print(line)

    if result.required_failures:
        print("\n=== 감사 실패 ===")
        for f in result.required_failures:
            print(f"- {f}")

    if args.write:
        write_doc(result)
        print(f"\n[theme_audit] {OUTPUT_DOC} 갱신됨")

    return 0 if result.ok else 1


if __name__ == "__main__":
    sys.exit(main())
