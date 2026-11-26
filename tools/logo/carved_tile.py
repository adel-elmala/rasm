"""Render the RASM logo: a carved star-and-cross tile, lit as a real surface, beside the RASM wordmark.

Writes docs/assets/logo/: icon-512.png, icon-180.png, icon-32.png, logo-dark.png and logo-light.png.
Needs Python 3 with numpy and Pillow:  python tools/logo/carved_tile.py
"""
import math
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "docs", "assets", "logo")
FONT = os.path.join(HERE, "fonts", "CormorantGaramond-SemiBold-RASM.ttf")  # Cormorant Garamond 600, only R A S M (OFL)
WORD_COLOR = {"dark": (230, 237, 243), "light": (31, 35, 40)}               # GitHub's text colors on #0d1117 and #ffffff


def noise(size, cells, seed):
    g = (np.random.default_rng(seed).random((cells, cells)) * 255).astype(np.uint8)
    return np.asarray(Image.fromarray(g, "L").resize((size, size), Image.BICUBIC), dtype=np.float32) / 255 - 0.5


def rounded(im, radius):
    w, h = im.size
    ss = 4
    mask = Image.new("L", (w * ss, h * ss), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, w * ss - 1, h * ss - 1], radius * ss, fill=255)
    im.putalpha(mask.resize((w, h), Image.LANCZOS))
    return im


def carved_tile(size=512):
    # An eight-pointed star tiled with its crosses. The star is a height field of 16 flat facets, the crosses a glazed
    # ground; both are lit per pixel by one light from the top left, with a specular term and a blurred-height occlusion.
    ss = 2
    n = size * ss
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float32) / ss
    period = 296.0
    outer, inner = period / 2, period / 2 * 0.7654
    lx = (xx - 256 + period / 2) % period - period / 2
    ly = (yy - 256 + period / 2) % period - period / 2
    rho = np.sqrt(lx * lx + ly * ly)
    phi = np.degrees(np.arctan2(ly, lx))
    beta = np.radians(np.abs((phi + 22.5) % 45.0 - 22.5))
    px, py = rho * np.cos(beta), rho * np.sin(beta)
    ix, iy = inner * math.cos(math.radians(22.5)), inner * math.sin(math.radians(22.5))
    v = py / iy
    u = (px - v * ix) / outer
    star = (u + v) <= 1.0
    h_center, h_tip, h_inner = 64.0, 30.0, 10.0
    h = np.where(star, h_center * (1 - u - v) + h_tip * u + h_inner * v, 0.0)
    h = h + np.where(star, 0.0, noise(n, 9, 3) * 9.0) + noise(n, 160, 8) * 0.5

    gy, gx = np.gradient(h, 1.0 / ss)
    length = np.sqrt(gx * gx + gy * gy + 1)
    nx, ny, nz = -gx / length, -gy / length, 1 / length
    light = np.array([-0.52, -0.62, 0.59])
    light /= np.linalg.norm(light)
    diffuse = np.clip(nx * light[0] + ny * light[1] + nz * light[2], 0, 1)
    half = light + np.array([0, 0, 1.0])
    half /= np.linalg.norm(half)
    spec = np.clip(nx * half[0] + ny * half[1] + nz * half[2], 0, 1)

    gold = np.array([0.98, 0.78, 0.36], np.float32)
    lapis = np.array([0.07, 0.20, 0.56], np.float32)
    albedo = np.where(star[..., None], gold, lapis)
    color = albedo * (0.30 + 0.95 * diffuse[..., None])
    color += np.where(star, spec ** 30 * 0.35, spec ** 90 * 0.9)[..., None]
    blurred = Image.fromarray(np.clip((h + 10) * 3, 0, 255).astype(np.uint8), "L").filter(ImageFilter.GaussianBlur(9 * ss))
    occlusion = 1 - np.clip((np.asarray(blurred, dtype=np.float32) / 3 - 10 - h) / 34.0, 0, 0.55)
    color *= occlusion[..., None]

    im = Image.fromarray((np.clip(color, 0, 1) * 255).astype(np.uint8), "RGB").resize((size, size), Image.LANCZOS)
    return rounded(im.convert("RGBA"), size * 104 // 512)


def wordmark(color, cap_height, tracking):
    # The word with letter spacing of `tracking` em, cropped to its ink.
    font = ImageFont.truetype(FONT, 400)
    cap = font.getbbox("R")
    scale = cap_height / (cap[3] - cap[1])
    font = ImageFont.truetype(FONT, round(400 * scale))
    em = font.size
    widths = [font.getlength(c) for c in "RASM"]
    canvas = Image.new("RGBA", (int(sum(widths) + tracking * em * 4 + em), em * 2), (0, 0, 0, 0))
    draw = ImageDraw.Draw(canvas)
    x = em * 0.25
    for c, w in zip("RASM", widths):
        draw.text((x, em * 0.5), c, font=font, fill=color + (255,))
        x += w + tracking * em
    return canvas.crop(canvas.getbbox())


def logo(tile, theme, scale=2):
    # The lockup of the README header: the tile, then the word, centered on each other, with a margin.
    pad, mark, gap = 10 * scale, 100 * scale, 28 * scale
    word = wordmark(WORD_COLOR[theme], 46 * scale, 0.16)
    w = pad * 2 + mark + gap + word.width
    h = pad * 2 + mark
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out.alpha_composite(tile.resize((mark, mark), Image.LANCZOS), (pad, pad))
    out.alpha_composite(word, (pad + mark + gap, pad + (mark - word.height) // 2))
    return out


def main():
    os.makedirs(OUT, exist_ok=True)
    tile = carved_tile(512)
    for size in (512, 180, 32):
        tile.resize((size, size), Image.LANCZOS).save(os.path.join(OUT, f"icon-{size}.png"), optimize=True)
    for theme in ("dark", "light"):
        logo(tile, theme).save(os.path.join(OUT, f"logo-{theme}.png"), optimize=True)
    print("wrote", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
