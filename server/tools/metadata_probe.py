#!/usr/bin/env python3
"""ONVIF 메타데이터 탐색 도구 (개발용, 배포 대상 아님).

카메라 RTSP 스트림의 data 트랙을 받아 원시 페이로드를 그대로 출력한다.
목적: WiseAI가 실제로 어떤 메타데이터(바운딩 박스/클래스/이벤트)를
XML로 내보내는지 눈으로 확인하는 것.

의존성 없음(표준 라이브러리 + 시스템 ffmpeg). RPi/PC 어디서든:
    python3 metadata_probe.py <채널> [초]
예:
    python3 metadata_probe.py 3 15
"""

import subprocess
import sys
import re

CAM_IP = "172.20.35.140"
CAM_USER = "admin"
CAM_PASS = "5hanwha!"

def rtsp_url(ch: int) -> str:
    return f"rtsp://{CAM_USER}:{CAM_PASS}@{CAM_IP}:554/{ch}/profile2/media.smp"

def main():
    ch = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    secs = int(sys.argv[2]) if len(sys.argv) > 2 else 15
    url = rtsp_url(ch)

    print(f"[probe] CH{ch} 메타데이터 트랙 덤프 ({secs}초)")
    print(f"[probe] {url}\n")

    # data 트랙만 골라 raw로 뽑는다. 영상(video)은 버리고 metadata만.
    # -map 0:d = data 스트림들, -c copy = 재인코딩 없이 원본 그대로
    cmd = [
        "ffmpeg", "-loglevel", "error",
        "-rtsp_transport", "tcp",
        "-i", url,
        "-map", "0:d?",           # data 트랙 (있으면)
        "-c", "copy",
        "-f", "data",             # 컨테이너 없이 원시 바이트
        "-t", str(secs),
        "pipe:1",
    ]

    proc = subprocess.run(cmd, capture_output=True)
    raw = proc.stdout

    if not raw:
        print("[probe] data 트랙에서 받은 바이트 없음.")
        print("[probe] → 이 프로파일엔 메타데이터가 없거나, ONVIF PullPoint 방식일 수 있음.")
        if proc.stderr:
            print("[ffmpeg]", proc.stderr.decode(errors="replace")[:500])
        return

    print(f"[probe] {len(raw)} 바이트 수신")

    text = raw.decode("utf-8", errors="replace")

    # 전체 XML을 파일로 저장 (자세히 분석용)
    out_path = f"meta_ch{ch}_full.xml"
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"[probe] 전체 XML 저장: {out_path}\n")

    # 이번 샘플에 등장한 이벤트 토픽 종류 집계
    topics = re.findall(r"<wsnt:Topic[^>]*>([^<]+)", text)
    topic_counts = {}
    for t in topics:
        t = t.strip()
        topic_counts[t] = topic_counts.get(t, 0) + 1
    print("===== 등장한 이벤트 토픽 (횟수) =====")
    for t, c in sorted(topic_counts.items(), key=lambda x: -x[1]):
        print(f"  {c:3d}  {t}")

    # 관심 항목: 사람 객체 / 바운딩 박스 / 상태 true
    print("\n===== 사람/객체/바운딩박스 관련 라인 =====")
    keys = ("ObjectId", "BoundingBox", "CenterOfGravity", "humanbody",
            "ObjectDetection", "Value=\"true\"", "Value=\"1\"", "Class", "Type")
    hits = [ln.strip() for ln in text.splitlines()
            if any(k in ln for k in keys)]
    if hits:
        for ln in hits[:60]:
            print(ln)
    else:
        print("  (이번 샘플엔 없음 — 카메라 앞에서 사람이 움직이는 동안 다시 실행)")

if __name__ == "__main__":
    main()
