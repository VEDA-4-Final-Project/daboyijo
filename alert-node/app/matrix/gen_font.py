#!/usr/bin/env python3
# gen_font.py - 한글 완성형 전체와 ASCII 를 16px 비트맵으로 구워 C 헤더로 뽑는다
#
# 나눔고딕Bold 를 PIL 로 렌더링해서 font16.h 를 만든다
#   - 글자 하나당 16행, 각 행은 uint16 (비트 0x8000 = 맨 왼쪽 칸)
#   - adv = 다음 글자로 커서를 옮길 칸 수 (한글은 넓고 숫자는 좁다)
#
# 완성형 11172 자를 전부 넣는다(450KB) - 입주자 이름은 DB 에서 오는 임의의 한글이라
# 골라 담으면 목록 밖 글자에서 렌더러가 조용히 8px 를 건너뛰고 지나간다
#
# 실행: python3 gen_font.py   (fonts-nanum 설치 필요, 5초쯤)

import os
import sys

from PIL import Image, ImageFont, ImageDraw

FONT_PATH = "/usr/share/fonts/truetype/nanum/NanumGothicBold.ttf"
SIZE = 15          # 16행 안에 안정적으로 들어가는 크기 (미리보기로 튜닝함)
H    = 16          # 글자 높이 (칸)
THRESH = 110       # 안티에일리어싱 알파 임계값 - 이보다 진하면 켠 픽셀
# 헤더를 쓰는 소스가 같은 디렉터리에 있어서, 어디서 실행하든 여기에 쓴다
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "font16.h")

# 한글 완성형 전체 (U+AC00 ~ U+D7A3)
CHARS_KO = "".join(chr(c) for c in range(0xAC00, 0xD7A3 + 1))
# 출력 가능한 ASCII 전부 - 서버 message 가 로그용 영문이라 알파벳이 없으면
# 폴백 표시가 숫자만 남는다 (main.cpp toText 참고)
CHARS_ASCII = "".join(chr(c) for c in range(0x20, 0x7E + 1))

font = ImageFont.truetype(FONT_PATH, SIZE)
asc, desc = font.getmetrics()
y0 = (H - (asc + desc)) // 2      # 글자를 세로 중앙에 놓는 시작 y

glyphs = []   # (codepoint, adv, [16개의 uint16 행])
seen = set()
clipped = []  # 16칸을 넘어가 잘린 글자 - 있으면 SIZE 를 줄여야 한다

# 셀보다 넓게 그린다 - 16칸만 그리면 넘친 픽셀이 조용히 버려져 잘림을 못 본다
CANVAS = H * 2

for ch in CHARS_KO + CHARS_ASCII:
    if ch in seen:
        continue
    seen.add(ch)
    cp = ord(ch)

    bbox = font.getbbox(ch)                 # (l, t, r, b)
    adv = round(font.getlength(ch))
    if ch == " ":
        adv = 5                             # 공백은 좁게

    img = Image.new("L", (CANVAS, H), 0)
    d = ImageDraw.Draw(img)
    d.text((-bbox[0], y0), ch, fill=255, font=font)   # 왼쪽 정렬(bbox 좌측 여백 제거)

    # 11000 자를 px[c, r] 로 하나씩 집으면 파이에서 몇 분 걸린다 - 행 단위로 뜬다
    raw = img.tobytes()

    rows = []
    overflow = False
    for r in range(H):
        line = raw[r * CANVAS:(r + 1) * CANVAS]
        bits = 0
        for c in range(H):
            if line[c] > THRESH:
                bits |= (0x8000 >> c)
        if any(v > THRESH for v in line[H:]):
            overflow = True
        rows.append(bits)

    if overflow:
        clipped.append(ch)
    glyphs.append((cp, adv, rows))

glyphs.sort(key=lambda g: g[0])             # 코드포인트 오름차순 (C 에서 이진탐색 가능)

with open(OUT, "w", encoding="utf-8") as f:
    f.write("/* 자동 생성 파일 - gen_font.py 가 만든다, 직접 고치지 말 것 */\n")
    f.write("/* 나눔고딕Bold %dpx, 셀 %d행, 비트 0x8000=맨 왼쪽 */\n" % (SIZE, H))
    f.write("#ifndef HUB75_FONT16_H\n#define HUB75_FONT16_H\n#include <stdint.h>\n\n")
    f.write("#define FONT16_H %d\n\n" % H)
    f.write("typedef struct {\n")
    f.write("\tuint32_t cp;        /* 유니코드 코드포인트 */\n")
    f.write("\tuint8_t  adv;       /* 다음 글자까지 칸 수 */\n")
    f.write("\tuint16_t rows[%d];  /* 위->아래, 비트 0x8000=왼쪽 */\n" % H)
    f.write("} glyph16_t;\n\n")
    f.write("static const glyph16_t font16[] = {\n")
    for cp, adv, rows in glyphs:
        try:
            name = chr(cp)
        except ValueError:
            name = "?"
        rowstr = ", ".join("0x%04X" % b for b in rows)
        f.write("\t{ 0x%04X, %2d, { %s } }, /* %s */\n" % (cp, adv, rowstr, name))
    f.write("};\n\n")
    f.write("#define FONT16_COUNT (sizeof(font16) / sizeof(font16[0]))\n\n")
    f.write("#endif /* HUB75_FONT16_H */\n")

print("생성 완료: %s (글자 %d개)" % (OUT, len(glyphs)))
if clipped:
    print("경고: 16칸을 넘어 잘린 글자 %d개 - SIZE 를 줄여야 한다" % len(clipped),
          file=sys.stderr)
    print("      " + "".join(clipped[:60]), file=sys.stderr)
