#!/usr/bin/env bash
# [진단] Gemini 왕복을 서버 바이너리 밖에서 그대로 재현한다.
#
# "지금 상황 보기"만 타임아웃나고 영상 검색(텍스트 질의)은 되는 상황에서,
# 원인이 우리 코드인지 회선/중간 장비인지 가르는 게 목적이다. 서버와 같은
# 라즈베리파이에서 돌릴 것.
#
#   ./tools/gemini_probe.sh config/cameras.conf
#
# 출력의 판독법은 맨 아래 주석 참고.
set -u

CONF="${1:-config/cameras.conf}"
if [ ! -f "$CONF" ]; then
    echo "설정 파일 없음: $CONF" >&2
    exit 1
fi

# cameras.conf 는 key=value 한 줄 형식(주석 #). 필요한 두 개만 뽑는다.
getconf_val() {
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\(.*\)$/\1/p" "$CONF" | tail -1 | tr -d '\r'
}
KEY="$(getconf_val gemini_api_key)"
MODEL="$(getconf_val gemini_model)"
[ -z "$MODEL" ] && MODEL="gemini-flash-latest"

if [ -z "$KEY" ]; then
    echo "gemini_api_key 가 비어 있음 — $CONF 확인" >&2
    exit 1
fi

URL="https://generativelanguage.googleapis.com/v1beta/models/${MODEL}:generateContent?key=${KEY}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# -w 로 구간별 시간을 뽑는다. size_upload 가 요청 크기에 못 미치면 업로드에서 막힌 것.
FMT='  연결 %{time_connect}s · TLS %{time_appconnect}s · 요청전송완료 %{time_pretransfer}s\n  첫응답 %{time_starttransfer}s · 총 %{time_total}s · 보낸바이트 %{size_upload} · HTTP %{http_code}\n'

echo "== 모델: $MODEL =="
echo
echo "[1/3] 텍스트 질의 (영상 검색이 쓰는 경로, ~1KB)"
cat > "$TMP/text.json" <<'EOF'
{"contents":[{"parts":[{"text":"한 단어로만 답하세요: 안녕"}]}]}
EOF
curl -sS -o "$TMP/text.out" -w "$FMT" \
     -H 'Content-Type: application/json' -H 'Expect:' \
     --max-time 60 --data-binary @"$TMP/text.json" "$URL"
echo "  응답: $(head -c 700 "$TMP/text.out" | tr -d '\n')"
echo

echo "[2/3] 이미지 질의 (지금 상황 보기가 쓰는 경로, ~90KB)"
# 서버와 같은 규격(640px q70)의 더미 JPEG 3장을 만들어 붙인다. 실제 스냅샷이
# 아니어도 크기·경로가 같아 전송 특성은 동일하다.
if command -v python3 >/dev/null 2>&1; then
    python3 - "$TMP" <<'PY'
import base64, json, sys, os
d = sys.argv[1]
try:
    import cv2, numpy as np
    img = (np.random.rand(360, 640, 3) * 255).astype('uint8')
    ok, buf = cv2.imencode('.jpg', img, [cv2.IMWRITE_JPEG_QUALITY, 70])
    blob = buf.tobytes()
except Exception:
    blob = os.urandom(22000)          # OpenCV 없으면 크기만 맞춘 더미
parts = [{"text": "이 사진들 속 상황을 한국어 두 문장으로 설명하세요."}]
for _ in range(3):
    parts.append({"inline_data": {"mime_type": "image/jpeg",
                                  "data": base64.b64encode(blob).decode()}})
json.dump({"contents": [{"parts": parts}]}, open(os.path.join(d, "img.json"), "w"))
print("  요청 크기: %d bytes" % os.path.getsize(os.path.join(d, "img.json")))
PY
else
    echo "  python3 없음 — 이 단계 건너뜀" >&2
fi
if [ -f "$TMP/img.json" ]; then
    curl -sS -o "$TMP/img.out" -w "$FMT" \
         -H 'Content-Type: application/json' -H 'Expect:' \
         --max-time 90 --data-binary @"$TMP/img.json" "$URL"
    echo "  응답: $(head -c 700 "$TMP/img.out" | tr -d '\n')"
fi
echo

echo "[3/3] 경로 MTU 확인 (큰 패킷만 죽는 회선인지)"
# 작은 요청은 되는데 큰 요청만 멈추는 전형적 원인이 PMTU 블랙홀이다.
# 1472 실패 + 1400 성공이면 MTU 를 낮춰야 한다.
for sz in 1472 1400 1300; do
    if ping -c 2 -W 3 -M do -s "$sz" 8.8.8.8 >/dev/null 2>&1; then
        echo "  payload ${sz}B (총 $((sz + 28))B): OK"
    else
        echo "  payload ${sz}B (총 $((sz + 28))B): 실패"
    fi
done
echo "  현재 인터페이스 MTU:"
ip -o link show 2>/dev/null | awk '{print "   ", $2, $0}' | grep -o 'mtu [0-9]*' | sort -u

cat <<'EOF'

── 판독법 ─────────────────────────────────────────────
· [1] 성공 + [2] 타임아웃  → 큰 POST만 죽는 것. [3]에서 1472 실패면
      경로 MTU 문제(라우터/테더링). MTU 를 1400 으로 낮춰 재시도:
        sudo ip link set dev wlan0 mtu 1400
· [1][2] 모두 성공         → 회선은 정상. 서버 바이너리 쪽 문제이므로
      [Gemini] 로그의 "업로드 x/yB · 첫응답" 값을 같이 볼 것.
· [1][2] 모두 실패         → 키·쿼터·차단. 응답 본문에 사유가 찍힌다.
· [2]만 HTTP 429/400       → 타임아웃이 아니라 쿼터·요청 형식 문제.
──────────────────────────────────────────────────────
EOF
