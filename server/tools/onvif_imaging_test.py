#!/usr/bin/env python3
"""ONVIF Imaging 테스트 — 한화 카메라 밝기/대비/채도/샤프니스 조절 확인용.

우리 A안(서버가 카메라에 이미지 파라미터를 적용)의 실제 동작을 C++로 옮기기 전에,
"우리 카메라에 진짜 되는지 / 값 범위가 얼마인지 / 토큰명이 뭔지"를 먼저 확인하는 용도.

동작 순서(= 서버 핸들러가 하게 될 일과 동일):
  ① Media.GetVideoSources        → VideoSourceToken 얻기
  ② Imaging.GetOptions           → 각 항목의 실제 범위(Min~Max) 확인
  ③ 0~100 정규화 값을 그 범위로 환산
  ④ Imaging.SetImagingSettings   → 카메라에 적용
  ⑤ 다시 조회해서 반영됐는지 확인

설치:
    pip install onvif-zeep

사용 예:
    # (1) 아무것도 안 바꾸고 현재값·지원범위만 조회
    python onvif_imaging_test.py --ip 172.20.35.140 --user admin --pw 'PASSWORD'

    # (2) 밝기 70%, 채도 40% 로 적용 (값은 0~100 정규화 — 서버가 하듯 자동 환산)
    python onvif_imaging_test.py --ip 172.20.35.140 --user admin --pw 'PASSWORD' \
        --brightness 70 --saturation 40

    # (3) 카메라 실제 단위값을 그대로 넣고 싶으면 --absolute
    python onvif_imaging_test.py --ip 172.20.35.140 --user admin --pw 'PASSWORD' \
        --brightness 200 --absolute

참고:
  · 인증 실패가 나면 PC와 카메라의 "시각"이 많이 어긋난 경우가 흔함(ONVIF digest는
    시간에 민감). PC 시계를 맞추거나 NTP 동기화 후 재시도.
  · 노출(Exposure)은 단순 숫자가 아니라 Mode/ExposureTime 구조체라 이 스크립트엔
    포함 안 함 — 먼저 이 4개(밝기/대비/채도/샤프니스)가 되는지부터 확인.
"""

import argparse
import sys

try:
    from onvif import ONVIFCamera
except ImportError:
    sys.exit("onvif-zeep 가 필요합니다:  pip install onvif-zeep")


def pct_to_range(pct, opt):
    """0~100 정규화 값을 카메라 실제 범위(opt.Min~opt.Max)로 환산."""
    if opt is None or getattr(opt, "Min", None) is None or getattr(opt, "Max", None) is None:
        return None
    lo, hi = float(opt.Min), float(opt.Max)
    return round(lo + (pct / 100.0) * (hi - lo), 3)


def fmt_range(opt):
    if opt is None or getattr(opt, "Min", None) is None:
        return "미지원"
    return f"{opt.Min} ~ {opt.Max}"


def main():
    ap = argparse.ArgumentParser(description="ONVIF Imaging 조절 테스트")
    ap.add_argument("--ip", required=True, help="카메라 IP")
    ap.add_argument("--user", required=True, help="카메라 계정 (보통 admin)")
    ap.add_argument("--pw", required=True, help="카메라 비밀번호")
    ap.add_argument("--port", type=int, default=80, help="ONVIF 포트 (기본 80)")
    ap.add_argument("--brightness", type=float, help="밝기 0~100 (--absolute면 실제단위)")
    ap.add_argument("--contrast", type=float, help="대비 0~100")
    ap.add_argument("--saturation", type=float, help="채도 0~100")
    ap.add_argument("--sharpness", type=float, help="샤프니스 0~100")
    ap.add_argument("--absolute", action="store_true",
                    help="값을 0~100 정규화가 아니라 카메라 실제 단위로 그대로 사용")
    args = ap.parse_args()

    # ─── 연결 ───
    print(f"[연결] {args.ip}:{args.port}  (user={args.user})")
    try:
        cam = ONVIFCamera(args.ip, args.port, args.user, args.pw)
        media = cam.create_media_service()
        imaging = cam.create_imaging_service()
    except Exception as e:
        sys.exit(f"[실패] 연결/서비스 생성 오류: {e}\n"
                 f"       → IP/포트/계정 확인, 그리고 PC·카메라 시각 동기화 확인")

    # ─── ① VideoSource 토큰 ───
    sources = media.GetVideoSources()
    if not sources:
        sys.exit("[실패] VideoSource 를 못 찾음")
    token = sources[0].token
    print(f"[토큰] VideoSourceToken = {token}")

    # ─── ② 범위(Options) + 현재값 ───
    opts = imaging.GetOptions({"VideoSourceToken": token})
    cur = imaging.GetImagingSettings({"VideoSourceToken": token})

    rows = [
        ("Brightness",      "brightness", args.brightness),
        ("Contrast",        "contrast",   args.contrast),
        ("ColorSaturation", "saturation", args.saturation),
        ("Sharpness",       "sharpness",  args.sharpness),
    ]

    print("\n[현재 설정 / 지원 범위]")
    for onvif_name, _, _ in rows:
        o = getattr(opts, onvif_name, None)
        c = getattr(cur, onvif_name, None)
        print(f"  {onvif_name:16s} 현재={str(c):>8}   범위={fmt_range(o)}")

    # ─── ③ 적용할 값 계산 ───
    changes = {}
    for onvif_name, _, val in rows:
        if val is None:
            continue
        o = getattr(opts, onvif_name, None)
        if args.absolute:
            changes[onvif_name] = val
        else:
            mapped = pct_to_range(val, o)
            if mapped is None:
                print(f"  ! {onvif_name} 미지원 — 건너뜀")
                continue
            changes[onvif_name] = mapped

    if not changes:
        print("\n(적용할 값 없음 — 조회만 했습니다. "
              "--brightness 70 처럼 값을 주면 실제로 바꿔봅니다.)")
        return

    # ─── ④ 적용 ───
    print("\n[적용]")
    for k, v in changes.items():
        setattr(cur, k, v)
        print(f"  {k} → {v}")

    try:
        req = imaging.create_type("SetImagingSettings")
        req.VideoSourceToken = token
        req.ImagingSettings = cur
        req.ForcePersistence = True
        imaging.SetImagingSettings(req)
        print("  ✓ 전송 완료")
    except Exception as e:
        sys.exit(f"[실패] SetImagingSettings 오류: {e}")

    # ─── ⑤ 재확인 ───
    after = imaging.GetImagingSettings({"VideoSourceToken": token})
    print("\n[적용 후 재조회]")
    for k in changes:
        print(f"  {k:16s} = {getattr(after, k, None)}")

    print("\n끝. 카메라 화면(RTSP/웹뷰어)에서 실제로 바뀌었는지 눈으로 확인하세요.")


if __name__ == "__main__":
    main()
