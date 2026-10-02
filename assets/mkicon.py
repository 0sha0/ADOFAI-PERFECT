# -*- coding: utf-8 -*-
import math, os
from PIL import Image, ImageDraw

S = 1024
img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
BLACK = (17, 17, 17, 255)
WHITE = (255, 255, 255, 255)
W = 20

m = 46
d.rounded_rectangle([m, m, S - m, S - m], radius=190, fill=WHITE, outline=BLACK, width=W)
cx = cy = S // 2

def polar(a_deg, r):
    a = math.radians(a_deg)
    return (cx + r * math.cos(a), cy + r * math.sin(a))

# ---- 中央：Malody 风格描边方块（实心黑）+ 白色 M ----
box = 172
d.rounded_rectangle([cx - box, cy - box, cx + box, cy + box], radius=58, fill=BLACK, outline=BLACK, width=26)
mw, mh = 98, 118
top, bot = cy - mh // 2, cy + mh // 2 - 30
mid = cy + 29
t = 40  # 笔画粗细
mpts = [(cx - mw, bot), (cx - mw, top), (cx, mid), (cx + mw, top), (cx + mw, bot)]
d.line(mpts, fill=WHITE, width=t, joint='curve')
for p in [mpts[0], mpts[1], mpts[3], mpts[4]]:
    d.ellipse([p[0] - t // 2, p[1] - t // 2, p[0] + t // 2, p[1] + t // 2], fill=WHITE)

# ---- 虚线圆环 ----
ring = 328
seg, gap = 12, 6
for i in range(seg + gap):
    a0 = 360.0 * i / (seg + gap)
    a1 = a0 + (360.0 / (seg + gap)) * 1.55
    d.arc([cx - ring, cy - ring, cx + ring, cy + ring], a0, a1, fill=BLACK, width=14)

# ---- 冰晶（左） ----
def crystal(a_deg, r=376, s=1.0):
    px, py = polar(a_deg, r)
    h, w = 66 * s, 38 * s
    d.polygon([(px, py - h), (px + w, py), (px, py + h), (px - w, py)], fill=WHITE, outline=BLACK)
    d.line([(px, py - h), (px + w, py), (px, py + h), (px - w, py), (px, py - h)], fill=BLACK, width=17, joint='curve')
    d.line([(px, py - h * 0.74), (px, py + h * 0.74)], fill=BLACK, width=13)
    d.line([(px - w * 0.70, py - h * 0.34), (px + w * 0.70, py + h * 0.34)], fill=BLACK, width=11)
    d.line([(px + w * 0.70, py - h * 0.34), (px - w * 0.70, py + h * 0.34)], fill=BLACK, width=11)

for a, s in ((190, 1.0), (236, 0.84), (144, 0.84)):
    crystal(a, 376, s)

# ---- 火苗（右） ----
def flame(a_deg, r=376, s=1.0):
    px, py = polar(a_deg, r)
    h, w = 72 * s, 42 * s
    pts = [(px, py - h),
           (px + w * 0.92, py - h * 0.10),
           (px + w * 0.56, py + h * 0.80),
           (px, py + h),
           (px - w * 0.56, py + h * 0.80),
           (px - w * 0.92, py - h * 0.10)]
    d.polygon(pts, fill=WHITE, outline=BLACK)
    d.line(pts + [pts[0]], fill=BLACK, width=17, joint='curve')
    d.arc([px - w * 0.50, py - h * 0.34, px + w * 0.50, py + h * 0.66], 205, 335, fill=BLACK, width=13)

for a, s in ((350, 1.0), (304, 0.84), (36, 0.84)):
    flame(a, 376, s)

os.makedirs('assets', exist_ok=True)
img.save('assets/adoFaiPerfect-icon.png')
img.save('assets/adoFaiPerfect.ico', sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (16, 16)])
bg = Image.new('RGBA', (S, S), (32, 34, 40, 255)); bg.alpha_composite(img)
bg.convert('RGB').resize((512, 512), Image.LANCZOS).save('.reverse/icon_preview.png')
sm = Image.new('RGBA', (300, 300), (32, 34, 40, 255)); sm.alpha_composite(img.resize((300, 300), Image.LANCZOS))
sm.convert('RGB').save('.reverse/icon_small.png')
print('ok')
