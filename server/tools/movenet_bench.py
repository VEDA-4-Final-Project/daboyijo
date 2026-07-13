#!/usr/bin/env python3
"""MoveNet 추론 속도 벤치 — 이 라즈베리파이에서 쓸 만한지 확인용.

더미(무작위) 이미지로 '순수 추론 시간'만 잰다. 카메라·전처리·후처리는 없음.
목적: C++/Python 통합 방식을 정하기 전에, RPi 4에서 MoveNet이 몇 ms / 몇 fps인지
      실측해서 "4채널 하이브리드가 현실적인가"를 판단한다.

────────────────────────────────────────────────────────────────────
사용법 (라즈베리파이, aarch64):
    pip install tflite-runtime numpy
    python3 movenet_bench.py --model movenet_lightning_int8.tflite
    python3 movenet_bench.py --model ... --threads 4 --runs 200

  * tflite-runtime 설치가 막히면(파이썬 버전 문제 등):
      pip install --index-url https://www.piwheels.org/simple tflite-runtime
    그래도 안 되면 최후수단: pip install tensorflow (무겁지만 lite 포함)

모델 구하기 (Lightning int8 권장 — 가장 가벼움):
    Kaggle "google/movenet" → SinglePose Lightning → tfLite → int8 → .tflite 다운로드
    받은 파일 경로를 --model 로 넘기면 됨. (Thunder는 더 정확하지만 느림)

판단 기준 (평균 추론 시간):
    ~30ms 이하   여유 있음 → C++ 통합도 편하게 감
    30~80ms      저프레임·라운드로빈 필요하지만 가능
    100ms 이상   4채널 실시간은 빡셈 → 처리 대상을 크게 줄여야
────────────────────────────────────────────────────────────────────
"""

import argparse
import os
import sys
import time

import numpy as np

# tflite_runtime(가벼움)을 먼저 시도하고, 없으면 full tensorflow로 폴백.
try:
    from tflite_runtime.interpreter import Interpreter
    _BACKEND = "tflite_runtime"
except ImportError:
    try:
        from tensorflow.lite import Interpreter
        _BACKEND = "tensorflow.lite"
    except ImportError:
        sys.exit("tflite_runtime 도 tensorflow 도 없음.  pip install tflite-runtime")


def main():
    ap = argparse.ArgumentParser(description="MoveNet 추론 속도 벤치 (순수 속도)")
    ap.add_argument("--model", required=True, help="MoveNet .tflite 파일 경로")
    ap.add_argument("--runs", type=int, default=100, help="측정 반복 횟수 (기본 100)")
    ap.add_argument("--warmup", type=int, default=10, help="워밍업 횟수 (기본 10)")
    ap.add_argument("--threads", type=int, default=4,
                    help="추론 스레드 수 (RPi 4는 4코어 → 기본 4)")
    args = ap.parse_args()

    if not os.path.isfile(args.model):
        sys.exit(f"모델 파일 없음: {args.model}\n"
                 "  Kaggle 'google/movenet' 에서 SinglePose Lightning int8 .tflite 를 받아 경로 지정.")

    # 인터프리터 로드 + 텐서 할당
    interpreter = Interpreter(model_path=args.model, num_threads=args.threads)
    interpreter.allocate_tensors()

    inp = interpreter.get_input_details()[0]
    out = interpreter.get_output_details()[0]
    shape = inp["shape"]          # 보통 [1, H, W, 3]
    dtype = inp["dtype"]          # int8 모델은 uint8, float 모델은 float32

    # 모델이 기대하는 dtype 그대로 더미 입력 생성 (실제 추론 부하와 동일하게)
    if np.issubdtype(dtype, np.integer):
        dummy = np.random.randint(0, 256, size=shape, dtype=dtype)
    else:
        dummy = np.random.rand(*shape).astype(dtype)

    print("─" * 56)
    print(f"백엔드      : {_BACKEND}")
    print(f"모델        : {os.path.basename(args.model)}")
    print(f"입력        : shape={list(shape)}  dtype={np.dtype(dtype).name}")
    print(f"스레드      : {args.threads}   |  워밍업 {args.warmup}  측정 {args.runs}회")
    print("─" * 56)

    # 워밍업 (첫 추론은 초기화 때문에 느려서 측정에서 제외)
    for _ in range(args.warmup):
        interpreter.set_tensor(inp["index"], dummy)
        interpreter.invoke()

    # 본 측정 — 매 회 set_tensor → invoke → get_tensor (실사용과 동일한 한 사이클)
    times_ms = []
    for _ in range(args.runs):
        t0 = time.perf_counter()
        interpreter.set_tensor(inp["index"], dummy)
        interpreter.invoke()
        _ = interpreter.get_tensor(out["index"])
        times_ms.append((time.perf_counter() - t0) * 1000.0)

    t = np.array(times_ms)
    avg = t.mean()
    print(f"최소        : {t.min():6.1f} ms")
    print(f"중앙값      : {np.median(t):6.1f} ms")
    print(f"평균        : {avg:6.1f} ms")
    print(f"최대        : {t.max():6.1f} ms")
    print(f"처리량      : {1000.0 / avg:6.1f} 추론/초  (사람 1명 기준)")
    print("─" * 56)

    # 해석 가이드 + 4채널 대략 추정
    if avg <= 30:
        verdict = "여유 있음 → C++ 통합도 편하게 가능"
    elif avg <= 80:
        verdict = "가능하나 저프레임·라운드로빈 필요"
    else:
        verdict = "빡셈 → 처리 대상(사람 수·fps)을 크게 줄여야"
    print(f"판정        : {verdict}")
    print(f"참고        : 초당 {1000.0/avg:.0f}회를 4채널·여러 명이 나눠 씀 → "
          f"채널당·사람당은 이보다 훨씬 적음")
    print("─" * 56)


if __name__ == "__main__":
    main()
