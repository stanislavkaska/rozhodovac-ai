"""
Python port kreslení obrazovek firmwaru Rozhodovače (480 × 480, RGB565).

Kopíruje souřadnice, barvy a písmo z src/main.cpp, src/buddy.cpp a
src/hw_waveshare.cpp, aby obrázky na webu odpovídaly skutečnému displeji.
Písmo je klasické 5×7 glcdfont z přibalené knihovny GFX.
Při změně rozvržení ve firmwaru je potřeba upravit i tento soubor.
"""

import math
import re
from pathlib import Path

from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parent.parent
W = H = 480

# ─── Barvy (RGB565 jako ve firmwaru) ────────────────────────────────────────
COL_BG, COL_CARD, COL_CARD_HI, COL_BORDER = 0x0000, 0x1082, 0x2124, 0x39E7
COL_GREEN, COL_GREEN_BG, COL_GREEN_HI = 0x07E0, 0x0320, 0x0540
COL_HOT, COL_RED_BG, COL_RED_HI = 0xFA20, 0x5000, 0x8800
COL_CYAN, COL_YELLOW, COL_AMBER_BG = 0x07FF, 0xFFE0, 0x2080
P_BODY, P_BG, P_TEXT, P_DIM = 0xDBAA, 0x0000, 0xFFFF, 0xAD55

COL_CLAWD, COL_CLAWD_LIGHT, COL_CLAWD_DARK = 0xDBAA, 0xECAF, 0xB2E7
COL_BLUSH, COL_SPARKLE, COL_STAR_ORANGE, COL_ALERT = 0xFB56, 0xFFE0, 0xFD20, 0xFA20

NAV_Y, NAV_H, NAV_X0, NAV_W, NAV_GAP = 424, 44, 36, 96, 8


def rgb(c: int):
    return (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)


def _load_font():
    src = (REPO / "lib/GFX_Library_for_Arduino/src/font/glcdfont.h").read_text(encoding="utf-8", errors="replace")
    body = src[src.index("font[]"):]
    body = body[body.index("{") + 1:body.index("};")]
    return [int(h, 16) for h in re.findall(r"0x[0-9A-Fa-f]{2}", body)]


FONT = _load_font()


class Canvas:
    def __init__(self):
        self.img = Image.new("RGB", (W, H), rgb(P_BG))
        self.d = ImageDraw.Draw(self.img)

    # ─── Primitiva Arduino_GFX ──────────────────────────────────────────────
    def fill_rect(self, x, y, w, h, c):
        if w <= 0 or h <= 0:
            return
        self.d.rectangle([x, y, x + w - 1, y + h - 1], fill=rgb(c))

    def draw_rect(self, x, y, w, h, c):
        self.hline(x, y, w, c); self.hline(x, y + h - 1, w, c)
        self.vline(x, y, h, c); self.vline(x + w - 1, y, h, c)

    def hline(self, x, y, w, c):
        self.fill_rect(x, y, w, 1, c)

    def vline(self, x, y, h, c):
        self.fill_rect(x, y, 1, h, c)

    def _ellipse_helper(self, x, y, r, corner, c):
        # writeEllipseHelper pro kružnici (rx = ry = r)
        rx = ry = r
        rx2, ry2 = rx * rx, ry * ry
        i, xt, yt = -1, 0, ry
        s = (ry2 << 1) + rx2 * (1 - (ry << 1))
        while True:
            while s < 0:
                xt += 1
                s += ry2 * ((xt << 2) + 2)
            if corner & 1: self.hline(x - xt, y - yt, xt - i, c)
            if corner & 2: self.hline(x + i + 1, y - yt, xt - i, c)
            if corner & 4: self.hline(x + i + 1, y + yt, xt - i, c)
            if corner & 8: self.hline(x - xt, y + yt, xt - i, c)
            i = xt
            yt -= 1
            s -= yt * rx2 << 2
            if not ry2 * xt <= rx2 * yt:
                break
        i, yt, xt = -1, 0, rx
        s = (rx2 << 1) + ry2 * (1 - (rx << 1))
        while True:
            while s < 0:
                yt += 1
                s += rx2 * ((yt << 2) + 2)
            if corner & 1: self.vline(x - xt, y - yt, yt - i, c)
            if corner & 2: self.vline(x + xt, y - yt, yt - i, c)
            if corner & 4: self.vline(x + xt, y + i + 1, yt - i, c)
            if corner & 8: self.vline(x - xt, y + i + 1, yt - i, c)
            i = yt
            xt -= 1
            s -= xt * ry2 << 2
            if not rx2 * yt <= ry2 * xt:
                break

    def draw_round_rect(self, x, y, w, h, r, c):
        r = min(r, min(w, h) // 2)
        self.hline(x + r, y, w - 2 * r, c)
        self.hline(x + r, y + h - 1, w - 2 * r, c)
        self.vline(x, y + r, h - 2 * r, c)
        self.vline(x + w - 1, y + r, h - 2 * r, c)
        self._ellipse_helper(x + r, y + r, r, 1, c)
        self._ellipse_helper(x + w - r - 1, y + r, r, 2, c)
        self._ellipse_helper(x + w - r - 1, y + h - r - 1, r, 4, c)
        self._ellipse_helper(x + r, y + h - r - 1, r, 8, c)

    def fill_round_rect(self, x, y, w, h, r, c):
        r = min(r, min(w, h) // 2)
        self.fill_rect(x, y + r, w, h - 2 * r, c)
        # Rohy: pro každý řádek šířka kružnice (výsledek odpovídá writeFillEllipseHelper)
        for dy in range(r + 1):
            dx = int((r * r - dy * dy) ** 0.5 + 0.5)
            self.hline(x + r - dx, y + r - dy, w - 2 * r + 2 * dx, c)
            self.hline(x + r - dx, y + h - r - 1 + dy, w - 2 * r + 2 * dx, c)

    def fill_circle(self, x, y, r, c):
        self.d.ellipse([x - r, y - r, x + r, y + r], fill=rgb(c))

    # ─── Písmo ──────────────────────────────────────────────────────────────
    def draw_char(self, x, y, ch, u, fg, bg):
        code = ord(ch) if isinstance(ch, str) else ch
        if fg != bg:
            self.fill_rect(x, y, 6 * u, 8 * u, bg)
        for i in range(5):
            line = FONT[code * 5 + i]
            for j in range(8):
                if line & (1 << j):
                    self.fill_rect(x + i * u, y + j * u, u, u, fg)

    def _cols(self, x, y, cols, u, fg, bg):
        if fg != bg:
            self.fill_rect(x, y, 6 * u, 8 * u, bg)
        for ci, bits in enumerate(cols):
            for r in range(8):
                if bits & (1 << r):
                    self.fill_rect(x + ci * u, y + r * u, u, u, fg)

    def _acute(self, x, bot, u, fg):
        if u == 1:
            self.fill_rect(x + 2, bot - 1, 1, 1, fg); self.fill_rect(x + 3, bot - 2, 1, 1, fg); return
        h = u // 2
        self.fill_rect(x + 2 * u, bot - u, u, u, fg)
        self.fill_rect(x + 2 * u + h, bot - u - h, u, u, fg)
        self.fill_rect(x + 3 * u, bot - 2 * u, u, u, fg)

    def _caron(self, x, bot, u, fg):
        if u == 1:
            self.fill_rect(x + 1, bot - 2, 1, 1, fg); self.fill_rect(x + 3, bot - 2, 1, 1, fg)
            self.fill_rect(x + 2, bot - 1, 1, 1, fg); return
        h = u // 2
        self.fill_rect(x + 2 * u, bot - (u - h), u, u - h, fg)
        self.fill_rect(x + 2 * u - h, bot - u, 2 * u, h, fg)
        self.fill_rect(x + 1 * u, bot - 2 * u, u, u, fg)
        self.fill_rect(x + 3 * u, bot - 2 * u, u, u, fg)

    def _ring(self, x, bot, u, fg):
        if u == 1:
            self.d.rectangle([x + 1, bot - 3, x + 3, bot - 1], outline=rgb(fg)); return
        s = (u + 1) // 2
        self.fill_rect(x + 2 * u, bot - s, u, s, fg)
        self.fill_rect(x + 2 * u - s, bot - s - u, s, u, fg)
        self.fill_rect(x + 3 * u, bot - s - u, s, u, fg)
        self.fill_rect(x + 2 * u, bot - 2 * s - u, u, s, fg)

    _ACUTE = {"á": "a", "é": "e", "ó": "o", "ú": "u", "ý": "y",
              "Á": "A", "É": "E", "Í": "I", "Ó": "O", "Ú": "U", "Ý": "Y"}
    _CARON = {"č": "c", "ě": "e", "ň": "n", "ř": "r", "š": "s", "ž": "z",
              "Č": "C", "Ď": "D", "Ě": "E", "Ň": "N", "Ř": "R", "Š": "S", "Ť": "T", "Ž": "Z"}

    def draw_glyph(self, x, y, ch, u, fg, bg):
        cp = ord(ch)
        if 0x20 <= cp <= 0x7E:
            self.draw_char(x, y, ch, u, fg, bg); return
        gap = (u + 1) // 2
        lo_bot, up_bot = y + 2 * u - gap, y - gap
        if ch == "í":
            self._cols(x, y, [0x00, 0x44, 0x7C, 0x40, 0x00], u, fg, bg); self._acute(x, lo_bot, u, fg); return
        if ch in self._ACUTE:
            base = self._ACUTE[ch]
            self.draw_char(x, y, base, u, fg, bg)
            self._acute(x, up_bot if base.isupper() else lo_bot, u, fg); return
        if ch in self._CARON:
            base = self._CARON[ch]
            self.draw_char(x, y, base, u, fg, bg)
            self._caron(x, up_bot if base.isupper() else lo_bot, u, fg); return
        if ch == "ů":
            self.draw_char(x, y, "u", u, fg, bg); self._ring(x, lo_bot, u, fg); return
        if ch == "Ů":
            self.draw_char(x, y, "U", u, fg, bg); self._ring(x, up_bot, u, fg); return
        if ch == "ď":
            self._cols(x, y, [0x38, 0x44, 0x7F, 0x00, 0x00], u, fg, bg); self._klicka(x, y, 4, u, fg); return
        if ch == "ť":
            self._cols(x, y, [0x04, 0x3F, 0x44, 0x20, 0x00], u, fg, bg); self._klicka(x, y, 3, u, fg); return
        if ch in "–—−":
            self.draw_char(x, y, "-", u, fg, bg); return
        if ch == "…":
            self._cols(x, y, [0x40, 0x00, 0x40, 0x00, 0x40], u, fg, bg); return
        if ch in "•·":
            self._cols(x, y, [0x00, 0x18, 0x18, 0x00, 0x00], u, fg, bg); return
        if ch == "→":
            self._cols(x, y, [0x08, 0x08, 0x2A, 0x1C, 0x08], u, fg, bg); return
        if ch == "←":
            self._cols(x, y, [0x08, 0x1C, 0x2A, 0x08, 0x08], u, fg, bg); return
        if ch == "↑":
            self._cols(x, y, [0x04, 0x02, 0x7F, 0x02, 0x04], u, fg, bg); return
        if ch == "↓":
            self._cols(x, y, [0x10, 0x20, 0x7F, 0x20, 0x10], u, fg, bg); return
        if ch in "✓✔":
            self._cols(x, y, [0x10, 0x20, 0x10, 0x08, 0x04], u, fg, bg); return
        self.draw_char(x, y, "?", u, fg, bg)

    def _klicka(self, x, y, col, u, fg):
        h, s = u // 2, (u + 1) // 2
        if col >= 4:
            self.fill_rect(x + 4 * u, y - h, u, u + h, fg); self.fill_rect(x + 3 * u + h, y + u, u, s, fg)
        else:
            self.fill_rect(x + 3 * u + h, y - h, u, u + h, fg); self.fill_rect(x + 3 * u, y + u, u, s, fg)

    def text(self, s, x, y, size, fg, bg, max_chars=0xFFFF):
        for i, ch in enumerate(s[:max_chars]):
            self.draw_glyph(x + i * 6 * size, y, ch, size, fg, bg)

    def text_centered(self, s, cx, y, size, fg, bg):
        self.text(s, cx - len(s) * 6 * size // 2, y, size, fg, bg)

    def text_right(self, s, rx, y, size, fg, bg):
        self.text(s, rx - len(s) * 6 * size, y, size, fg, bg)


def wrap_utf8(text, max_rows, max_cols):
    """Port wrapUtf8 z main.cpp (počítá znaky, ne bajty, „\n“ zalomí řádek)."""
    rows, cur = [], ""
    for word in text.replace("\n", " \n ").split(" "):
        if not word:
            continue
        if word == "\n":
            if cur:
                rows.append(cur); cur = ""
                if len(rows) >= max_rows:
                    return rows
            continue
        if cur and len(cur) + 1 + len(word) > max_cols:
            rows.append(cur); cur = ""
            if len(rows) >= max_rows:
                return rows
        if cur:
            cur += " "
        for ch in word:
            if len(cur) >= max_cols:
                rows.append(cur); cur = ""
                if len(rows) >= max_rows:
                    return rows
            cur += ch
    if cur and len(rows) < max_rows:
        rows.append(cur)
    return rows


# ─── Clawd (buddy.cpp) ──────────────────────────────────────────────────────
def _clawd_sprite(cv, cx, top, uW, uH, eye_shift, eye_style, l_arm, r_arm, legs, blush):
    x0 = cx - 8 * uW
    cv.fill_round_rect(x0 + 3 * uW, top + 5 * uH + 2, 10 * uW, max(3, uH // 5), 2, 0x1082)
    bx, bw, bh = x0 + 2 * uW, 12 * uW, 4 * uH
    cv.fill_rect(bx, top, bw, bh, COL_CLAWD)
    hi = max(2, uH // 7)
    cv.fill_rect(bx, top, bw, hi, COL_CLAWD_LIGHT)
    cv.fill_rect(bx, top + bh - hi, bw, hi, COL_CLAWD_DARK)
    la = top + 2 * uH + l_arm * (uH * 3 // 4)
    ra = top + 2 * uH + r_arm * (uH * 3 // 4)
    cv.fill_rect(x0, la, 2 * uW, uH, COL_CLAWD); cv.fill_rect(x0, la, 2 * uW, hi, COL_CLAWD_LIGHT)
    cv.fill_rect(x0 + 14 * uW, ra, 2 * uW, uH, COL_CLAWD); cv.fill_rect(x0 + 14 * uW, ra, 2 * uW, hi, COL_CLAWD_LIGHT)
    ly = top + 4 * uH
    lh = uH // 3 if legs == 3 else uH
    if legs in (0, 3):
        for c in (3, 5, 10, 12):
            cv.fill_rect(x0 + c * uW, ly, uW, lh, COL_CLAWD)
            cv.fill_rect(x0 + c * uW, ly + lh - hi, uW, hi, COL_CLAWD_DARK)
    elif legs == 1:
        for c, h in zip((2, 5, 9, 12), (lh - uH // 4, lh, lh - uH // 4, lh)):
            cv.fill_rect(x0 + c * uW, ly, uW, h, COL_CLAWD)
    elif legs == 2:
        for c, h in zip((3, 6, 10, 13), (lh, lh - uH // 4, lh, lh - uH // 4)):
            cv.fill_rect(x0 + c * uW, ly, uW, h, COL_CLAWD)
    elif legs == 4:
        for c in (2, 4, 11, 13):
            cv.fill_rect(x0 + c * uW, ly, uW, uH * 3 // 4, COL_CLAWD)
    if blush:
        bh2 = max(3, uH // 3)
        cv.fill_rect(x0 + 3 * uW, top + 2 * uH + 2, uW + uW // 3, bh2, COL_BLUSH)
        cv.fill_rect(x0 + 12 * uW - uW // 3, top + 2 * uH + 2, uW + uW // 3, bh2, COL_BLUSH)
    ey = top + uH
    lx, rx = x0 + (4 + eye_shift) * uW, x0 + (11 + eye_shift) * uW
    if eye_style == 0:
        cv.fill_rect(lx, ey, uW, uH, COL_BG); cv.fill_rect(rx, ey, uW, uH, COL_BG)
    elif eye_style == 1:
        sh, sy = max(3, uH // 4), ey + (uH * 2) // 3
        cv.fill_rect(lx - uW // 3, sy, uW + (2 * uW) // 3, sh, COL_BG)
        cv.fill_rect(rx - uW // 3, sy, uW + (2 * uW) // 3, sh, COL_BG)
    elif eye_style == 4:
        cv.fill_rect(lx, ey - 2, uW + 2, uH + 2, COL_BG)
        cv.fill_rect(rx - 2, ey - 2, uW + 2, uH + 2, COL_BG)
        gp = max(2, uW // 3)
        cv.fill_rect(lx + 1, ey, gp, gp, 0xFFFF); cv.fill_rect(rx - 1, ey, gp, gp, 0xFFFF)


def _spinner(cv, cx, cy, ps, frame):
    f = frame % 6
    cv.fill_rect(cx - ps // 2, cy - ps // 2, ps, ps, COL_CLAWD if f == 0 else COL_CLAWD_LIGHT)
    if f == 0:
        return
    d1 = ps + ps // 2
    for dx, dy in ((0, -d1), (0, d1), (-d1, 0), (d1, 0)):
        cv.fill_rect(cx + dx - ps // 2, cy + dy - ps // 2, ps, ps, COL_CLAWD)
    if f == 1:
        return
    dg = ps + (ps // 2 if f >= 4 else 0)
    for dx, dy in ((-dg, -dg), (dg, -dg), (-dg, dg), (dg, dg)):
        cv.fill_rect(cx + dx - ps // 2, cy + dy - ps // 2, ps, ps, COL_CLAWD_LIGHT)
    if f in (3, 4):
        d2 = d1 + ps
        for dx, dy in ((0, -d2), (0, d2), (-d2, 0), (d2, 0)):
            cv.fill_rect(cx + dx - ps // 2, cy + dy - ps // 2, ps, ps, COL_STAR_ORANGE)


def draw_clawd(cv, mode, state, tick):
    """mode: 'full' | 'prompt' | 'peek'; state: 'idle' | 'busy' | 'attention'."""
    top, area_h, uW, uH, ps = 52, 222, 16, 26, 5
    if mode == "prompt":
        area_h, uW, uH, ps = 162, 12, 19, 4
    elif mode == "peek":
        area_h, uW, uH, ps = 104, 9, 14, 3
    cv.fill_rect(0, top, W, area_h, COL_BG)
    base_top = top + (area_h - 5 * uH) // 2 + (12 if mode == "full" else 6)
    if state == "busy":
        dx = -4 if tick % 4 < 2 else 4
        _clawd_sprite(cv, 240 + dx, base_top, uW, uH, (tick // 3) % 3 - 1, 0,
                      -1 if tick % 4 < 2 else 0, 0 if tick % 4 < 2 else -1, 1 if tick % 2 == 0 else 2, False)
        _spinner(cv, 240, base_top - 4 * ps - 4, ps, tick)
    elif state == "attention":
        jump = -6 if tick % 4 == 0 else 0
        _clawd_sprite(cv, 240, base_top + jump, uW, uH, 0, 4,
                      -1 if tick % 2 == 0 else 0, 0 if tick % 2 == 0 else -1, 4 if jump else 0, False)
        col = COL_SPARKLE if tick % 2 == 0 else COL_ALERT
        cx, cy = 240, base_top + jump - 4 * ps - 4
        cv.fill_rect(cx - ps, cy - 3 * ps, 2 * ps, 4 * ps, col)
        cv.fill_rect(cx - ps, cy + 2 * ps, 2 * ps, 2 * ps, col)
    else:  # idle, fáze klidu bez kroku
        _clawd_sprite(cv, 240, base_top, uW, uH, 0, 0, 0, 0, 0, False)


# ─── Agy (buddy.cpp, maskot pro Antigravity CLI) ────────────────────────────
AGY_EDGE, AGY_TOP, AGY_MID, AGY_BOT = 0x5F1F, 0x3C7F, 0x4B7E, 0x79FE
AGY_EYE, AGY_IRIS, AGY_SPARK, AGY_ARM, AGY_SHOE = 0x0888, 0x2971, 0xCFBF, 0x35BF, 0x2B5F
AGY_W, AGY_H, AGY_BASE = 44, 24, 19
COL_HEART_RED, COL_SLEEP_Z = 0xF98A, 0xAD55


def _agy_profile():
    tops = []
    for x in range(AGY_W):
        d = abs((x + 0.5) - 22.0)
        h = 19.0 * math.exp(-0.5 * (d / 9.5) ** 2.6)
        tops.append(AGY_BASE - int(h + 0.5))
    return tops


AGY_TOP_ROW = _agy_profile()


def _agy_body_color(x, y):
    t = AGY_TOP_ROW
    edge = (y == t[x] or y == AGY_BASE or x == 0 or x == AGY_W - 1 or
            (x > 0 and y < t[x - 1]) or (x < AGY_W - 1 and y < t[x + 1]))
    if edge:
        return AGY_EDGE
    if y < 8:
        return AGY_TOP
    if y < 12:
        return AGY_TOP if (x + y) & 1 else AGY_MID
    if y < 14:
        return AGY_MID
    if y < 17:
        return AGY_MID if (x + y) & 1 else AGY_BOT
    return AGY_BOT


def _agy_eye(cell, ex, ey, style):
    if style == 1:
        cell(ex, ey + 3, AGY_EYE); cell(ex + 5, ey + 3, AGY_EYE)
        for i in range(1, 5): cell(ex + i, ey + 4, AGY_EYE)
        return
    if style == 2:
        for i in range(1, 5): cell(ex + i, ey + 2, AGY_EYE)
        cell(ex, ey + 3, AGY_EYE); cell(ex + 5, ey + 3, AGY_EYE)
        return
    if style == 3:
        for i in range(6):
            cell(ex + i, ey + i, AGY_EYE); cell(ex + 5 - i, ey + i, AGY_EYE)
        return
    if style == 5:
        for i in range(1, 5):
            cell(ex + i, ey, AGY_EYE); cell(ex + i, ey + 5, AGY_EYE)
            cell(ex, ey + i, AGY_EYE); cell(ex + 5, ey + i, AGY_EYE)
        return
    wide = style == 4
    for r in range(6):
        for c in range(6):
            corner = r in (0, 5) and c in (0, 5)
            if not corner or wide:
                cell(ex + c, ey + r, AGY_EYE)
    if wide:
        for i in range(1, 5): cell(ex + i, ey - 1, AGY_EYE)
    for r in range(2, 5):
        for c in range(1, 5): cell(ex + c, ey + r, AGY_IRIS)
    cell(ex + 1, ey + 1, 0xFFFF); cell(ex + 2, ey + 1, 0xFFFF); cell(ex + 1, ey + 2, 0xFFFF)
    cell(ex + 4, ey + 4, AGY_SPARK)
    for i in range(1, 5): cell(ex + i, ey - 3, AGY_EYE)
    cell(ex, ey - 2, AGY_EYE); cell(ex + 5, ey - 2, AGY_EYE)


_AGY_ARMS = {
    0: ([(9, 12), (8, 12), (7, 12), (6, 12)], [(5, 11), (5, 12), (5, 13), (4, 11), (4, 13)]),
    1: ([(9, 12), (8, 11), (7, 10), (7, 9), (7, 8)], [(6, 7), (7, 7), (8, 7), (5, 6), (7, 6), (9, 6)]),
    2: ([(9, 12), (8, 11), (7, 11), (6, 10)], [(5, 9), (5, 10), (4, 9), (4, 8), (6, 8)]),
}


def agy_sprite(cv, ox, oy, ps, eye_shift=0, eyes=0, l_arm=0, r_arm=0, l_lift=0, r_lift=0, blush=False, tick=0):
    def cell(cx, cy, col):
        cv.fill_rect(ox + cx * ps, oy + cy * ps, ps, ps, col)

    for side in (0, 1):
        lift = r_lift if side else l_lift
        leg_x, shoe_x, sole_x = (25, 24, 23) if side else (17, 16, 16)
        cv.fill_rect(ox + leg_x * ps, oy + 20 * ps, 2 * ps, (2 - lift) * ps, AGY_TOP)
        cv.fill_rect(ox + shoe_x * ps, oy + (22 - lift) * ps, 4 * ps, ps, AGY_SHOE)
        cv.fill_rect(ox + sole_x * ps, oy + (23 - lift) * ps, 5 * ps, ps, AGY_EDGE)

    for y in range(AGY_BASE + 1):
        for x in range(AGY_W):
            if y >= AGY_TOP_ROW[x]:
                cell(x, y, _agy_body_color(x, y))

    for i, (sx, sy) in enumerate(((21, 3), (25, 5), (20, 5), (22, 7), (13, 16), (31, 17))):
        if (tick + i) % 3 != 0:
            cell(sx, sy, AGY_SPARK)

    if eyes == 3:
        _agy_eye(cell, 14 + eye_shift, 8, 3); _agy_eye(cell, 24 + eye_shift, 8, 5)
    else:
        _agy_eye(cell, 14 + eye_shift, 8, eyes); _agy_eye(cell, 24 + eye_shift, 8, eyes)

    if blush:
        for bx in (13, 14, 29, 30):
            cell(bx, 14, COL_BLUSH)

    for right, pose in ((False, l_arm), (True, r_arm)):
        arm, hand = _AGY_ARMS[pose]
        for (ax, ay) in arm:
            cell(AGY_W - 1 - ax if right else ax, ay, AGY_ARM)
        for (hx, hy) in hand:
            cell(AGY_W - 1 - hx if right else hx, hy, AGY_EDGE)


def _agy_star(cv, cx, cy, s, col):
    for dx, dy in ((0, 0), (0, -s), (0, s), (-s, 0), (s, 0)):
        cv.fill_rect(cx - s // 2 + dx, cy - s // 2 + dy, s, s, col)


def _alert_badge(cv, cx, cy, ps, bright):
    col = COL_SPARKLE if bright else COL_ALERT
    cv.fill_rect(cx - ps, cy - 3 * ps, 2 * ps, 4 * ps, col)
    cv.fill_rect(cx - ps, cy + 2 * ps, 2 * ps, 2 * ps, col)


def draw_agy(cv, mode, state, tick):
    """Port agyTick; mode: 'full' | 'prompt' | 'peek'."""
    top, area_h, ps, bps, y_off = 52, 222, 6, 5, 6
    if mode == "prompt":
        area_h, ps, bps, y_off = 162, 5, 4, 12
    elif mode == "peek":
        area_h, ps, bps, y_off = 104, 3, 3, 4
    cv.fill_rect(0, top, W, area_h, COL_BG)
    sprite_h = AGY_H * ps
    ox = W // 2 - (AGY_W // 2) * ps
    base_y = top + (area_h - sprite_h) // 2 + y_off
    ground_y = base_y + sprite_h + 2
    FLOAT = (0, -1, -2, -2, -1, 0, 1, 1)
    dy = dx = look = l_lift = r_lift = eyes = l_arm = r_arm = 0
    blush = False
    if state == "sleep":
        dy = ps // 2 if (tick // 6) % 2 else 0
        eyes = 1
    elif state == "idle":
        phase = tick % 40
        dy = int(FLOAT[(tick // 2) % 8] * ps / 2)
        eyes = 1 if phase in (6, 23) else 0
        look = 1 if 12 <= phase <= 15 else (-1 if 28 <= phase <= 31 else 0)
        if 34 <= phase <= 37:
            r_arm = 1 if phase % 2 else 2
    elif state == "busy":
        dy = int(FLOAT[tick % 8] * ps / 2)
        l_arm, r_arm = (1, 0) if tick % 4 < 2 else (0, 1)
        l_lift, r_lift = (1, 0) if tick % 2 else (0, 1)
        look = (tick // 3) % 3 - 1
    elif state == "attention":
        dy = -ps if tick % 4 == 0 else 0
        l_arm, r_arm = (1, 2) if tick % 2 else (2, 1)
        eyes = 4
    elif state == "celebrate":
        dy = (-3, -1, 0)[tick % 3] * ps
        l_arm = r_arm = 1
        eyes, blush = 2, True
    elif state == "heart":
        dy = -ps if tick % 4 < 2 else 0
        l_arm = r_arm = 1
        eyes, blush = 2, True

    lift = -dy if dy < 0 else 0
    sh_w = 18 * ps - lift * 2
    cv.fill_round_rect(W // 2 - sh_w // 2 + dx, ground_y, sh_w, max(3, ps // 2), 2, 0x1082)
    ox2, oy2 = ox + dx, base_y + dy
    agy_sprite(cv, ox2, oy2, ps, look, eyes, l_arm, r_arm, l_lift, r_lift, blush, tick)
    cx = W // 2 + dx
    if state == "busy":
        s = max(2, bps - 1)
        if tick % 4 < 2:
            _agy_star(cv, ox2 + 8 * ps, oy2 + 2 * ps, s, AGY_SPARK)
        else:
            _agy_star(cv, ox2 + 36 * ps, oy2 + 3 * ps, s, AGY_SPARK)
        _agy_star(cv, cx, oy2 - 3 * ps, s if tick % 2 else s + 1, AGY_EDGE)
    elif state == "attention":
        _alert_badge(cv, cx, oy2 - 3 * bps - 2, bps, tick % 2 == 0)


def agy_svg(state="idle", tick=0, eyes=None):
    """Agy jako SVG pro simulátor na webu: jedna buňka mřížky = jedna jednotka viewBoxu."""
    class _Small(Canvas):
        def __init__(self):
            self.img = Image.new("RGBA", (AGY_W, AGY_H), (0, 0, 0, 0))
            self.d = ImageDraw.Draw(self.img)

    cv = _Small()
    kw = {"attention": dict(eyes=4, l_arm=1, r_arm=2), "idle": dict()}.get(state, {})
    if eyes is not None:
        kw["eyes"] = eyes
    agy_sprite(cv, 0, 0, 1, tick=tick, **kw)
    px = cv.img.load()
    rects = []
    for y in range(AGY_H):
        x = 0
        while x < AGY_W:
            c = px[x, y]
            if c[3] == 0:
                x += 1
                continue
            x2 = x + 1
            while x2 < AGY_W and px[x2, y] == c:
                x2 += 1
            rects.append(f'<rect x="{x}" y="{y}" width="{x2 - x}" height="1" fill="#{c[0]:02X}{c[1]:02X}{c[2]:02X}"/>')
            x = x2
    return rects


# ─── Části obrazovky (main.cpp) ─────────────────────────────────────────────
def top_bar(cv, link="USB", center=None, center_col=P_DIM, bat_pct=87, charging=True, client="AGY"):
    cv.fill_rect(0, 0, W, 50, P_BG)
    pill, txt = {"BLE": (0x0210, COL_CYAN), "USB": (COL_GREEN_BG, COL_GREEN)}.get(link, (COL_CARD_HI, P_DIM))
    label = f"{client}·{link}" if link in ("BLE", "USB") and client else link
    pw = len(label) * 12 + 20
    cv.fill_round_rect(40, 12, pw, 28, 8, pill)
    cv.draw_round_rect(40, 12, pw, 28, 8, txt)
    cv.text_centered(label, 40 + pw // 2, 19, 2, txt, pill)
    if center:
        cv.text_centered(center, 240, 19, 2, center_col, P_BG)
    bat_col = COL_GREEN if charging else (COL_HOT if bat_pct <= 20 else P_DIM)
    cv.text_right(f"{bat_pct}%", 402, 19, 2, bat_col, P_BG)
    cv.draw_round_rect(408, 16, 26, 18, 3, bat_col)
    cv.fill_rect(434, 21, 3, 8, bat_col)
    cv.fill_rect(411, 19, max(2, bat_pct * 20 // 100), 12, bat_col)
    cv.hline(28, 49, W - 56, 0x2104)


def bottom_nav(cv, active=0):
    cv.fill_rect(0, NAV_Y - 4, W, H - (NAV_Y - 4), P_BG)
    for i, lbl in enumerate(("Akce", "Profil", "Info", "Menu")):
        bx = NAV_X0 + i * (NAV_W + NAV_GAP)
        on = i == active
        bg, bd, fg = (COL_CARD_HI, P_BODY, P_TEXT) if on else (COL_CARD, COL_BORDER, P_DIM)
        cv.fill_round_rect(bx, NAV_Y, NAV_W, NAV_H, 10, bg)
        cv.draw_round_rect(bx, NAV_Y, NAV_W, NAV_H, 10, bd)
        cv.text_centered(lbl, bx + NAV_W // 2, NAV_Y + 15, 2, fg, bg)


def compact_chat(cv, entries, species="Agy"):
    """drawHUD v kompaktním režimu; entries = nejnovější první."""
    x, y, w, h, cols, show = 24, 278, 432, 136, 33, 5
    cv.fill_rect(0, y - 2, W, NAV_Y - y + 2, P_BG)
    cv.fill_round_rect(x, y, w, h, 12, COL_CARD)
    cv.draw_round_rect(x, y, w, h, 12, COL_BORDER)
    cv.text_right(f"{species} [zvětšit]", x + w - 12, y + 10, 1, P_BODY, COL_CARD)
    rows = []
    for idx, e in enumerate(entries):
        rows += [(r, idx == 0) for r in wrap_utf8(e, 48, cols)]
    for i, (r, newest) in enumerate(rows[:show]):
        cv.text(r, x + 14, y + 24 + i * 22, 2, P_TEXT if newest else P_DIM, COL_CARD)
    bottom_nav(cv, 0)


def trunc_ell(text, cols):
    """truncEll: jeden řádek nejvýš cols znaků, delší končí „…“."""
    return text if len(text) <= cols else text[:cols - 1] + "…"


def _opt_label(cv, lbl, sub, cx, y, fg, bg):
    """drawOptLabel: do 8 znaků velikost 3, jinak velikost 2 na 1–3 řádky po 16
    znacích; na tři řádky se drobná nápověda pod popiskem nevejde."""
    if len(lbl) <= 8:
        cv.text_centered(lbl, cx, y + 20, 3, fg, bg)
        cv.text_centered(sub, cx, y + 54, 1, P_DIM, bg)
        return
    rows = wrap_clamp(lbl, 3, 16)
    if len(rows) <= 1:
        cv.text_centered(rows[0] if rows else "", cx, y + 25, 2, fg, bg)
    elif len(rows) == 2:
        cv.text_centered(rows[0], cx, y + 12, 2, fg, bg)
        cv.text_centered(rows[1], cx, y + 32, 2, fg, bg)
    else:
        for r, row in enumerate(rows):
            cv.text_centered(row, cx, y + 6 + r * 20, 2, fg, bg)
        return
    cv.text_centered(sub, cx, y + 54, 1, P_DIM, bg)


def approval(cv, tool_label, hint, opt_a="", opt_b="", waited=4, urgent_phase=True,
             remaining=None, danger=False):
    """drawApproval pro dvě volby. remaining = zbývající sekundy (most posílá ttl),
    None = Claude Desktop (ukazuje se, jak dlouho dotaz čeká). danger = kritická
    akce, schvaluje se podržením."""
    cv.fill_rect(0, 216, W, H - 216, P_BG)
    hot = (remaining <= 15) if remaining is not None else (waited >= 10)
    border = COL_HOT if (hot and urgent_phase) else COL_YELLOW
    cv.fill_round_rect(20, 216, 440, 156, 14, COL_AMBER_BG)
    cv.draw_round_rect(20, 216, 440, 156, 14, border)
    cv.draw_round_rect(21, 217, 438, 154, 13, border)
    is_q = bool(opt_a)
    cv.text("DOTAZ / ROZHODNUTÍ" if is_q else "ROZHODNOUT AKCI?", 36, 228, 2,
            COL_HOT if hot else COL_YELLOW, COL_AMBER_BG)
    wait_txt = f"zbývá {remaining}s" if remaining is not None else f"{waited}s"
    cv.text_right(wait_txt, 444, 228, 2, COL_HOT if hot else P_DIM, COL_AMBER_BG)
    size = 3 if len(tool_label) <= 18 else 2
    cv.text(trunc_ell(tool_label, 34), 36, 258, size, P_TEXT, COL_AMBER_BG)
    for r, row in enumerate(wrap_clamp(hint, 4, 34)):
        cv.text(row, 36, 292 + r * 20, 2, 0xDEFB, COL_AMBER_BG)
    for (bx, base, fg, lbl, sub) in ((24, COL_GREEN_BG, COL_GREEN, opt_a or "SCHVÁLIT", "[dotyk / BOOT]"),
                                      (250, COL_RED_BG, COL_HOT, opt_b or "ZAMÍTNOUT", "[dotyk / IO18]")):
        cv.fill_round_rect(bx, 384, 206, 74, 14, base)
        cv.draw_round_rect(bx, 384, 206, 74, 14, fg)
        cv.draw_round_rect(bx + 1, 385, 204, 72, 13, fg)
        if danger and bx == 24:
            cv.text_centered(lbl, bx + 103, 384 + 12, 3, fg, base)
            cv.text_centered("podržet 1 s", bx + 103, 384 + 46, 2, P_DIM, base)
        else:
            _opt_label(cv, lbl, sub, bx + 103, 384, fg, base)


def desk_clock(cv, hms, date_line):
    cv.fill_rect(0, 50, W, NAV_Y - 4 - 50, P_BG)
    cv.text_centered(hms, 240, 184, 7, P_TEXT, P_BG)
    cv.text_centered(date_line, 240, 268, 2, P_BODY, P_BG)
    bottom_nav(cv, 0)


# ─── Obrazovka otázky (drawQuestionScreen, "q":1) ───────────────────────────
Q_CARD_Y, Q_BAR_Y, Q_BAR_H, Q_GAP, Q_ROW_H, Q_PEEK_H = 58, 414, 54, 6, 20, 22
Q_TEXT_COLS, Q_HINT_COLS = 31, 34
Q_ACCENTS = (COL_GREEN, COL_CYAN, COL_YELLOW, 0xFBE0)


def wrap_clamp(text, max_rows, cols):
    rows = wrap_utf8(text, max_rows + 1, cols)
    if len(rows) > max_rows:
        rows = rows[:max_rows]
        rows[-1] = rows[-1][:cols - 1] + "…"
    return rows


def _q_row_h(lr, dr):
    return max(54, 14 + (lr + dr) * Q_ROW_H)


def q_layout(hint, options, descs, buttons, q_first=0, cursor_opt=-1, armed_opt=-1):
    """Port qComputeLayout. Vrací slovník rozložení a nové q_first."""
    n = len(options)
    L = {"n": n, "first": 1, "last": 0, "above": False, "below": False, "y": {}, "h": {}, "dm": 0}
    L["bottom"] = Q_BAR_Y - 8 if buttons else H - 12
    hint_max = 5 if n else (L["bottom"] - Q_CARD_Y - 46) // Q_ROW_H
    L["hint_rows"] = len(wrap_clamp(hint, hint_max, Q_HINT_COLS))
    L["card_h"] = 44 + L["hint_rows"] * Q_ROW_H if n else L["bottom"] - Q_CARD_Y
    L["top"] = Q_CARD_Y + L["card_h"] + 8
    if not n:
        return L, q_first
    area = L["bottom"] - L["top"]
    for dm in (3, 2):
        lr = [max(1, len(wrap_clamp(o, 2, Q_TEXT_COLS))) for o in options]
        dr = [len(wrap_clamp(d, dm, Q_TEXT_COLS)) if d else 0 for d in descs]
        if 0 <= armed_opt < n:
            dr[armed_opt] = max(dr[armed_opt], 1)
        L["dm"] = dm
        if sum(_q_row_h(a, b) for a, b in zip(lr, dr)) + Q_GAP * (n - 1) <= area:
            y = L["top"]
            for i in range(n):
                L["y"][i], L["h"][i] = y, _q_row_h(lr[i], dr[i]); y += L["h"][i] + Q_GAP
            L["first"], L["last"] = 0, n - 1
            return L, 0
    # Posouvání: nad první volbou konec předchozí (Q_PEEK_H), pod poslední celou
    # začátek další, oříznutý na spodním okraji; vpravo posuvník
    q_first = min(q_first, n - 1)
    if 0 <= cursor_opt < q_first:
        q_first = cursor_opt
    while True:
        y = L["top"] + (Q_PEEK_H + Q_GAP if q_first > 0 else 0)
        last = q_first
        for i in range(q_first, n):
            h = _q_row_h(lr[i], dr[i])
            if i > q_first and y + h > L["bottom"]:
                break
            L["y"][i], L["h"][i] = y, h; last = i; y += h + Q_GAP
        if cursor_opt > last and q_first < cursor_opt:
            q_first += 1
            continue
        L.update(first=q_first, last=last, above=q_first > 0, below=last < n - 1)
        if L["above"]:
            a = q_first - 1
            L["h"][a] = _q_row_h(lr[a], dr[a])
            L["y"][a] = L["top"] + Q_PEEK_H - L["h"][a]
        if L["below"]:
            b = last + 1
            L["h"][b] = _q_row_h(lr[b], dr[b])
            L["y"][b] = y
        return L, q_first


def _q_option(cv, L, i, options, descs, multi, checked, cursor, armed):
    in_list = L["first"] <= i <= L["last"]
    bx, bw, by, bh = 24, 432, L["y"][i], L["h"][i]
    is_cur = in_list and cursor == i
    is_chk = multi and i in checked
    is_arm = in_list and not multi and is_cur and armed
    accent = Q_ACCENTS[i % 4]
    bg = COL_GREEN_BG if is_arm else (0x18E4 if is_cur else COL_CARD)
    bd = accent if (is_cur or is_arm) else (COL_GREEN if is_chk else 0x2965)
    cv.fill_round_rect(bx, by, bw, bh, 10, bg)
    cv.draw_round_rect(bx, by, bw, bh, 10, bd)
    if is_cur or is_arm:
        cv.draw_round_rect(bx + 1, by + 1, bw - 2, bh - 2, 9, bd)
    lrows = wrap_clamp(options[i], 2, Q_TEXT_COLS)
    drows = wrap_clamp(descs[i], L["dm"], Q_TEXT_COLS) if descs[i] else []
    if is_arm:
        drows = ["klepni znovu = odeslat"]
    ty = by + (bh - ((len(lrows) + len(drows)) * Q_ROW_H - 4)) // 2
    if multi:
        box = COL_GREEN if is_chk else accent
        cv.draw_rect(bx + 12, ty - 2, 22, 20, box)
        cv.draw_rect(bx + 13, ty - 1, 20, 18, box)
        if is_chk:
            cv.fill_rect(bx + 15, ty + 1, 16, 14, COL_GREEN)
            cv.text("✓", bx + 17, ty, 2, COL_BG, COL_GREEN)
    else:
        cv.text(f"{i + 1}.", bx + 12, ty, 2, accent, bg)
    for r, row in enumerate(lrows):
        cv.text(row, bx + 44, ty + r * Q_ROW_H, 2, P_TEXT, bg)
    for r, row in enumerate(drows):
        cv.text(row, bx + 44, ty + (len(lrows) + r) * Q_ROW_H, 2, COL_GREEN if is_arm else P_DIM, bg)


def question_screen(cv, head, hint, options=(), descs=None, multi=False, checked=(), cursor=0,
                    armed=False, back=False, term=True, ok="", remaining=None, waited=3,
                    q_first=0, sent=False):
    """drawQuestionScreen: volby pod sebou s popisem, zaškrtávání, ZPĚT / TERMINÁL / POTVRDIT.
    cursor = pořadí vybrané položky (volby, za nimi spodní tlačítka)."""
    options = list(options)
    descs = list(descs or [""] * len(options))
    buttons = (["back"] if back else []) + (["term"] if term else []) + (["ok"] if multi or not options else [])
    L, q_first = q_layout(hint, options, descs, buttons, q_first, cursor if cursor < len(options) else -1,
                          cursor if armed and not multi else -1)
    cv.fill_rect(0, 50, W, H - 50, P_BG)
    hot = (remaining <= 15) if remaining is not None else (waited >= 10)
    border = COL_HOT if hot else COL_YELLOW
    if not sent and options:
        # Volby včetně ořezaných krajních, přesah se přemaže, karta se kreslí přes ně
        idx = ([L["first"] - 1] if L["above"] else []) + list(range(L["first"], L["last"] + 1)) \
            + ([L["last"] + 1] if L["below"] else [])
        for i in idx:
            _q_option(cv, L, i, options, descs, multi, checked, cursor, armed)
        cv.fill_rect(0, 50, W, L["top"] - 50, P_BG)
        cv.fill_rect(0, L["bottom"], W, H - L["bottom"], P_BG)
        if L["above"] or L["below"]:
            track = L["bottom"] - L["top"]
            th = max(20, track * (L["last"] - L["first"] + 1) // L["n"])
            ty = min(L["top"] + track * L["first"] // L["n"], L["bottom"] - th)
            cv.fill_round_rect(463, L["top"], 5, track, 2, 0x2965)
            cv.fill_round_rect(463, ty, 5, th, 2, P_DIM)
    cv.fill_round_rect(20, Q_CARD_Y, 440, L["card_h"], 14, COL_AMBER_BG)
    cv.draw_round_rect(20, Q_CARD_Y, 440, L["card_h"], 14, border)
    cv.draw_round_rect(21, Q_CARD_Y + 1, 438, L["card_h"] - 2, 13, border)
    wait_txt = f"zbývá {remaining}s" if remaining is not None else f"{waited}s"
    cv.text_right(wait_txt, 444, Q_CARD_Y + 12, 2, COL_HOT if hot else P_DIM, COL_AMBER_BG)
    head_cols = max(4, (444 - len(wait_txt) * 12 - 12 - 36) // 12)
    cv.text(trunc_ell(head, head_cols), 36, Q_CARD_Y + 12, 2, COL_HOT if hot else COL_YELLOW, COL_AMBER_BG)
    for r, row in enumerate(wrap_clamp(hint, L["hint_rows"], Q_HINT_COLS)):
        cv.text(row, 36, Q_CARD_Y + 38 + r * Q_ROW_H, 2, 0xDEFB, COL_AMBER_BG)
    if sent:
        top = L["top"] if options else Q_BAR_Y
        h = H - 12 - top
        cv.fill_round_rect(24, top, 432, h, 14, COL_CARD_HI)
        cv.text_centered("Odesláno…", 240, top + h // 2 - 10, 3, P_DIM, COL_CARD_HI)
        return
    nb = len(buttons)
    for b, kind in enumerate(buttons):
        w = (432 - 8 * (nb - 1)) // nb
        x = 24 + b * (w + 8)
        is_cur = cursor == len(options) + b
        if kind == "back":
            lbl, fg, bg = "← ZPĚT", COL_CYAN, 0x0210
        elif kind == "term":
            lbl, fg, bg = "TERMINÁL", P_TEXT, COL_CARD
        else:
            lbl, fg, bg = ok or "POTVRDIT", COL_GREEN, COL_GREEN_BG
            if multi and not checked:
                fg, bg = COL_BORDER, COL_CARD
        cv.fill_round_rect(x, Q_BAR_Y, w, Q_BAR_H, 12, bg)
        cv.draw_round_rect(x, Q_BAR_Y, w, Q_BAR_H, 12, fg)
        if is_cur:
            cv.draw_round_rect(x + 1, Q_BAR_Y + 1, w - 2, Q_BAR_H - 2, 11, fg)
        cv.text_centered(lbl, x + w // 2, Q_BAR_Y + (Q_BAR_H - 16) // 2, 2, fg, bg)


# ─── Scény pro galerii ──────────────────────────────────────────────────────
def scene_home():
    cv = Canvas()
    top_bar(cv, "BLE", "* 1 běží", COL_CYAN, client="CLAUDE")
    draw_clawd(cv, "full", "busy", 3)
    compact_chat(cv, [
        "09:42 Úprava: main.cpp",
        "09:41 Příkaz: Překlad firmwaru (pio run)",
        "09:40 Čtení: data.h",
        "09:39 > přidej ukládání jasu do NVS",
    ], species="Clawd")
    return cv.img


def scene_question():
    cv = Canvas()
    top_bar(cv, "USB", "! 1 čeká", COL_HOT, client="AGY")
    draw_agy(cv, "prompt", "attention", 1)
    # AGY posílá u dotazu zbývající čas (otázka 90 s)
    approval(cv, "Dotaz na postup", "Jaký přístup zvolíme pro novou architekturu?",
             "Moduly", "Monolit", waited=4, remaining=86)
    return cv.img


def scene_danger():
    cv = Canvas()
    top_bar(cv, "BLE", "! 1 čeká", COL_HOT, bat_pct=64, charging=False, client="CLAUDE")
    draw_clawd(cv, "prompt", "attention", 0)
    # Claude Desktop rizikovou akci neoznačuje, krabička ji pozná podle příkazu
    # a schvaluje se podržením; Claude čas na odpověď neposílá, ukazuje se čekání
    approval(cv, "Spustit příkaz", "git push --force origin main", waited=12, danger=True)
    return cv.img


def scene_clock():
    cv = Canvas()
    top_bar(cv, "USB", "22:47", P_DIM, bat_pct=100, charging=False, client="AGY")
    desk_clock(cv, "22:47:18", "Čtvrtek 29.10.2026")
    return cv.img


def scene_q_single():
    cv = Canvas()
    top_bar(cv, "USB", "! 1 čeká", COL_HOT, client="CLAUDE")
    question_screen(cv, "1/3 · Knihovna", "Kterou knihovnu použít pro grafy na přehledu mezd?",
                    ["Chart.js", "ECharts", "D3.js"],
                    ["Doporučeno. Jednoduchá a malá, stačí na sloupce a čáry",
                     "Hodně typů grafů, větší balík", "Plná kontrola nad vzhledem, nejvíc práce"],
                    cursor=1, armed=True, remaining=52)
    return cv.img


def scene_q_multi():
    cv = Canvas()
    top_bar(cv, "USB", "! 1 čeká", COL_HOT, client="CLAUDE")
    question_screen(cv, "2/3 · Funkce", "Které funkce má graf mít?",
                    ["Export", "Přiblížení", "Legenda", "Popisky hodnot"],
                    ["Uložení do PNG a CSV", "", "Klikací, skryje řadu", "Hodnota po najetí prstem"],
                    multi=True, checked=(0, 2), cursor=2, back=True, ok="DALŠÍ", remaining=41)
    return cv.img


def scene_q_scroll():
    cv = Canvas()
    top_bar(cv, "USB", "! 1 čeká", COL_HOT, client="CLAUDE")
    question_screen(cv, "3/3 · Nasazení", "Kam a jak novou verzi přehledu nasadit, až bude hotová a otestovaná?",
                    ["Jen na DEV", "DEV a REL", "Ostrá verze", "Zatím nikam"],
                    ["Nahraje na vývojový server, nic dalšího se nemění",
                     "Vývojový server a připraví podklady pro vydání verze",
                     "Nahraje všude a vytvoří verzi pro zákazníky, nejde vrátit",
                     "Změny zůstanou jen v pracovní kopii"],
                    cursor=0, back=True, remaining=58)
    return cv.img


def scene_q_summary():
    cv = Canvas()
    top_bar(cv, "USB", "! 1 čeká", COL_HOT, client="CLAUDE")
    question_screen(cv, "Shrnutí odpovědí",
                    "Knihovna: ECharts\nFunkce: Export, Legenda\nNasazení: Jen na DEV",
                    [], back=True, ok="ODESLAT", cursor=2, remaining=57)
    return cv.img


def scene_q_perm():
    cv = Canvas()
    top_bar(cv, "USB", "! 1 čeká", COL_HOT, client="CLAUDE")
    question_screen(cv, "Příkaz · rozhodovac", "npm test -- --watch=false | Spustí testy",
                    ["Ano", "Ano, už se neptat", "Ne"],
                    ["", "Příkaz npm test:* v tomto projektu", ""], cursor=2, remaining=24)
    return cv.img


SCENES = {"home": scene_home, "question": scene_question, "danger": scene_danger, "clock": scene_clock}
# Obrazovka otázky pro Claude Code v terminálu (zatím mimo galerii webu)
Q_SCENES = {"q_single": scene_q_single, "q_multi": scene_q_multi, "q_scroll": scene_q_scroll,
            "q_summary": scene_q_summary, "q_perm": scene_q_perm}
