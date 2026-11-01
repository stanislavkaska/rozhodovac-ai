"""
Vykreslí obrázky do galerie webu (docs/images/gallery_*.jpg).

Obrazovky pocházejí z fw_screen.py (port kreslení firmwaru), krabička je
jednoduchý 3D model podle rozměrového výkresu Waveshare ESP32-S3-Touch-AMOLED-2.16:
tělo 46 × 46 × 22,5 mm s rohy R5,8, černé sklo 43,3 mm, aktivní plocha 38,99 mm,
tři tlačítka Ø5,3 mm nahoře po 10 mm, 9,6 mm od čela.

Spuštění:  python tools/render_gallery.py
Výstup:    gallery_<scéna>.jpg (2000 × 1250, zvětšení po kliknutí)
           gallery_<scéna>_600.jpg a _1200.jpg (zmenšeniny do galerie)
Závislosti: pillow, numpy
"""

import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fw_screen  # noqa: E402

OUT_DIR = fw_screen.REPO / "docs" / "images"
SS = 2                      # supersampling
OUT_W, OUT_H = 2000, 1250   # plné rozlišení pro zvětšení po kliknutí
THUMB_WIDTHS = (600, 1200)   # zmenšeniny do galerie (1× a 2× displej)
SCALE = OUT_W / 1600

# Rozměry v mm
BODY, DEPTH, BODY_R = 46.0, 22.5, 5.8
GLASS, GLASS_R = 43.3, 4.6
ACTIVE, ACTIVE_OFF, ACTIVE_R = 38.99, 3.5, 3.4
BTN_D, BTN_H, BTN_Z = 5.3, 1.1, 9.6
BTN_XS = (13.0, 23.0, 33.0)

BODY_RGB = np.array([238, 239, 236], dtype=float)
LIGHT = np.array([-0.45, 0.75, -0.55])
LIGHT = LIGHT / np.linalg.norm(LIGHT)


# ─── Kamera ─────────────────────────────────────────────────────────────────
class Camera:
    def __init__(self, yaw_deg, pitch_deg, dist=420.0, focal=5600.0 * SCALE, cx=None, cy=None):
        self.yaw, self.pitch = math.radians(yaw_deg), math.radians(pitch_deg)
        self.dist, self.f = dist, focal * SS
        self.cx = (cx if cx is not None else OUT_W * 0.5) * SS
        self.cy = (cy if cy is not None else OUT_H * 0.47) * SS
        self.center = np.array([BODY / 2, BODY / 2, DEPTH / 2])
        cy_, sy_ = math.cos(self.yaw), math.sin(self.yaw)
        cp_, sp_ = math.cos(self.pitch), math.sin(self.pitch)
        ry = np.array([[cy_, 0, sy_], [0, 1, 0], [-sy_, 0, cy_]])
        rx = np.array([[1, 0, 0], [0, cp_, -sp_], [0, sp_, cp_]])
        self.rot = rx @ ry

    def to_cam(self, p):
        q = self.rot @ (np.asarray(p, dtype=float) - self.center)
        q[2] += self.dist
        return q

    def project(self, p):
        q = self.to_cam(p)
        return (self.cx + self.f * q[0] / q[2], self.cy - self.f * q[1] / q[2])

    def facing(self, point, normal):
        q = self.to_cam(point)
        n = self.rot @ np.asarray(normal, dtype=float)
        return float(np.dot(n, q)) < 0


def rounded_square(size, r, n_corner=14):
    """Obrys zaobleného čtverce v rovině XY (proti směru hodinových ručiček)."""
    pts, normals = [], []
    corners = [(size - r, r, -90), (size - r, size - r, 0), (r, size - r, 90), (r, r, 180)]
    for cx, cy, a0 in corners:
        for k in range(n_corner + 1):
            a = math.radians(a0 + 90 * k / n_corner)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
            normals.append((math.cos(a), math.sin(a)))
    return pts, normals


def shade(normal, base=BODY_RGB, amb=0.62, k=0.40):
    n = np.asarray(normal, dtype=float)
    n = n / np.linalg.norm(n)
    lam = max(0.0, float(np.dot(n, LIGHT)))
    c = base * (amb + k * lam)
    return tuple(int(min(255, v)) for v in c)


# ─── Textura čela ───────────────────────────────────────────────────────────
def front_texture(screen: Image.Image, ppm=480 * 4 / ACTIVE):
    """Čelo krabičky (46 × 46 mm) jako RGBA obrázek: bílý okraj, sklo, displej."""
    s = int(BODY * ppm)
    tex = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(tex)
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=int(BODY_R * ppm), fill=(246, 246, 244, 255))
    g0 = int((BODY - GLASS) / 2 * ppm)
    d.rounded_rectangle([g0, g0, s - 1 - g0, s - 1 - g0], radius=int(GLASS_R * ppm), fill=(10, 10, 12, 255))

    a0 = int(((BODY - GLASS) / 2 + 2.15) * ppm)
    asz = int(round(ACTIVE * ppm))  # celočíselný násobek 480, pixely displeje zůstanou stejně široké
    scr = screen.resize((asz, asz), Image.NEAREST).convert("RGBA")
    mask = Image.new("L", (asz, asz), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, asz - 1, asz - 1], radius=int(ACTIVE_R * ppm), fill=255)
    # Lehká záře AMOLEDu do skla
    glow = screen.resize((asz, asz), Image.BILINEAR).filter(ImageFilter.GaussianBlur(ppm * 0.9))
    glow_layer = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    glow_layer.paste(glow.convert("RGBA"), (a0, a0))
    glow_layer.putalpha(glow_layer.convert("L").point(lambda v: int(v * 0.35)))
    glass_mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(glass_mask).rounded_rectangle([g0, g0, s - 1 - g0, s - 1 - g0], radius=int(GLASS_R * ppm), fill=255)
    tex.paste(glow_layer, (0, 0), Image.composite(glow_layer, Image.new("RGBA", (s, s)), glass_mask).split()[3])
    tex.paste(scr, (a0, a0), mask)

    # Odlesk na skle: měkký diagonální přechod z levého horního rohu
    yy, xx = np.mgrid[0:s, 0:s].astype(float)
    t = 1.0 - (xx + yy) / (2.0 * s)
    refl = np.clip((t - 0.45) * 2.2, 0, 1) ** 1.6 * 46
    refl_img = Image.fromarray(refl.astype(np.uint8), "L")
    refl_rgba = Image.new("RGBA", (s, s), (255, 255, 255, 0))
    refl_rgba.putalpha(Image.composite(refl_img, Image.new("L", (s, s), 0), glass_mask))
    tex = Image.alpha_composite(tex, refl_rgba)
    return tex


def perspective_coeffs(dst, src):
    """Koeficienty pro Image.transform(PERSPECTIVE): výstupní bod dst -> vstupní src."""
    a = []
    b = []
    for (x, y), (u, v) in zip(dst, src):
        a.append([x, y, 1, 0, 0, 0, -u * x, -u * y]); b.append(u)
        a.append([0, 0, 0, x, y, 1, -v * x, -v * y]); b.append(v)
    return np.linalg.solve(np.array(a, dtype=float), np.array(b, dtype=float)).tolist()


# ─── Scéna ──────────────────────────────────────────────────────────────────
def background(w, h):
    yy = np.linspace(0, 1, h)[:, None]
    xx = np.linspace(0, 1, w)[None, :]
    top = np.array([16, 18, 24], dtype=float)
    mid = np.array([30, 33, 41], dtype=float)
    desk = np.array([22, 23, 27], dtype=float)
    horizon = 0.56
    img = np.zeros((h, w, 3))
    wall = yy < horizon
    tw = (yy / horizon)
    img[:] = np.where(wall[..., None], top + (mid - top) * tw[..., None], 0)
    td = np.clip((yy - horizon) / (1 - horizon), 0, 1)
    desk_col = mid * 0.78 + (desk - mid * 0.78) * td[..., None]
    img = np.where(wall[..., None], img, desk_col)
    # Měkké světlo zezadu zleva (jako tón webu) a vinětace
    glow = np.exp(-(((xx - 0.30) / 0.35) ** 2 + ((yy - 0.25) / 0.30) ** 2))[..., None]
    img += glow * np.array([217, 119, 87]) * 0.10
    vig = 1 - 0.45 * (((xx - 0.5) / 0.75) ** 2 + ((yy - 0.5) / 0.85) ** 2)
    img *= np.clip(vig, 0.4, 1)[..., None]
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB").convert("RGBA")


def render(screen: Image.Image, cam: Camera, caption_light=None) -> Image.Image:
    W, H = OUT_W * SS, OUT_H * SS
    img = background(W, H)

    # Stín pod krabičkou (průmět půdorysu na stůl, rozmazaný)
    shadow = Image.new("L", (W, H), 0)
    sd = ImageDraw.Draw(shadow)
    foot = [cam.project((x, 0, z)) for x, z in ((-1.5, -1), (BODY + 1.5, -1), (BODY + 4, DEPTH + 6), (-0.5, DEPTH + 6))]
    sd.polygon(foot, fill=200)
    shadow = shadow.filter(ImageFilter.GaussianBlur(14 * SS * SCALE))
    contact = Image.new("L", (W, H), 0)
    ImageDraw.Draw(contact).polygon([cam.project((x, 0, z)) for x, z in ((0.5, 0.3), (BODY - 0.5, 0.3), (BODY - 0.5, DEPTH), (0.5, DEPTH))], fill=255)
    contact = contact.filter(ImageFilter.GaussianBlur(3 * SS * SCALE))
    shadow = Image.fromarray(np.maximum(np.array(shadow) * 0.75, np.array(contact) * 0.9).astype(np.uint8))
    black = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    img = Image.composite(black, img, shadow)

    # Odraz světla displeje na desce stolu
    avg = np.array(screen.resize((1, 1), Image.BILINEAR)).reshape(3).astype(float)
    spill = Image.new("L", (W, H), 0)
    ImageDraw.Draw(spill).polygon([cam.project((x, 0, z)) for x, z in ((-6, -2), (BODY + 6, -2), (BODY + 14, -26), (-14, -26))], fill=255)
    spill = spill.filter(ImageFilter.GaussianBlur(18 * SS * SCALE)).point(lambda v: int(v * 0.22))
    spill_col = Image.new("RGBA", (W, H), tuple(int(min(255, c * 1.6 + 20)) for c in avg) + (255,))
    img = Image.composite(spill_col, img, spill)

    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)

    # Boky tělesa: vytažený zaoblený čtverec, odzadu dopředu
    pts, normals = rounded_square(BODY, BODY_R)
    quads = []
    for i in range(len(pts)):
        (x1, y1), (x2, y2) = pts[i], pts[(i + 1) % len(pts)]
        n1, n2 = normals[i], normals[(i + 1) % len(pts)]
        n = ((n1[0] + n2[0]) / 2, (n1[1] + n2[1]) / 2, 0.0)
        mid = ((x1 + x2) / 2, (y1 + y2) / 2, DEPTH / 2)
        if not cam.facing(mid, n):
            continue
        base = shade(n)
        # Bok rozdělený po hloubce: k zadní hraně mírně tmavne (dojem hloubky)
        steps = 24
        for k in range(steps):
            za, zb = DEPTH * k / steps, DEPTH * (k + 1) / steps
            f = 1.0 - 0.13 * (k + 0.5) / steps
            col = tuple(int(c * f) for c in base)
            poly3 = [(x1, y1, za), (x2, y2, za), (x2, y2, zb), (x1, y1, zb)]
            depth = cam.to_cam(((x1 + x2) / 2, (y1 + y2) / 2, (za + zb) / 2))[2]
            quads.append((depth, [cam.project(p) for p in poly3], col))
    for _, poly, col in sorted(quads, key=lambda q: -q[0]):
        d.polygon(poly, fill=col + (255,), outline=col + (255,))

    # Jemná linka spáry mezi čelem a zadním dílem
    seam = [cam.project((x, y, 9.0)) for x, y in pts]
    for i in range(len(seam) - 1):
        mid = (pts[i][0], pts[i][1], 9.0)
        if cam.facing(mid, (normals[i][0], normals[i][1], 0)):
            d.line([seam[i], seam[i + 1]], fill=(205, 206, 204, 255), width=max(1, SS))

    # Mřížka reproduktoru a otvor mikrofonu na pravém boku
    if cam.facing((BODY, BODY / 2, DEPTH / 2), (1, 0, 0)):
        for k in range(4):
            z0 = 8.0 + k * 1.6
            slot = [(BODY, 22.5 + 0.0, z0), (BODY, 22.5 + 0.0, z0 + 0.7), (BODY, 26.5, z0 + 1.6), (BODY, 26.5, z0 + 0.9)]
            d.polygon([cam.project(p) for p in slot], fill=(150, 152, 150, 255))
        for (yy, zz) in ((36.0, 11.0), (9.0, 18.0)):
            c = cam.project((BODY, yy, zz))
            d.ellipse([c[0] - 2 * SS, c[1] - 2 * SS, c[0] + 2 * SS, c[1] + 2 * SS], fill=(140, 142, 140, 255))

    # Tlačítka na horní hraně (válečky)
    for bx in BTN_XS:
        ring_lo, ring_hi = [], []
        for k in range(36):
            a = 2 * math.pi * k / 36
            px, pz = bx + BTN_D / 2 * math.cos(a), BTN_Z + BTN_D / 2 * math.sin(a)
            ring_lo.append(cam.project((px, BODY, pz)))
            ring_hi.append(cam.project((px, BODY + BTN_H, pz)))
        side = ring_lo + ring_hi
        hull = _convex_hull(side)
        d.polygon(hull, fill=shade((0.2, 0.2, -1), amb=0.55) + (255,))
        d.polygon(ring_hi, fill=shade((0, 1, 0), base=BODY_RGB * 1.02) + (255,), outline=(214, 215, 212, 255))

    img = Image.alpha_composite(img, layer)

    # Čelo s displejem (perspektivní promítnutí textury)
    tex = front_texture(screen)
    ts = tex.size[0]
    dst = [cam.project(p) for p in ((0, BODY, 0), (BODY, BODY, 0), (BODY, 0, 0), (0, 0, 0))]
    src = [(0, 0), (ts, 0), (ts, ts), (0, ts)]
    coeffs = perspective_coeffs(dst, src)
    face = tex.transform((W, H), Image.PERSPECTIVE, coeffs, Image.BICUBIC)
    img = Image.alpha_composite(img, face)

    return img.convert("RGB").resize((OUT_W, OUT_H), Image.LANCZOS)


def _convex_hull(points):
    pts = sorted(set((round(x, 2), round(y, 2)) for x, y in points))
    if len(pts) <= 2:
        return pts

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    lower, upper = [], []
    for p in pts:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    for p in reversed(pts):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return lower[:-1] + upper[:-1]


SHOTS = {
    # název: (scéna, yaw, pitch)
    "home": ("home", 20, -14),
    "danger": ("danger", -18, -14),
    "question": ("question", 22, -12),
    "clock": ("clock", -12, -9),
}


def main(names=None):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for name, (scene, yaw, pitch) in SHOTS.items():
        if names and name not in names:
            continue
        screen = fw_screen.SCENES[scene]()
        out = render(screen, Camera(yaw, pitch))
        path = OUT_DIR / f"gallery_{name}.jpg"
        out.save(path, quality=88, optimize=True, progressive=True)
        print(f"{path.relative_to(fw_screen.REPO)}  {path.stat().st_size // 1024} KB")
        # Zmenšeniny přepočítané Lanczosem s jemným doostřením: prohlížeč by
        # pixelové písmo při zmenšení z plného rozlišení rozmazal
        for tw in THUMB_WIDTHS:
            th = round(OUT_H * tw / OUT_W)
            thumb = out.resize((tw, th), Image.LANCZOS).filter(ImageFilter.UnsharpMask(radius=0.8, percent=70, threshold=2))
            tpath = OUT_DIR / f"gallery_{name}_{tw}.jpg"
            thumb.save(tpath, quality=87, optimize=True, progressive=True)
            print(f"{tpath.relative_to(fw_screen.REPO)}  {tpath.stat().st_size // 1024} KB")


if __name__ == "__main__":
    main(sys.argv[1:])
