#!/usr/bin/env python3
# gen_alert_gif.py - alert.gif(노란 경고 삼각형, 43프레임)를 alert-gif.h 로 뽑는다
#
# 원본이 472x480 검정 배경 위 삼각형이라(투명 없음, 거의 정사각형) 알파 없이
# RGB 그대로 24x24 로 축소해서 굽는다 - showFallAlert() 의 아이콘 자리를 대체
#
# 실행: python3 gen_alert_gif.py   (Pillow 필요)
import os

from PIL import Image

SIZE = 20   # 한글 폰트(16px)보다 살짝 큰 정도로
SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "alert.gif")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "alert-gif.h")

im = Image.open(SRC)
n_frames = im.n_frames

frames = []
for i in range(n_frames):
    im.seek(i)
    frame = im.convert("RGB")
    w, h = frame.size
    scale = min(SIZE / w, SIZE / h)
    rw, rh = max(1, round(w * scale)), max(1, round(h * scale))
    resized = frame.resize((rw, rh), Image.LANCZOS)
    canvas = Image.new("RGB", (SIZE, SIZE), (0, 0, 0))
    canvas.paste(resized, ((SIZE - rw) // 2, (SIZE - rh) // 2))
    frames.append(canvas)

# GIF 프레임 지속시간(ms) - 전부 같으면 하나로, 다르면 첫 프레임 값 사용(원본이 균일함)
frame_ms = im.info.get("duration", 70)

with open(OUT, "w", encoding="utf-8") as f:
    f.write("/* 자동 생성 파일 - gen_alert_gif.py 가 만든다, 직접 고치지 말 것 */\n")
    f.write("/* alert.gif(노란 경고 삼각형) %d프레임, %dx%d 로 축소, 알파 없음(원본이 불투명) */\n"
            % (n_frames, SIZE, SIZE))
    f.write("#ifndef HUB75_ALERT_GIF_H\n#define HUB75_ALERT_GIF_H\n#include <stdint.h>\n\n")
    f.write("#define ALERT_GIF_W %d\n" % SIZE)
    f.write("#define ALERT_GIF_H %d\n" % SIZE)
    f.write("#define ALERT_GIF_FRAMES %d\n" % n_frames)
    f.write("#define ALERT_GIF_FRAME_MS %d\n\n" % frame_ms)
    f.write("typedef struct {\n\tuint8_t r, g, b;\n} alertgifpix_t;\n\n")
    f.write("static const alertgifpix_t alert_gif[ALERT_GIF_FRAMES][ALERT_GIF_H][ALERT_GIF_W] = {\n")
    for img in frames:
        pixels = list(img.getdata())
        f.write("\t{\n")
        for row in range(SIZE):
            vals = []
            for col in range(SIZE):
                r, g, b = pixels[row * SIZE + col]
                vals.append("{%d,%d,%d}" % (r, g, b))
            f.write("\t\t{ " + ", ".join(vals) + " },\n")
        f.write("\t},\n")
    f.write("};\n\n")
    f.write("#endif /* HUB75_ALERT_GIF_H */\n")

print("생성 완료: %s (%d프레임, %dms/프레임)" % (OUT, n_frames, frame_ms))
