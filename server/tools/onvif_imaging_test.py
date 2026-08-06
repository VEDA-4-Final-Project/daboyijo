#!/usr/bin/env python3
"""ONVIF Imaging 테스트 (의존성 없음 — 파이썬 표준 라이브러리만).

onvif-zeep 라이브러리의 WSDL 패키징 버그를 피하려고, SOAP XML을 직접 만들어
HTTP로 쏜다. 이 방식이 우리 C++ 서버가 하게 될 것(libcurl + 직접 SOAP)과 동일.

동작 순서(= 서버 핸들러가 하게 될 일):
  ⓪ GetSystemDateAndTime  → 카메라 시각을 읽어 인증에 사용(시계 어긋남 회피, 무인증)
  ① GetCapabilities       → Media/Imaging 서비스 주소(XAddr) 찾기
  ② GetVideoSources       → VideoSourceToken 얻기
  ③ GetOptions            → 각 항목의 실제 범위(Min~Max) 확인
  ④ 0~100 정규화 값을 그 범위로 환산
  ⑤ SetImagingSettings    → 카메라에 적용 후 재조회

사용 예:
    # (1) 조회만 (현재값·범위 확인)
    python onvif_imaging_test.py --ip 172.20.32.6 --user admin --pw '5hanwha!'

    # (2) 밝기 70%, 채도 40% 적용 (0~100 정규화 — 자동 환산)
    python onvif_imaging_test.py --ip 172.20.32.6 --user admin --pw '5hanwha!' \
        --brightness 70 --saturation 40

    # (3) 카메라 실제 단위값 그대로 넣기
    python onvif_imaging_test.py --ip 172.20.32.6 --user admin --pw '5hanwha!' \
        --brightness 200 --absolute
"""

import argparse
import base64
import datetime
import hashlib
import os
import sys
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from urllib.parse import urlparse, urlunparse

NS_S    = "http://www.w3.org/2003/05/soap-envelope"
NS_TDS  = "http://www.onvif.org/ver10/device/wsdl"
NS_TRT  = "http://www.onvif.org/ver10/media/wsdl"
NS_TIMG = "http://www.onvif.org/ver20/imaging/wsdl"
NS_TPTZ = "http://www.onvif.org/ver20/ptz/wsdl"
NS_TT   = "http://www.onvif.org/ver10/schema"

# ImagingSettings 자식 요소는 스키마상 "정해진 순서"로 보내야 카메라가 안 튕긴다.
# (Brightness, ColorSaturation, Contrast, Sharpness 순 — 우리가 쓰는 4개 기준)
SCHEMA_ORDER = ["Brightness", "ColorSaturation", "Contrast", "Sharpness"]
# CLI 인자명 → ONVIF 요소명 매핑
ARG_TO_ONVIF = {
    "brightness": "Brightness",
    "saturation": "ColorSaturation",
    "contrast":   "Contrast",
    "sharpness":  "Sharpness",
}


def localname(tag):
    return tag.split("}")[-1]


def find_first(root, name):
    for el in root.iter():
        if localname(el.tag) == name:
            return el
    return None


def dump_tree(el, depth=0):
    """요소 트리를 네임스페이스 접두사 없이 들여쓰기로 출력(값·속성 포함)."""
    txt = (el.text or "").strip()
    attrs = " ".join(f'{localname(k)}="{v}"' for k, v in el.attrib.items())
    head = localname(el.tag) + (f"  [{attrs}]" if attrs else "")
    print("    " + "  " * depth + head + (f" = {txt}" if txt else ""))
    for c in el:
        dump_tree(c, depth + 1)


def post(url, body, timeout=8):
    """SOAP 요청 전송. (status, text) 반환. 오류 응답도 본문을 그대로 돌려준다."""
    req = urllib.request.Request(
        url, data=body.encode("utf-8"),
        headers={"Content-Type": "application/soap+xml; charset=utf-8"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def envelope(body_inner, user=None, pw=None, created=None):
    """SOAP 봉투 생성. user/pw 주면 WS-Security UsernameToken(digest) 헤더 추가."""
    header = ""
    if user is not None:
        nonce = os.urandom(16)
        if created is None:
            created = datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ")
        digest = base64.b64encode(
            hashlib.sha1(nonce + created.encode() + pw.encode()).digest()).decode()
        n64 = base64.b64encode(nonce).decode()
        wsse = ("http://docs.oasis-open.org/wss/2004/01/"
                "oasis-200401-wss-wssecurity-secext-1.0.xsd")
        wsu = ("http://docs.oasis-open.org/wss/2004/01/"
               "oasis-200401-wss-wssecurity-utility-1.0.xsd")
        pt = ("http://docs.oasis-open.org/wss/2004/01/"
              "oasis-200401-wss-username-token-profile-1.0#PasswordDigest")
        enc = ("http://docs.oasis-open.org/wss/2004/01/"
               "oasis-200401-wss-soap-message-security-1.0#Base64Binary")
        header = (
            f'<s:Header><Security s:mustUnderstand="1" xmlns="{wsse}">'
            f"<UsernameToken><Username>{user}</Username>"
            f'<Password Type="{pt}">{digest}</Password>'
            f'<Nonce EncodingType="{enc}">{n64}</Nonce>'
            f'<Created xmlns="{wsu}">{created}</Created>'
            f"</UsernameToken></Security></s:Header>")
    return (
        '<?xml version="1.0" encoding="UTF-8"?>'
        f'<s:Envelope xmlns:s="{NS_S}" xmlns:tds="{NS_TDS}" xmlns:trt="{NS_TRT}" '
        f'xmlns:timg="{NS_TIMG}" xmlns:tptz="{NS_TPTZ}" xmlns:tt="{NS_TT}">'
        f"{header}<s:Body>{body_inner}</s:Body></s:Envelope>")


def fix_host(xaddr, ip, port):
    """카메라가 돌려준 XAddr의 host를 우리가 접속한 IP로 교체(내부호스트/0.0.0.0 회피)."""
    if not xaddr:
        return None
    p = urlparse(xaddr)
    netloc = ip if port == 80 else f"{ip}:{port}"
    return urlunparse((p.scheme or "http", netloc, p.path, "", "", ""))


def get_camera_created(dev_url):
    """카메라 UTC 시각을 읽어 Created 문자열로. (무인증 요청) 실패 시 None."""
    st, resp = post(dev_url, envelope("<tds:GetSystemDateAndTime/>"))
    try:
        root = ET.fromstring(resp)
        utc = find_first(root, "UTCDateTime")
        d, t = find_first(utc, "Date"), find_first(utc, "Time")
        y = int(find_first(d, "Year").text);  mo = int(find_first(d, "Month").text)
        da = int(find_first(d, "Day").text);   h = int(find_first(t, "Hour").text)
        mi = int(find_first(t, "Minute").text); se = int(find_first(t, "Second").text)
        return f"{y:04d}-{mo:02d}-{da:02d}T{h:02d}:{mi:02d}:{se:02d}Z"
    except Exception:
        return None


def fault_text(resp):
    """SOAP Fault 안의 사람이 읽을 사유 텍스트 추출(없으면 응답 앞부분)."""
    try:
        root = ET.fromstring(resp)
        for name in ("Text", "Reason", "faultstring"):
            el = find_first(root, name)
            if el is not None and el.text:
                return el.text.strip()
    except Exception:
        pass
    return resp[:300]


def main():
    ap = argparse.ArgumentParser(description="ONVIF Imaging 조절 테스트 (무의존)")
    ap.add_argument("--ip", required=True)
    ap.add_argument("--user", required=True)
    ap.add_argument("--pw", required=True)
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--brightness", type=float)
    ap.add_argument("--contrast", type=float)
    ap.add_argument("--saturation", type=float)
    ap.add_argument("--sharpness", type=float)
    ap.add_argument("--absolute", action="store_true",
                    help="값을 0~100 정규화가 아니라 카메라 실제 단위로 그대로 사용")
    ap.add_argument("--raw", action="store_true",
                    help="전체 옵션의 원본 XML까지 그대로 출력")
    args = ap.parse_args()

    base = f"http://{args.ip}:{args.port}" if args.port != 80 else f"http://{args.ip}"
    dev_url = f"{base}/onvif/device_service"
    print(f"[연결] {dev_url}  (user={args.user})")

    # ⓪ 카메라 시각(인증용)
    created = get_camera_created(dev_url)
    print(f"[시각] 카메라 UTC = {created or '읽기 실패(로컬 시각 사용)'}")

    def call(url, inner, step, return_raw=False):
        st, resp = post(url, envelope(inner, args.user, args.pw, created))
        if st != 200:
            print(f"[실패] {step} (HTTP {st}): {fault_text(resp)}")
            if st in (400, 401):
                print("       → 계정/비번 확인. 반복되면 PC·카메라 시각 동기화 확인.")
            sys.exit(1)
        root = ET.fromstring(resp)
        return (root, resp) if return_raw else root

    # ① 서비스 주소 찾기
    root = call(dev_url, "<tds:GetCapabilities><tds:Category>All</tds:Category>"
                         "</tds:GetCapabilities>", "GetCapabilities")
    media_url = imaging_url = ptz_url = None
    for el in root.iter():
        ln = localname(el.tag)
        if ln == "Media":
            x = find_first(el, "XAddr");  media_url = x.text if x is not None else media_url
        elif ln == "Imaging":
            x = find_first(el, "XAddr");  imaging_url = x.text if x is not None else imaging_url
        elif ln == "PTZ":
            x = find_first(el, "XAddr");  ptz_url = x.text if x is not None else ptz_url
    media_url = fix_host(media_url, args.ip, args.port) or dev_url
    imaging_url = fix_host(imaging_url, args.ip, args.port) or dev_url
    ptz_url = fix_host(ptz_url, args.ip, args.port) if ptz_url else None
    print(f"[주소] Media   = {media_url}")
    print(f"[주소] Imaging = {imaging_url}")
    print(f"[주소] PTZ     = {ptz_url or '(없음)'}")

    # ② VideoSource 토큰
    root = call(media_url, "<trt:GetVideoSources/>", "GetVideoSources")
    token = None
    for el in root.iter():
        if localname(el.tag) == "VideoSources" and el.get("token"):
            token = el.get("token");  break
    if not token:
        vs = find_first(root, "VideoSources")
        token = vs.get("token") if vs is not None else None
    if not token:
        sys.exit("[실패] VideoSourceToken 을 못 찾음")
    print(f"[토큰] VideoSourceToken = {token}")

    # ③ 범위 + 현재값
    opt_root, opt_raw = call(imaging_url,
                    f"<timg:GetOptions><timg:VideoSourceToken>{token}"
                    f"</timg:VideoSourceToken></timg:GetOptions>", "GetOptions",
                    return_raw=True)
    cur_root = call(imaging_url,
                    f"<timg:GetImagingSettings><timg:VideoSourceToken>{token}"
                    f"</timg:VideoSourceToken></timg:GetImagingSettings>",
                    "GetImagingSettings")

    # ── 카메라가 지원하는 "전체" 옵션 (우리가 쓰는 4개 말고 전부) ──
    opts_el = find_first(opt_root, "ImagingOptions")
    if opts_el is None:  # 일부 카메라는 ImagingOptions20 이름을 쓸 수 있음
        opts_el = find_first(opt_root, "GetOptionsResponse")
    print("\n[전체 지원 옵션 — 카메라가 돌려준 것 전부]")
    if opts_el is not None:
        top = [localname(c.tag) for c in opts_el]
        print("  ▸ 지원 항목:", ", ".join(top) if top else "(없음)")
        print("  ▸ 상세(값/범위):")
        for c in opts_el:
            dump_tree(c, 0)
    else:
        print("  ImagingOptions 를 못 찾음 — --raw 로 원본 XML 확인")

    if args.raw:
        print("\n[원본 XML — GetOptions 응답]")
        print(opt_raw)

    ranges = {}
    print("\n[우리가 쓰는 항목: 현재값 / 지원 범위]")
    for name in SCHEMA_ORDER:
        opt_el = find_first(opt_root, name)
        rng = "미지원"
        if opt_el is not None:
            mn, mx = find_first(opt_el, "Min"), find_first(opt_el, "Max")
            if mn is not None and mx is not None:
                ranges[name] = (float(mn.text), float(mx.text))
                rng = f"{mn.text} ~ {mx.text}"
        cur_el = find_first(cur_root, name)
        cur_v = cur_el.text if cur_el is not None else "-"
        print(f"  {name:16s} 현재={str(cur_v):>8}   범위={rng}")

    # ── 초점(Focus) 조절 가능 여부 (GetMoveOptions) ──
    # 초점은 '설정값'이 아니라 렌즈 모터 이동(Move)이라 별도 명령으로 확인한다.
    # 고정초점 렌즈 카메라는 여기서 Fault 또는 빈 MoveOptions를 돌려준다.
    print("\n[초점(Focus) 원격 조절 가능 여부]")
    fst, fresp = post(imaging_url,
        envelope(f"<timg:GetMoveOptions><timg:VideoSourceToken>{token}"
                 f"</timg:VideoSourceToken></timg:GetMoveOptions>",
                 args.user, args.pw, created))
    if fst != 200:
        print(f"  ❌ 불가 — GetMoveOptions HTTP {fst}: {fault_text(fresp)}")
        print("     → 고정초점 렌즈이거나 초점 원격제어 미지원")
    else:
        mv = find_first(ET.fromstring(fresp), "MoveOptions")
        modes = [localname(c.tag) for c in mv] if mv is not None else []
        if modes:
            print(f"  ✅ 가능! 지원 방식: {', '.join(modes)}")
            for c in mv:
                dump_tree(c, 0)
            if args.raw:
                print("\n[원본 XML — GetMoveOptions 응답]")
                print(fresp)
        else:
            print("  ❌ 불가 — MoveOptions 비어 있음 (고정초점 렌즈)")

    # ── 확대/축소(Zoom) 가능 여부 (PTZ 서비스) ──
    # 줌은 Imaging이 아니라 PTZ 서비스 소관. PTZ 노드의 SupportedPTZSpaces에
    # "Zoom" 공간이 있으면 확대/축소 가능. 고정형 카메라는 PTZ 서비스 자체가 없다.
    print("\n[확대/축소(Zoom) — PTZ 지원 여부]")
    if not ptz_url:
        print("  ❌ PTZ 서비스 없음 — 확대/축소 불가 (고정형 카메라)")
    else:
        zst, zresp = post(ptz_url, envelope("<tptz:GetNodes/>",
                                            args.user, args.pw, created))
        if zst != 200:
            print(f"  ❌ PTZ 조회 실패 (HTTP {zst}: {fault_text(zresp)})")
        else:
            zroot = ET.fromstring(zresp)
            zoom_spaces = sorted({localname(el.tag) for el in zroot.iter()
                                  if "Zoom" in localname(el.tag)})
            if zoom_spaces:
                print(f"  ✅ 확대/축소 가능! 지원 공간: {', '.join(zoom_spaces)}")
            else:
                print("  ❌ PTZ는 있으나 Zoom 미지원 (팬/틸트 전용이거나 줌 없음)")
            if args.raw:
                print("\n[원본 XML — GetNodes 응답]")
                print(zresp)

    # ④ 적용값 계산
    wanted = {"Brightness": args.brightness, "Contrast": args.contrast,
              "ColorSaturation": args.saturation, "Sharpness": args.sharpness}
    changes = {}
    for name in SCHEMA_ORDER:
        v = wanted.get(name)
        if v is None:
            continue
        if args.absolute:
            changes[name] = v
        elif name in ranges:
            lo, hi = ranges[name]
            changes[name] = round(lo + (v / 100.0) * (hi - lo), 3)
        else:
            print(f"  ! {name} 미지원 — 건너뜀")

    if not changes:
        print("\n(적용할 값 없음 — 조회만 했습니다. --brightness 70 처럼 주면 실제로 바꿉니다.)")
        return

    # ⑤ 적용 (스키마 순서대로) + 재조회
    print("\n[적용]")
    inner = ""
    for name in SCHEMA_ORDER:
        if name in changes:
            print(f"  {name} → {changes[name]}")
            inner += f"<tt:{name}>{changes[name]}</tt:{name}>"
    call(imaging_url,
         f"<timg:SetImagingSettings><timg:VideoSourceToken>{token}</timg:VideoSourceToken>"
         f"<timg:ImagingSettings>{inner}</timg:ImagingSettings>"
         f"<timg:ForcePersistence>true</timg:ForcePersistence></timg:SetImagingSettings>",
         "SetImagingSettings")
    print("  ✓ 전송 완료")

    after = call(imaging_url,
                 f"<timg:GetImagingSettings><timg:VideoSourceToken>{token}"
                 f"</timg:VideoSourceToken></timg:GetImagingSettings>", "GetImagingSettings(재확인)")
    print("\n[적용 후 재조회]")
    for name in changes:
        el = find_first(after, name)
        print(f"  {name:16s} = {el.text if el is not None else '-'}")

    print("\n끝. RTSP/웹뷰어에서 실제로 바뀌었는지 눈으로 확인하세요.")


if __name__ == "__main__":
    main()
