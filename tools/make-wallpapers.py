#!/usr/bin/env python3
"""Generates the built-in CAMAI wallpapers (resources/wallpapers/*.jpg).

Dark, low-contrast designs: they must never compete with the video players.
    pip install pillow numpy && python3 tools/make-wallpapers.py
"""
import math
import os
import random

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

W, H = 1920, 1080
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "resources", "wallpapers")
TEAL = (20, 184, 166)
CYAN = (34, 211, 238)
BLUE = (59, 130, 246)
VIOLET = (124, 58, 237)


def gradient(c1, c2, angle=35):
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    a = math.radians(angle)
    t = (x * math.cos(a) + y * math.sin(a))
    t = (t - t.min()) / (t.max() - t.min())
    img = np.zeros((H, W, 3), np.float32)
    for i in range(3):
        img[..., i] = c1[i] * (1 - t) + c2[i] * t
    return img


def glow(img, cx, cy, radius, color, strength):
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    d = np.sqrt((x - cx) ** 2 + (y - cy) ** 2) / radius
    g = np.exp(-d * d) * strength
    for i in range(3):
        img[..., i] += color[i] * g
    return img


def vignette(img, strength=0.45):
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    d = np.sqrt(((x - W / 2) / (W / 2)) ** 2 + ((y - H / 2) / (H / 2)) ** 2) / math.sqrt(2)
    img *= (1 - strength * d ** 1.6)[..., None]
    return img


def overlay(img, layer, blur=0):
    if blur:
        layer = layer.filter(ImageFilter.GaussianBlur(blur))
    a = np.asarray(layer).astype(np.float32)
    alpha = a[..., 3:4] / 255.0
    return img * (1 - alpha) + a[..., :3] * alpha


LOGO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "resources", "branding", "logo-dark.png")


def wordmark(img):
    """CAMAI VMS logo in the lower right corner, dimmed: visible, but quieter than any video."""
    logo = Image.open(LOGO).convert("RGBA")
    lw = 520
    logo = logo.resize((lw, round(logo.height * lw / logo.width)), Image.LANCZOS)
    a = np.asarray(logo).astype(np.float32)
    a[..., 3] *= 0.22
    logo = Image.fromarray(a.astype(np.uint8), "RGBA")
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    layer.alpha_composite(logo, (W - lw - 64, H - logo.height - 64))
    return overlay(img, layer)


def finish(img, name):
    img = wordmark(img)
    rng = np.random.default_rng(7)
    img += rng.normal(0, 1.4, img.shape)          # dithering against banding
    img = np.clip(img, 0, 255).astype(np.uint8)
    Image.fromarray(img).save(os.path.join(OUT, name + ".jpg"), quality=86, optimize=True, progressive=True)
    print("written", name)


def grid():
    img = gradient((8, 12, 22), (14, 22, 36))
    img = glow(img, W * 0.12, H * 0.1, 700, TEAL, 0.10)
    img = glow(img, W * 0.95, H * 0.95, 600, BLUE, 0.07)
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    step = 64
    for x in range(0, W, step):
        a = int(16 + 20 * (1 - abs(x - W * 0.3) / W))
        d.line((x, 0, x, H), fill=CYAN + (a,), width=1)
    for y in range(0, H, step):
        a = int(16 + 20 * (1 - abs(y - H * 0.3) / H))
        d.line((0, y, W, y), fill=CYAN + (a,), width=1)
    for x in range(0, W, step * 4):          # brighter crosses at major nodes
        for y in range(0, H, step * 4):
            d.line((x - 6, y, x + 6, y), fill=TEAL + (60,), width=1)
            d.line((x, y - 6, x, y + 6), fill=TEAL + (60,), width=1)
    img = overlay(img, layer)
    finish(vignette(img), "camai-grid")


def lens():
    img = gradient((7, 10, 18), (13, 18, 30), 120)
    cx, cy = W * 0.70, H * 0.46
    img = glow(img, cx, cy, 520, TEAL, 0.09)
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for i, r in enumerate(range(90, 900, 46)):
        a = max(6, 46 - i * 2)
        d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=CYAN + (a,), width=1 if i % 3 else 2)
    # aperture blades
    for k in range(8):
        a0 = math.radians(k * 45 + 10)
        pts = [(cx + 80 * math.cos(a0), cy + 80 * math.sin(a0)),
               (cx + 210 * math.cos(a0 + 0.6), cy + 210 * math.sin(a0 + 0.6))]
        d.line(pts, fill=TEAL + (55,), width=2)
    d.ellipse((cx - 18, cy - 18, cx + 18, cy + 18), outline=CYAN + (70,), width=2)
    img = overlay(img, layer, blur=0.6)
    finish(vignette(img, 0.5), "camai-lens")


def network():
    img = gradient((8, 10, 20), (16, 16, 32), 60)
    img = glow(img, W * 0.8, H * 0.2, 650, VIOLET, 0.07)
    img = glow(img, W * 0.2, H * 0.85, 650, TEAL, 0.07)
    rnd = random.Random(42)
    pts = [(rnd.uniform(0, W), rnd.uniform(0, H)) for _ in range(140)]
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for i, (x1, y1) in enumerate(pts):
        for x2, y2 in pts[i + 1:]:
            dist = math.hypot(x2 - x1, y2 - y1)
            if dist < 210:
                d.line((x1, y1, x2, y2), fill=CYAN + (int(52 * (1 - dist / 210)),), width=1)
    for x, y in pts:
        r = rnd.choice((2, 2, 3, 4))
        d.ellipse((x - r, y - r, x + r, y + r), fill=TEAL + (90,))
    img = overlay(img, layer)
    finish(vignette(img), "camai-network")


def hexes():
    img = gradient((9, 13, 22), (12, 20, 30), 20)
    img = glow(img, W * 0.0, H * 1.0, 900, TEAL, 0.08)
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    r = 46
    hw = math.sqrt(3) * r
    for row in range(-1, int(H / (1.5 * r)) + 2):
        for col in range(-1, int(W / hw) + 2):
            cx = col * hw + (hw / 2 if row % 2 else 0)
            cy = row * 1.5 * r
            dist = math.hypot(cx - 0, cy - H) / math.hypot(W, H)
            a = int(max(0, 62 * (1 - dist * 1.2)))
            if a < 3:
                continue
            poly = [(cx + r * math.cos(math.radians(60 * k + 30)), cy + r * math.sin(math.radians(60 * k + 30)))
                    for k in range(6)]
            d.polygon(poly, outline=CYAN + (a,))
    img = overlay(img, layer)
    finish(vignette(img, 0.4), "camai-hex")


def waves():
    img = gradient((7, 9, 18), (15, 14, 30), 90)
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for k in range(26):
        col = TEAL if k % 2 else VIOLET
        base = H * 0.55 + k * 9
        amp = 90 + k * 3
        pts = [(x, base + amp * math.sin(x / 260 + k * 0.17) + 40 * math.sin(x / 90 + k)) for x in range(0, W + 20, 20)]
        d.line(pts, fill=col + (42,), width=2)
    img = overlay(img, layer, blur=1.2)
    img = glow(img, W * 0.5, H * 0.62, 800, TEAL, 0.05)
    finish(vignette(img, 0.5), "camai-waves")


def graphite():
    img = gradient((14, 16, 20), (24, 27, 33), 30)
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for x in range(-H, W, 14):
        d.line((x, H, x + H, 0), fill=(255, 255, 255, 5), width=1)
    img = overlay(img, layer)
    img = glow(img, W * 0.85, H * 0.1, 700, TEAL, 0.04)
    finish(vignette(img, 0.35), "camai-graphite")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for fn in (grid, lens, network, hexes, waves, graphite):
        fn()
