#!/usr/bin/env bash
# [진단] Gemini 왕복이 60초 무응답으로 멈추는 원인을 가른다.
#
# 지금까지 확인된 사실:
#   · 오류 응답(429/400)은 0.3~0.5초에 즉답으로 온다 → 연결·업로드·키는 정상.
#   · 진짜 생성 요청은 60초 동안 1바이트도 안 온다(curl 로도 재현) → 서버
#     바이너리 문제 아님.
# 오류 본문은 작아서 패킷 하나에 들어가고 생성 응답은 여러 패킷이라, "큰 응답만
# 못 받는 경로"인지부터 확인한다. 그게 아니면 모델·별칭 쪽을 본다.
#
#   ./tools/gemini_probe.sh config/cameras.conf
set -u

CONF="${1:-config/cameras.conf}"
if [ ! -f "$CONF" ]; then
    echo "설정 파일 없음: $CONF" >&2
    exit 1
fi

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

HOST="generativelanguage.googleapis.com"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
MAXT=25   # 케이스마다 25초 — 멈추는 건 어차피 60초를 넘겨도 안 온다

FMT='    연결 %{time_connect}s · 첫응답 %{time_starttransfer}s · 총 %{time_total}s · 보냄 %{size_upload}B · 받음 %{size_download}B · HTTP %{http_code}\n'

# POST 한 건. $1=설명 $2=본문파일 $3=URL, 나머지는 curl 추가 옵션.
post_case() {
    local label="$1" body="$2" url="$3"; shift 3
    local out="$TMP/out"
    rm -f "$out"
    echo "  $label"
    curl -sS -o "$out" -w "$FMT" --max-time "$MAXT" \
         -H 'Content-Type: application/json' -H 'Expect:' \
         "$@" --data-binary @"$body" "$url" || true
    if [ -s "$out" ]; then
        echo "    응답: $(head -c 300 "$out" | tr -d '\n')"
    else
        echo "    응답: (본문 없음 — 1바이트도 못 받음)"
    fi
}

url_for() { echo "https://${HOST}/v1beta/models/$1:generateContent?key=${KEY}"; }

cat > "$TMP/text.json" <<'EOF'
{"contents":[{"parts":[{"text":"한 단어로만 답하세요: 안녕"}]}],"generationConfig":{"maxOutputTokens":20}}
EOF

echo "== 설정 모델: $MODEL =="
echo
echo "[1] 텍스트 생성 — 같은 요청 2회 (일시적인지 확인)"
post_case "1차" "$TMP/text.json" "$(url_for "$MODEL")"
post_case "2차" "$TMP/text.json" "$(url_for "$MODEL")"
echo

echo "[2] 텍스트 생성 — IPv4 강제 (-4)"
# IPv6 경로에 MTU 블랙홀이 있으면 기본(v6 우선)만 멈추고 -4 는 통과한다.
post_case "IPv4" "$TMP/text.json" "$(url_for "$MODEL")" -4
echo

echo "[3] 큰 응답 수신 시험 — 모델 목록 (같은 호스트, 수십 KB GET)"
# ★ 이게 이번 진단의 핵심이다. 생성 요청과 달리 모델이 개입하지 않는데도
#   응답이 크다. 여기서 멈추면 "큰 응답을 못 받는 경로" 가 확정된다
#   (모델·쿼터와 무관). 잘 받아지면 경로는 무죄고 모델 쪽을 봐야 한다.
rm -f "$TMP/models.json"
curl -sS -o "$TMP/models.json" -w "$FMT" --max-time "$MAXT" \
     "https://${HOST}/v1beta/models?key=${KEY}" || true
if [ -s "$TMP/models.json" ]; then
    echo "    받은 크기: $(wc -c < "$TMP/models.json")B · 모델 수: $(grep -o '"name"' "$TMP/models.json" | wc -l)"
fi
echo

echo "[4] 모델 고정 — 별칭 대신 실제 id (별칭 문제 가르기)"
# gemini-flash-latest 는 별칭이라 어떤 실제 모델을 가리키는지 시기에 따라 바뀐다.
# 2026-08 실측: 별칭은 응답이 아예 안 오고, 2.5-flash 는 404 + "3.6-flash 를
# 쓰라"는 안내를 준다. 그래서 고정 id 로 3.6-flash 를 시험한다.
post_case "3.6-flash" "$TMP/text.json" "$(url_for gemini-3.6-flash)"
echo
echo "    (사용 가능한 flash 계열 모델 목록)"
if [ -s "$TMP/models.json" ]; then
    grep -o '"name": "models/[^"]*flash[^"]*"' "$TMP/models.json" |
        sed 's/.*models\//      /; s/"$//' | sort -u
fi
echo

echo "[5] 이미지 생성 — 진짜 JPEG 3장"
# ★ 이전 판은 난수 바이트를 JPEG 인 척 보내서 400(Unable to process input image)
#   을 받았다 — 그건 이 스크립트의 버그였지 서버 문제가 아니었다. 여기서는
#   ffmpeg 로 실제 디코딩되는 JPEG 를 만든다.
if command -v ffmpeg >/dev/null 2>&1; then
    ffmpeg -loglevel quiet -y -f lavfi -i testsrc=size=640x360:duration=1 \
           -frames:v 1 -q:v 7 "$TMP/f.jpg" </dev/null
fi
if [ -s "$TMP/f.jpg" ] && command -v python3 >/dev/null 2>&1; then
    python3 - "$TMP" <<'PY'
import base64, json, os, sys
d = sys.argv[1]
blob = open(os.path.join(d, "f.jpg"), "rb").read()
parts = [{"text": "이 사진 속 화면을 한국어 한 문장으로 설명하세요."}]
for _ in range(3):
    parts.append({"inline_data": {"mime_type": "image/jpeg",
                                  "data": base64.b64encode(blob).decode()}})
body = {"contents": [{"parts": parts}],
        "generationConfig": {"maxOutputTokens": 100}}
json.dump(body, open(os.path.join(d, "img.json"), "w"))
print("    JPEG %dB → 요청 %dB" % (len(blob), os.path.getsize(os.path.join(d, "img.json"))))
PY
    post_case "이미지 3장" "$TMP/img.json" "$(url_for "$MODEL")"
else
    echo "    ffmpeg 또는 python3 없음 — 건너뜀"
fi
echo

echo "[6] 경로·MTU 정보"
V4="$(getent ahostsv4 "$HOST" 2>/dev/null | awk '{print $1; exit}')"
V6="$(getent ahostsv6 "$HOST" 2>/dev/null | awk '{print $1; exit}')"
echo "    $HOST → IPv4 ${V4:-없음} / IPv6 ${V6:-없음}"
[ -n "$V4" ] && echo "    v4 경로: $(ip route get "$V4" 2>/dev/null | head -1)"
[ -n "$V6" ] && echo "    v6 경로: $(ip -6 route get "$V6" 2>/dev/null | head -1)"
ip -o link show 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="mtu") print "    " $2 " mtu " $(i+1)}'

cat <<'EOF'

── 판독법 ─────────────────────────────────────────────
· [3] 이 멈춘다        → 큰 응답을 못 받는 경로다(모델·쿼터 무관). [6]의 MTU 와
                         [2] 결과를 같이 볼 것.
· [2] -4 만 성공       → IPv6 경로 문제. 코드에서 IPv4 를 강제하면 해결된다.
· [3] 성공 + [1] 멈춤  → 경로는 정상. 생성 요청만 응답이 안 오는 것이므로
                         [4] 로 별칭/모델을 갈라볼 것.
· [4] 만 성공          → 설정된 모델(별칭)이 문제. cameras.conf 의 gemini_model 을
                         [4]에서 통한 고정 id 로 바꾼다.
· [1] 1차 멈춤 + 2차 성공 → 일시적. 코드에 타임아웃 재시도를 넣으면 넘어간다.
· [5] 만 멈춤          → 이미지 요청 특유의 문제(응답 크기가 아니라 처리 시간).
──────────────────────────────────────────────────────
EOF
