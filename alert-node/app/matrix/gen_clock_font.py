#!/usr/bin/env python3
# gen_clock_font.py - 고급진 디지털시계용 7세그먼트 숫자 폰트를 clock-font.h 로 뽑는다
#
# 세그먼트 하나는 항상 단색으로 딱 떨어지게 그린다(내부에 번지는 그라데이션 없음) -
# 대신 세그먼트마다(그 세그먼트의 세로 위치에 따라) 다른 색을 골라 칠해서, 숫자
# 전체로 봤을 때만 아래(연함)->위(진함)로 그라데이션이 보이게 한다
#   - 안 켜진 세그먼트는 아예 안 그린다(완전히 꺼짐, 고스트 없음)
#   - 다운스케일 후 알파를 다시 이진화해 가장자리가 부옇게 번지지 않고 딱 떨어진다
#
# 8x 슈퍼샘플로 그린 뒤 24x24 류로 축소 (LANCZOS) 해서 대각선/모서리만 매끈하게
#
# 실행: python3 gen_clock_font.py   (Pillow 필요)
import os

from PIL import Image, ImageDraw

W, H = 8, 20              # 숫자 한 칸 크기(최종 px) - HH:MM:SS 8글자를 64px 안에 넣으려 축소
COLON_W = 3                # 콜론 칸 너비(높이는 숫자와 동일)
SCALE = 8
MARGIN = 0.8
THICK = 1.8                # 세그먼트 굵기(최종 px 기준)
ALPHA_THRESH = 128          # 다운스케일 후 이 값 기준으로 다시 이진화 (가장자리 안 번지게)

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "clock-font.h")

# 진한 초록 계열 - 눈에 편안하면서, 낙상 경보(alert.gif 의 노랑/금색)와는 뚜렷이 구분돼
# 시계 -> 경보로 화면이 바뀌는 순간 색 자체가 확 달라져 눈에 띈다
# 아래(밝은 초록)일수록 밝고, 위(짙은 숲색)일수록 진하다 - 카메라로 찍었을 때도
# 구간이 뭉개지지 않게 대비 폭을 크게 벌림
LIGHT = (170, 255, 190)   # 아래쪽 세그먼트
DEEP  = (0, 35, 12)       # 위쪽 세그먼트
# 안 켜진 세그먼트는 아예 안 그린다(완전히 꺼짐) - 고스트 톤 없음

DIGIT_SEGMENTS = {
    "0": set("abcdef"),
    "1": set("bc"),
    "2": set("abged"),
    "3": set("abgcd"),
    "4": set("fgbc"),
    "5": set("afgcd"),
    "6": set("afgecd"),
    "7": set("abc"),
    "8": set("abcdefg"),
    "9": set("abcdfg"),
}


def s(pt):
    return (pt[0] * SCALE, pt[1] * SCALE)


def hbar(d, y_center, color):
    half = THICK / 2
    x0, y0 = s((MARGIN + THICK * 0.3, y_center - half))
    x1, y1 = s((W - MARGIN - THICK * 0.3, y_center + half))
    d.rectangle([x0, y0, x1, y1], fill=color)


def vbar(d, x_center, y0f, y1f, color):
    half = THICK / 2
    x0, y0 = s((x_center - half, y0f))
    x1, y1 = s((x_center + half, y1f))
    d.rectangle([x0, y0, x1, y1], fill=color)


# 세그먼트별 (그리는 함수용 좌표, 세로 중심) - 중심 y 로 그 세그먼트의 그라데이션 색을 고른다
def segment_geom(seg):
    y_top, y_mid, y_bot = MARGIN, H / 2, H - MARGIN
    x_l, x_r = MARGIN, W - MARGIN
    if seg == "a": return ("h", y_top + THICK / 2)
    if seg == "d": return ("h", y_bot - THICK / 2)
    if seg == "g": return ("h", y_mid)
    if seg == "f": y0, y1 = y_top + THICK, y_mid - THICK * 0.3; return ("v", x_l + THICK / 2, y0, y1)
    if seg == "b": y0, y1 = y_top + THICK, y_mid - THICK * 0.3; return ("v", x_r - THICK / 2, y0, y1)
    if seg == "e": y0, y1 = y_mid + THICK * 0.3, y_bot - THICK; return ("v", x_l + THICK / 2, y0, y1)
    if seg == "c": y0, y1 = y_mid + THICK * 0.3, y_bot - THICK; return ("v", x_r - THICK / 2, y0, y1)
    raise ValueError(seg)


def draw_segment(d, seg, color):
    g = segment_geom(seg)
    if g[0] == "h":
        hbar(d, g[1], color)
    else:
        _, xc, y0, y1 = g
        vbar(d, xc, y0, y1, color)


def segment_center_y(seg):
    g = segment_geom(seg)
    return g[1] if g[0] == "h" else (g[2] + g[3]) / 2


def sample(t):
    """t=0(맨 위) -> DEEP, t=1(맨 아래) -> LIGHT"""
    return tuple(round(DEEP[i] + (LIGHT[i] - DEEP[i]) * t) for i in range(3))


def binarize(img):
    px = img.load()
    w, h = img.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            px[x, y] = (r, g, b, 255) if a >= ALPHA_THRESH else (0, 0, 0, 0)
    return img


def build_digit(ch):
    on = DIGIT_SEGMENTS[ch]
    big = Image.new("RGBA", (W * SCALE, H * SCALE), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    for seg in on:   # 안 켜진 세그먼트는 그리지도 않는다 - 완전히 꺼짐
        t = segment_center_y(seg) / H
        color = sample(t)
        draw_segment(d, seg, (*color, 255))
    return binarize(big.resize((W, H), Image.LANCZOS))


def build_colon(on):
    big = Image.new("RGBA", (COLON_W * SCALE, H * SCALE), (0, 0, 0, 0))
    if on:
        d = ImageDraw.Draw(big)
        cx = COLON_W / 2 * SCALE
        r = 0.9 * SCALE
        for cy_f in (H * 0.34, H * 0.66):
            color = sample(cy_f / H)
            cy = cy_f * SCALE
            d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(*color, 255))
    return binarize(big.resize((COLON_W, H), Image.LANCZOS))


digits = [build_digit(str(n)) for n in range(10)]
colons = [build_colon(False), build_colon(True)]   # [0]=꺼짐(고스트) [1]=켜짐


def emit_glyph(f, img, w, h, indent="\t\t"):
    pixels = list(img.getdata())
    for row in range(h):
        vals = []
        for col in range(w):
            r, g, b, a = pixels[row * w + col]
            vals.append("{%d,%d,%d,1}" % (r, g, b) if a else "{0,0,0,0}")
        f.write(indent + "{ " + ", ".join(vals) + " },\n")


with open(OUT, "w", encoding="utf-8") as f:
    f.write("/* 자동 생성 파일 - gen_clock_font.py 가 만든다, 직접 고치지 말 것 */\n")
    f.write("/* 7세그먼트 숫자 0-9 + 콜론(꺼짐/켜짐) - 세그먼트별 단색, 숫자 전체로 봤을 때만\n")
    f.write("   아래(밝은 초록)->위(짙은 숲색) 그라데이션, 안 켜진 세그먼트는 완전히 꺼짐 */\n")
    f.write("#ifndef HUB75_CLOCK_FONT_H\n#define HUB75_CLOCK_FONT_H\n#include <stdint.h>\n\n")
    f.write("#define CLOCK_DIGIT_W %d\n" % W)
    f.write("#define CLOCK_DIGIT_H %d\n" % H)
    f.write("#define CLOCK_COLON_W %d\n\n" % COLON_W)
    f.write("typedef struct {\n\tuint8_t r, g, b;\n\tuint8_t a;  /* 0=투명 1=그림 */\n} clockpix_t;\n\n")

    f.write("static const clockpix_t clock_digit[10][CLOCK_DIGIT_H][CLOCK_DIGIT_W] = {\n")
    for img in digits:
        f.write("\t{\n")
        emit_glyph(f, img, W, H)
        f.write("\t},\n")
    f.write("};\n\n")

    f.write("/* [0] = 꺼짐(완전 투명), [1] = 켜짐(밝음) - 초 단위 점멸용 */\n")
    f.write("static const clockpix_t clock_colon[2][CLOCK_DIGIT_H][CLOCK_COLON_W] = {\n")
    for img in colons:
        f.write("\t{\n")
        emit_glyph(f, img, COLON_W, H)
        f.write("\t},\n")
    f.write("};\n\n")

    f.write("#endif /* HUB75_CLOCK_FONT_H */\n")

print("생성 완료:", OUT)
