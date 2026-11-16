#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""为喵喵同学机芯盒生成 11 个新表情 GIF（240x240，风格对齐原厂美术）。
用法: /usr/bin/python3 make_emojis.py <素材目录> <输出目录>
素材: shocked_5.png(睁眼底图) neutral_5.png(闭眼脸/弧线)"""

import math, os, random, sys
from PIL import Image, ImageDraw, ImageFilter

FACE = (250, 205, 138)
ARC = (66, 38, 16)
LID = (238, 233, 248)
LID_EDGE = (178, 100, 222)
PUPIL = (32, 23, 32)
HEART = (214, 40, 70)
TEAR = (110, 190, 245)
BLUSH = (250, 178, 186)
SIZE = 240

random.seed(42)
IRIS_C = (120, 145)
ARC_PTS = {}

def load_sources(src):
    ball = Image.open(os.path.join(src, 'shocked_5.png')).convert('RGB')
    face = Image.open(os.path.join(src, 'face_closed.png')).convert('RGB')
    return ball, face

def extract_arc(face):
    """从闭眼脸帧提取弧线骨架（每列弧线色像素的中线 y）。
    过滤：弧线为深棕色(40<=r<140)，排除近黑描边与米色底。"""
    pts = {}
    px = face.load()
    for x in range(SIZE):
        ys = [y for y in range(40, 190)
              if 40 <= px[x, y][0] < 140 and px[x, y][1] < 100 and px[x, y][2] < 90]
        if ys and len(ys) < 15:
            pts[x] = sum(ys) / len(ys)
    return pts

def circ_mask(r):
    m = Image.new('L', (2*r, 2*r), 0)
    ImageDraw.Draw(m).ellipse([0, 0, 2*r, 2*r], fill=255)
    return m

def make_clean_ball(ball):
    """把底图的瞳孔/高光修补掉，得到纯净星云虹膜眼球"""
    global IRIS_C
    px = ball.load()
    # 瞳孔 = 近黑色像素团的质心
    xs, ys, n = 0, 0, 0
    for y in range(126, 176):
        for x in range(85, 160):
            r, g, b = px[x, y]
            if r < 70 and g < 70 and b < 70:
                xs += x; ys += y; n += 1
    if n > 50:
        bc = (xs // n, ys // n)
    else:
        bc = (120, 145)
    IRIS_C = bc
    tex = ball.rotate(180)
    out = ball.copy()
    m1 = Image.new('L', (SIZE, SIZE), 0)
    ImageDraw.Draw(m1).ellipse([bc[0]-27, bc[1]-27, bc[0]+27, bc[1]+27], fill=255)
    out.paste(tex, (0, 0), m1)
    # 右上粉色高光斑也补掉
    tex2 = tex.rotate(53)
    m2 = Image.new('L', (SIZE, SIZE), 0)
    ImageDraw.Draw(m2).ellipse([bc[0]+12, bc[1]-40, bc[0]+42, bc[1]-10], fill=255)
    out.paste(tex2, (0, 0), m2)
    return out

def ball_mask_soft():
    m = Image.new('L', (SIZE, SIZE), 0)
    ImageDraw.Draw(m).ellipse([2, 2, SIZE-2, SIZE-2], fill=255)
    return m.filter(ImageFilter.GaussianBlur(1.0))

_BALL_MASK = ball_mask_soft()

def eye_open(base_clean, pupil=(0, 0), pr=13, lid=0.0, heart=False, dilate=0):
    """睁眼。pupil=瞳孔相对虹膜中心偏移, pr=半径, lid=眼皮覆盖0-1"""
    im = base_clean.copy()
    d = ImageDraw.Draw(im)
    ic = IRIS_C
    if dilate:
        r = 46
        region = im.crop((ic[0]-r, ic[1]-r, ic[0]+r, ic[1]+r))
        region = region.resize((r+dilate, r+dilate), Image.LANCZOS)
        region = region.crop((dilate//2, dilate//2, dilate//2+r, dilate//2+r))
        im.paste(region, (ic[0]-r, ic[1]-r))
        d = ImageDraw.Draw(im)
    pc = (ic[0]+pupil[0], ic[1]+pupil[1])
    if heart:
        s = pr * 1.5
        d.polygon(heart_poly(pc, s), fill=HEART)
        d.ellipse([pc[0]+s*0.12, pc[1]-s*0.42, pc[0]+s*0.40, pc[1]-s*0.14],
                  fill=(255, 120, 140))
    else:
        d.ellipse([pc[0]-pr, pc[1]-pr, pc[0]+pr, pc[1]+pr], fill=PUPIL)
        gl = max(3, int(pr*0.38))
        d.ellipse([pc[0]-pr*0.45, pc[1]-pr*0.5,
                   pc[0]-pr*0.45+gl, pc[1]-pr*0.5+gl], fill=(255, 255, 255))
    if lid > 0.02:
        chord = int(SIZE*0.10 + lid*(SIZE*0.62))
        m = Image.new('L', (SIZE, SIZE), 0)
        ImageDraw.Draw(m).ellipse([-60, chord-900, SIZE+60, chord], fill=255)
        m = Image.composite(m, Image.new('L', (SIZE, SIZE), 0), _BALL_MASK)
        im.paste(LID, (0, 0), m)
        d = ImageDraw.Draw(im)
        d.arc([-60, chord-900, SIZE+60, chord], start=178, end=362,
              fill=LID_EDGE, width=4)
    return im

def heart_poly(c, s):
    pts = []
    for i in range(24):
        t = math.pi * 2 * i / 24
        x = 16*math.sin(t)**3
        y = -(13*math.cos(t) - 5*math.cos(2*t) - 2*math.cos(3*t) - math.cos(4*t))
        pts.append((c[0]+x*s/17, c[1]+y*s/17))
    return pts

def closed_face(face_base, depth=1.0, dy=0):
    """闭眼米色脸 + 弧线。depth: 弧线弯曲倍率"""
    im = face_base.copy()
    d = ImageDraw.Draw(im)
    ys = [ARC_PTS[x] for x in range(6, SIZE-6, 4) if x in ARC_PTS]
    yc = sum(ys)/len(ys)
    path = []
    for x in range(4, SIZE-3, 2):
        if x in ARC_PTS:
            path.append((x, yc + (ARC_PTS[x]-yc)*depth + dy))
    d.line(path, fill=ARC, width=11, joint='curve')
    return im

def save_gif(frames, out, duration):
    frames[0].save(out, save_all=True, append_images=frames[1:],
                   duration=duration, loop=0, optimize=True)
    return os.path.getsize(out)

def star(d, x, y, s, col):
    pts = []
    for i in range(8):
        r = s if i % 2 == 0 else s*0.4
        a = math.pi*2*i/8 - math.pi/2
        pts.append((x+r*math.cos(a), y+r*math.sin(a)))
    d.polygon(pts, fill=col)

def draw_question(d, x, y):
    qc = (60, 45, 75)
    d.arc([x-20, y-34, x+20, y+6], start=145, end=390, fill=qc, width=8)
    d.line([(x+20, y+4), (x+4, y+18)], fill=qc, width=8)
    d.ellipse([x-3, y+28, x+9, y+40], fill=qc)

def gen_all(src, outdir):
    os.makedirs(outdir, exist_ok=True)
    ball, face = load_sources(src)
    clean = make_clean_ball(ball)
    global ARC_PTS
    ARC_PTS = extract_arc(face)
    print("虹膜中心:", IRIS_C, " 弧线点数:", len(ARC_PTS))

    made = {}
    F = lambda n: [i/(n-1) for i in range(n)]

    # 1. laughing 大笑：闭眼深弧 + 弹跳 + 笑泪
    fr = []
    for i, t in enumerate(F(16)):
        dy = int(6*math.sin(math.pi*2*t*2))
        im = closed_face(face, depth=1.3, dy=dy)
        d = ImageDraw.Draw(im)
        side = -1 if (i//4) % 2 == 0 else 1
        x, y = 120 + side*58, 74 + dy
        d.ellipse([x-11, y-16, x+11, y+16], fill=TEAR)
        fr.append(im)
    made['laughing'] = save_gif(fr, f'{outdir}/emoji_laughing.gif', 95)

    # 2. kissy 飞吻：闭眼弧 + 上升爱心
    fr = []
    for i, t in enumerate(F(14)):
        im = closed_face(face, depth=1.0, dy=int(4*math.sin(math.pi*2*t)))
        d = ImageDraw.Draw(im)
        y = 210 - 150*t
        s = 5 + 9*math.sin(math.pi*t)
        alpha = 1.0 if t < 0.85 else max(0.0, (1-t)/0.15)
        col = tuple(int(c*alpha + 253*(1-alpha)) for c in HEART)
        d.polygon([(120+dx*s/17, y+dy2*s/17) for dx, dy2 in heart_poly((0, 0), 17)],
                  fill=col)
        fr.append(im)
    made['kissy'] = save_gif(fr, f'{outdir}/emoji_kissy.gif', 110)

    # 3. loving 爱心瞳
    fr = []
    for i, t in enumerate(F(14)):
        s = 1 + 0.13*math.sin(math.pi*2*t)
        fr.append(eye_open(clean, heart=True, pr=13*s))
    made['loving'] = save_gif(fr, f'{outdir}/emoji_loving.gif', 120)

    # 4. funny 调皮乱瞟
    fr = []
    xs = [-16, 16, -12, 14, -16, 16, 0, 8]
    for i in range(12):
        fr.append(eye_open(clean, pupil=(xs[i % len(xs)], 2), pr=12))
    made['funny'] = save_gif(fr, f'{outdir}/emoji_funny.gif', 70)

    # 5. confident 自信半眯
    fr = []
    for i, t in enumerate(F(12)):
        lid = 0.35 + 0.04*math.sin(math.pi*2*t)
        fr.append(eye_open(clean, pupil=(0, -6), pr=13, lid=lid))
    made['confident'] = save_gif(fr, f'{outdir}/emoji_confident.gif', 110)

    # 6. silly 呆萌
    fr = []
    pos = [(0, -12), (10, 4), (-10, 4), (0, -8)]
    for i in range(12):
        fr.append(eye_open(clean, pupil=pos[i % len(pos)], pr=8, lid=0.12))
    made['silly'] = save_gif(fr, f'{outdir}/emoji_silly.gif', 100)

    # 7. embarrassed 羞涩垂眼
    fr = []
    for i, t in enumerate(F(14)):
        jx = 2*math.sin(math.pi*6*t)
        im = eye_open(clean, pupil=(-7+jx, 9), pr=9, lid=0.62)
        ImageDraw.Draw(im).ellipse([44, 158, 92, 196], fill=BLUSH)
        fr.append(im)
    made['embarrassed'] = save_gif(fr, f'{outdir}/emoji_embarrassed.gif', 120)

    # 8. surprised 惊讶瞳孔放大
    fr = []
    for i, t in enumerate(F(14)):
        pr = 20 + 3*math.sin(math.pi*2*t*2)
        fr.append(eye_open(clean, dilate=6, pr=pr))
    made['surprised'] = save_gif(fr, f'{outdir}/emoji_surprised.gif', 110)

    # 9. confused 疑问
    fr = []
    for i, t in enumerate(F(14)):
        im = eye_open(clean, pupil=(-9, -10), pr=10, lid=0.10)
        draw_question(ImageDraw.Draw(im), 170, 142)
        fr.append(im)
    made['confused'] = save_gif(fr, f'{outdir}/emoji_confused.gif', 120)

    # 10. winking 眨眼
    fr = []
    seq = [0.0]*6 + [0.45, 1.0, 1.0, 1.0, 0.45] + [0.0]*6
    for lid in seq:
        if lid >= 0.98:
            fr.append(closed_face(face, depth=0.9))
        else:
            fr.append(eye_open(clean, pr=13, lid=lid))
    made['winking'] = save_gif(fr, f'{outdir}/emoji_winking.gif', 100)

    # 11. delicious 美味星星
    fr = []
    for i, t in enumerate(F(14)):
        im = closed_face(face, depth=0.75, dy=int(4*math.sin(math.pi*2*t*2)))
        d = ImageDraw.Draw(im)
        for k, (sx, sy) in enumerate([(78, 66), (150, 92), (108, 44)]):
            ph = (t*2 + k*0.33) % 1.0
            ss = 12*(0.4 + 0.6*math.sin(math.pi*ph))
            star(d, sx, sy, ss, (255, 236, 120) if ph < 0.5 else (255, 210, 90))
        fr.append(im)
    made['delicious'] = save_gif(fr, f'{outdir}/emoji_delicious.gif', 110)

    for k, v in made.items():
        print(f"  {k:<12} {v//1024} KB")

if __name__ == '__main__':
    gen_all(sys.argv[1], sys.argv[2])
