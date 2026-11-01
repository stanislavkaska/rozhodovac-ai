#include "buddy.h"
#include "hw_waveshare.h"
#include <Arduino.h>

// Persona states matching main.cpp
enum { B_SLEEP = 0, B_IDLE = 1, B_BUSY = 2, B_ATTENTION = 3, B_CELEBRATE = 4, B_DIZZY = 5, B_HEART = 6 };

// Authentic Claude Code Clawd colors (RGB565)
static const uint16_t COL_BG          = 0x0000;
static const uint16_t COL_CLAWD       = 0xDBAA; // #D97757 Terracotta coral-orange
static const uint16_t COL_CLAWD_LIGHT = 0xECAF; // #E8967B Warm top highlight
static const uint16_t COL_CLAWD_DARK  = 0xB2E7; // #B25D3C Subtle bottom leg/claw shade
static const uint16_t COL_BLUSH       = 0xFB56; // #FF6B8A Cute pink blush cheeks
static const uint16_t COL_HEART_RED   = 0xF98A; // #FF3355 Pixel heart
static const uint16_t COL_SPARKLE     = 0xFFE0; // Gold sparkle
static const uint16_t COL_STAR_ORANGE = 0xFD20; // Claude spinner amber-orange
static const uint16_t COL_SLEEP_Z     = 0xAD55; // Soft silver Zzz
static const uint16_t COL_ALERT       = 0xFA20; // Bright orange-red !

static ClawdSizeMode _mode = CLAWD_FULL;
static BuddyCharacter _char = CHAR_CLAWD;
static uint8_t  _lastState = 0xFF;
static ClawdSizeMode _lastMode = (ClawdSizeMode)0xFF;
static uint32_t _tick = 0;
static uint32_t _nextTickAt = 0;
static const uint32_t TICK_MS = 140;

void buddyInit() {
  _tick = 0;
  _nextTickAt = 0;
  _lastState = 0xFF;
}

void buddyInvalidate() {
  _lastState = 0xFF;
}

void buddySetMode(ClawdSizeMode mode) {
  if (_mode != mode) {
    _mode = mode;
    _lastState = 0xFF;
  }
}

void buddySetCharacter(BuddyCharacter ch) {
  if (_char != ch) {
    _char = ch;
    _lastState = 0xFF;
  }
}

const char* buddySpeciesName() {
  return _char == CHAR_AGY ? "Agy" : "Clawd";
}

// Helper: draw a pixel-art heart centered at (cx, cy) with pixel size `ps`
static void drawPixelHeart(Arduino_GFX* g, int cx, int cy, int ps, uint16_t col) {
  static const uint8_t H[6] = {
    0b0110110,
    0b1111111,
    0b1111111,
    0b0111110,
    0b0011100,
    0b0001000
  };
  int x0 = cx - (7 * ps) / 2;
  int y0 = cy - (6 * ps) / 2;
  for (int r = 0; r < 6; r++) {
    for (int c = 0; c < 7; c++) {
      if (H[r] & (1 << (6 - c))) {
        g->fillRect(x0 + c * ps, y0 + r * ps, ps, ps, col);
      }
    }
  }
}

// Helper: draw the iconic Claude Code thinking starburst spinner (6 frames)
static void drawClaudeSpinner(Arduino_GFX* g, int cx, int cy, int ps, uint32_t frame) {
  uint8_t f = frame % 6;
  uint16_t cMain = COL_CLAWD;
  uint16_t cHi   = COL_CLAWD_LIGHT;

  // Center core pixel
  g->fillRect(cx - ps / 2, cy - ps / 2, ps, ps, (f == 0) ? cMain : cHi);
  if (f == 0) return; // '·' dot

  // Orthogonal rays (+)
  int d1 = ps + ps / 2;
  g->fillRect(cx - ps / 2, cy - d1 - ps / 2, ps, ps, cMain);
  g->fillRect(cx - ps / 2, cy + d1 - ps / 2, ps, ps, cMain);
  g->fillRect(cx - d1 - ps / 2, cy - ps / 2, ps, ps, cMain);
  g->fillRect(cx + d1 - ps / 2, cy - ps / 2, ps, ps, cMain);
  if (f == 1) return; // '✢'

  // Diagonal rays (✳ / ✶)
  int diag = ps + (f >= 4 ? ps / 2 : 0);
  g->fillRect(cx - diag - ps / 2, cy - diag - ps / 2, ps, ps, cHi);
  g->fillRect(cx + diag - ps / 2, cy - diag - ps / 2, ps, ps, cHi);
  g->fillRect(cx - diag - ps / 2, cy + diag - ps / 2, ps, ps, cHi);
  g->fillRect(cx + diag - ps / 2, cy + diag - ps / 2, ps, ps, cHi);

  // Extended tips on peak frames (✻ / ✽)
  if (f == 3 || f == 4) {
    int d2 = d1 + ps;
    g->fillRect(cx - ps / 2, cy - d2 - ps / 2, ps, ps, COL_STAR_ORANGE);
    g->fillRect(cx - ps / 2, cy + d2 - ps / 2, ps, ps, COL_STAR_ORANGE);
    g->fillRect(cx - d2 - ps / 2, cy - ps / 2, ps, ps, COL_STAR_ORANGE);
    g->fillRect(cx + d2 - ps / 2, cy - ps / 2, ps, ps, COL_STAR_ORANGE);
  }
}

// Helper: draw crisp pixel-art '!' alert badge
static void drawAlertBadge(Arduino_GFX* g, int cx, int cy, int ps, bool bright) {
  uint16_t col = bright ? COL_SPARKLE : COL_ALERT;
  g->fillRect(cx - ps, cy - 3 * ps, 2 * ps, 4 * ps, col);
  g->fillRect(cx - ps, cy + 2 * ps, 2 * ps, 2 * ps, col);
}

// Core pixel-accurate Clawd sprite renderer!
// Canonical 16x5 quadrant matrix decoded directly from Claude Code CLI:
//   Row 0:   cols 2..13  (12-wide head top)
//   Row 1:   cols 2..13  with eyes at col 4 and col 11
//   Row 2:   cols 0..15  (12-wide torso + 2-wide left & right crab claws/arms)
//   Row 3:   cols 2..13  (12-wide lower body)
//   Row 4:   legs at cols [3, 5] and [10, 12]
static void drawClawdSprite(
  Arduino_GFX* g,
  int cx, int topY, int uW, int uH,
  int8_t eyeShiftX,   // -1, 0, +1
  uint8_t eyeStyle,   // 0=normal, 1=closed/blink, 2=happy ^^, 3=dizzy XO, 4=wide alert
  int8_t leftArmRow,  // -1=raised(R1), 0=normal(R2), +1=lowered(R2+half)
  int8_t rightArmRow, // -1=raised(R1), 0=normal(R2), +1=lowered(R2+half)
  uint8_t legPose,    // 0=neutral, 1=step A, 2=step B, 3=tucked(sleep), 4=jump spread
  bool blush
) {
  int x0 = cx - 8 * uW; // 16 units wide total (0..15)

  // Subtle ground shadow under Clawd on AMOLED
  g->fillRoundRect(x0 + 3 * uW, topY + 5 * uH + 2, 10 * uW, max(3, uH / 5), 2, 0x1082);

  // 1. Main 12-unit wide rectangular body (cols 2..13, rows 0..3 -> 12*uW x 4*uH)
  int bodyX = x0 + 2 * uW;
  int bodyW = 12 * uW;
  int bodyH = 4 * uH;
  g->fillRect(bodyX, topY, bodyW, bodyH, COL_CLAWD);

  // Crisp top edge pixel highlight & bottom edge shade for subtle depth
  int hiH = max(2, uH / 7);
  g->fillRect(bodyX, topY, bodyW, hiH, COL_CLAWD_LIGHT);
  g->fillRect(bodyX, topY + bodyH - hiH, bodyW, hiH, COL_CLAWD_DARK);

  // 2. Side Claws / Arms (2 units wide each: cols 0..1 on left, cols 14..15 on right)
  int lArmY = topY + 2 * uH + leftArmRow * (uH * 3 / 4);
  int rArmY = topY + 2 * uH + rightArmRow * (uH * 3 / 4);
  g->fillRect(x0,            lArmY, 2 * uW, uH, COL_CLAWD);
  g->fillRect(x0,            lArmY, 2 * uW, hiH, COL_CLAWD_LIGHT);
  g->fillRect(x0 + 14 * uW,  rArmY, 2 * uW, uH, COL_CLAWD);
  g->fillRect(x0 + 14 * uW,  rArmY, 2 * uW, hiH, COL_CLAWD_LIGHT);

  // 3. Four Cute Crab Legs (Row 4)
  int legY = topY + 4 * uH;
  int legH = (legPose == 3) ? (uH / 3) : uH;
  if (legPose == 0 || legPose == 3) {
    const int cols[4] = { 3, 5, 10, 12 };
    for (int i = 0; i < 4; i++) {
      g->fillRect(x0 + cols[i] * uW, legY, uW, legH, COL_CLAWD);
      g->fillRect(x0 + cols[i] * uW, legY + legH - hiH, uW, hiH, COL_CLAWD_DARK);
    }
  } else if (legPose == 1) {
    // Crab scuttle frame A
    const int cols[4] = { 2, 5, 9, 12 };
    const int hs[4]   = { legH - uH / 4, legH, legH - uH / 4, legH };
    for (int i = 0; i < 4; i++) {
      g->fillRect(x0 + cols[i] * uW, legY, uW, hs[i], COL_CLAWD);
    }
  } else if (legPose == 2) {
    // Crab scuttle frame B
    const int cols[4] = { 3, 6, 10, 13 };
    const int hs[4]   = { legH, legH - uH / 4, legH, legH - uH / 4 };
    for (int i = 0; i < 4; i++) {
      g->fillRect(x0 + cols[i] * uW, legY, uW, hs[i], COL_CLAWD);
    }
  } else if (legPose == 4) {
    // Jump / celebrate splayed legs
    const int cols[4] = { 2, 4, 11, 13 };
    int jh = (uH * 3) / 4;
    for (int i = 0; i < 4; i++) {
      g->fillRect(x0 + cols[i] * uW, legY, uW, jh, COL_CLAWD);
    }
  }

  // 4. Optional Pink Blush Cheeks (Row 2 upper outer corners of face)
  if (blush) {
    int bh = max(3, uH / 3);
    g->fillRect(x0 + 3 * uW,  topY + 2 * uH + 2, uW + uW / 3, bh, COL_BLUSH);
    g->fillRect(x0 + 12 * uW - uW / 3, topY + 2 * uH + 2, uW + uW / 3, bh, COL_BLUSH);
  }

  // 5. Eyes (Row 1: cols 4 and 11, shifted optionally by eyeShiftX)
  int eyeY = topY + uH;
  int lx = x0 + (4 + eyeShiftX) * uW;
  int rx = x0 + (11 + eyeShiftX) * uW;

  if (eyeStyle == 0) {
    // Classic Clawd tall rectangular black pixel eyes
    g->fillRect(lx, eyeY, uW, uH, COL_BG);
    g->fillRect(rx, eyeY, uW, uH, COL_BG);
  } else if (eyeStyle == 1) {
    // Sleeping / blinking closed slits
    int sh = max(3, uH / 4);
    int sy = eyeY + (uH * 2) / 3;
    g->fillRect(lx - uW / 3, sy, uW + (2 * uW) / 3, sh, COL_BG);
    g->fillRect(rx - uW / 3, sy, uW + (2 * uW) / 3, sh, COL_BG);
  } else if (eyeStyle == 2) {
    // Happy ^ ^ pixel chevrons
    int p = max(3, uW / 2);
    int cy = eyeY + uH / 3;
    g->fillRect(lx,         cy,     uW, p, COL_BG);
    g->fillRect(lx - p,     cy + p, p,  p, COL_BG);
    g->fillRect(lx + uW,    cy + p, p,  p, COL_BG);

    g->fillRect(rx,         cy,     uW, p, COL_BG);
    g->fillRect(rx - p,     cy + p, p,  p, COL_BG);
    g->fillRect(rx + uW,    cy + p, p,  p, COL_BG);
  } else if (eyeStyle == 3) {
    // Dizzy X and O pixel eyes
    int p = max(2, uW / 3);
    // Left eye: X
    g->fillRect(lx - p / 2,      eyeY + p,         p, p, COL_BG);
    g->fillRect(lx + uW - p / 2, eyeY + p,         p, p, COL_BG);
    g->fillRect(lx + uW / 3,     eyeY + uH / 2,    p, p, COL_BG);
    g->fillRect(lx - p / 2,      eyeY + uH - 2*p,  p, p, COL_BG);
    g->fillRect(lx + uW - p / 2, eyeY + uH - 2*p,  p, p, COL_BG);
    // Right eye: hollow square O
    g->fillRect(rx - p / 2, eyeY + p, uW + p, uH - 2 * p, COL_BG);
    g->fillRect(rx + p / 2, eyeY + 2 * p, max(2, uW - p), max(2, uH - 4 * p), COL_CLAWD);
  } else if (eyeStyle == 4) {
    // Wide alert eyes with crisp white pixel glint
    g->fillRect(lx, eyeY - 2, uW + 2, uH + 2, COL_BG);
    g->fillRect(rx - 2, eyeY - 2, uW + 2, uH + 2, COL_BG);
    int gp = max(2, uW / 3);
    g->fillRect(lx + 1, eyeY, gp, gp, 0xFFFF);
    g->fillRect(rx - 1, eyeY, gp, gp, 0xFFFF);
  }
}

// ─── Agy: maskot pro Antigravity CLI ───────────────────────────────────────
// Zvonovité tělo s modrofialovým přechodem a azurovým okrajem, velké oči,
// tenké ručky a botky. Kreslí se do logické mřížky 44 × 24 buněk,
// jedna buňka má ps × ps pixelů. Port pro web je v tools/fw_screen.py.
static const uint16_t AGY_EDGE  = 0x5F1F; // #5CE1FF azurový okraj
static const uint16_t AGY_TOP   = 0x3C7F; // #3B8CFF modrá nahoře
static const uint16_t AGY_MID   = 0x4B7E; // #4A6CF7 přechod
static const uint16_t AGY_BOT   = 0x79FE; // #7A3FF0 fialová dole
static const uint16_t AGY_EYE   = 0x0888; // #0E1240 tmavé oči a obočí
static const uint16_t AGY_IRIS  = 0x2971; // #2B2F8F duhovka
static const uint16_t AGY_SPARK = 0xCFBF; // #CFF6FF jiskry
static const uint16_t AGY_ARM   = 0x35BF; // #36B6FF ručky
static const uint16_t AGY_SHOE  = 0x2B5F; // #2A6BFF botky

static const int AGY_W = 44;
static const int AGY_H = 24;
static const int AGY_BASE = 19;           // řádek spodního lemu těla
static int8_t _agyTop[AGY_W];             // první řádek těla v každém sloupci
static bool   _agyProfileReady = false;

// Obrys těla: zvon s kulatým vrcholem a rozšířeným lemem
// (zobecněná Gaussova křivka: výška 19 buněk, šířka 9,5 buňky, exponent 2,6)
static void agyInitProfile() {
  for (int x = 0; x < AGY_W; x++) {
    float d = fabsf((x + 0.5f) - 22.0f);
    float h = 19.0f * expf(-0.5f * powf(d / 9.5f, 2.6f));
    _agyTop[x] = (int8_t)(AGY_BASE - (int)(h + 0.5f));
  }
  _agyProfileReady = true;
}

static uint16_t agyBodyColor(int x, int y) {
  bool edge = (y == _agyTop[x]) || (y == AGY_BASE) || x == 0 || x == AGY_W - 1 ||
              (x > 0 && y < _agyTop[x - 1]) || (x < AGY_W - 1 && y < _agyTop[x + 1]);
  if (edge)   return AGY_EDGE;
  if (y < 8)  return AGY_TOP;
  if (y < 12) return ((x + y) & 1) ? AGY_TOP : AGY_MID;
  if (y < 14) return AGY_MID;
  if (y < 17) return ((x + y) & 1) ? AGY_MID : AGY_BOT;
  return AGY_BOT;
}

static inline void agyCell(Arduino_GFX* g, int ox, int oy, int ps, int cx, int cy, uint16_t col) {
  g->fillRect(ox + cx * ps, oy + cy * ps, ps, ps, col);
}

// Velké oko v rámečku 6 × 6 s levým horním rohem (ex, ey).
// style: 0 normální, 1 zavřené, 2 šťastné, 3 X, 4 vykulené, 5 O
static void agyEye(Arduino_GFX* g, int ox, int oy, int ps, int ex, int ey, uint8_t style) {
  switch (style) {
    case 1:
      agyCell(g, ox, oy, ps, ex, ey + 3, AGY_EYE);
      agyCell(g, ox, oy, ps, ex + 5, ey + 3, AGY_EYE);
      for (int i = 1; i <= 4; i++) agyCell(g, ox, oy, ps, ex + i, ey + 4, AGY_EYE);
      return;
    case 2:
      for (int i = 1; i <= 4; i++) agyCell(g, ox, oy, ps, ex + i, ey + 2, AGY_EYE);
      agyCell(g, ox, oy, ps, ex, ey + 3, AGY_EYE);
      agyCell(g, ox, oy, ps, ex + 5, ey + 3, AGY_EYE);
      return;
    case 3:
      for (int i = 0; i < 6; i++) {
        agyCell(g, ox, oy, ps, ex + i, ey + i, AGY_EYE);
        agyCell(g, ox, oy, ps, ex + 5 - i, ey + i, AGY_EYE);
      }
      return;
    case 5:
      for (int i = 1; i <= 4; i++) {
        agyCell(g, ox, oy, ps, ex + i, ey, AGY_EYE);
        agyCell(g, ox, oy, ps, ex + i, ey + 5, AGY_EYE);
        agyCell(g, ox, oy, ps, ex, ey + i, AGY_EYE);
        agyCell(g, ox, oy, ps, ex + 5, ey + i, AGY_EYE);
      }
      return;
    default: {
      bool wide = (style == 4);
      for (int r = 0; r < 6; r++) {
        for (int c = 0; c < 6; c++) {
          bool corner = (r == 0 || r == 5) && (c == 0 || c == 5);
          if (!corner || wide) agyCell(g, ox, oy, ps, ex + c, ey + r, AGY_EYE);
        }
      }
      if (wide) {
        for (int i = 1; i <= 4; i++) agyCell(g, ox, oy, ps, ex + i, ey - 1, AGY_EYE);
      }
      // duhovka, velký lesk vlevo nahoře, malá jiskra vpravo dole
      for (int r = 2; r <= 4; r++)
        for (int c = 1; c <= 4; c++) agyCell(g, ox, oy, ps, ex + c, ey + r, AGY_IRIS);
      agyCell(g, ox, oy, ps, ex + 1, ey + 1, 0xFFFF);
      agyCell(g, ox, oy, ps, ex + 2, ey + 1, 0xFFFF);
      agyCell(g, ox, oy, ps, ex + 1, ey + 2, 0xFFFF);
      agyCell(g, ox, oy, ps, ex + 4, ey + 4, AGY_SPARK);
      // obočí jako oblouček nad okem
      for (int i = 1; i <= 4; i++) agyCell(g, ox, oy, ps, ex + i, ey - 3, AGY_EYE);
      agyCell(g, ox, oy, ps, ex, ey - 2, AGY_EYE);
      agyCell(g, ox, oy, ps, ex + 5, ey - 2, AGY_EYE);
      return;
    }
  }
}

// Ruka: 0 = do strany, 1 = zvednutá, 2 = napůl (mávnutí). Levá ruka, pravá je zrcadlo.
static void agyArm(Arduino_GFX* g, int ox, int oy, int ps, bool right, uint8_t pose) {
  static const int8_t SIDE_ARM[][2]  = { {9, 12}, {8, 12}, {7, 12}, {6, 12} };
  static const int8_t SIDE_HAND[][2] = { {5, 11}, {5, 12}, {5, 13}, {4, 11}, {4, 13} };
  static const int8_t UP_ARM[][2]    = { {9, 12}, {8, 11}, {7, 10}, {7, 9}, {7, 8} };
  static const int8_t UP_HAND[][2]   = { {6, 7}, {7, 7}, {8, 7}, {5, 6}, {7, 6}, {9, 6} };
  static const int8_t HALF_ARM[][2]  = { {9, 12}, {8, 11}, {7, 11}, {6, 10} };
  static const int8_t HALF_HAND[][2] = { {5, 9}, {5, 10}, {4, 9}, {4, 8}, {6, 8} };

  const int8_t (*arm)[2] = SIDE_ARM;
  const int8_t (*hand)[2] = SIDE_HAND;
  int nArm = 4, nHand = 5;
  if (pose == 1) { arm = UP_ARM; hand = UP_HAND; nArm = 5; nHand = 6; }
  else if (pose == 2) { arm = HALF_ARM; hand = HALF_HAND; nArm = 4; nHand = 5; }

  for (int i = 0; i < nArm; i++) {
    int cx = right ? (AGY_W - 1 - arm[i][0]) : arm[i][0];
    agyCell(g, ox, oy, ps, cx, arm[i][1], AGY_ARM);
  }
  for (int i = 0; i < nHand; i++) {
    int cx = right ? (AGY_W - 1 - hand[i][0]) : hand[i][0];
    agyCell(g, ox, oy, ps, cx, hand[i][1], AGY_EDGE);
  }
}

// Nožičky s botkami; lift zvedne botku o buňku (krok)
static void agyFeet(Arduino_GFX* g, int ox, int oy, int ps, int8_t lLift, int8_t rLift) {
  for (int side = 0; side < 2; side++) {
    int lift = side ? rLift : lLift;
    int legX = side ? 25 : 17;
    g->fillRect(ox + legX * ps, oy + 20 * ps, 2 * ps, (2 - lift) * ps, AGY_TOP);
    int shoeX = side ? 24 : 16;
    int soleX = side ? 23 : 16;
    g->fillRect(ox + shoeX * ps, oy + (22 - lift) * ps, 4 * ps, ps, AGY_SHOE);
    g->fillRect(ox + soleX * ps, oy + (23 - lift) * ps, 5 * ps, ps, AGY_EDGE);
  }
}

// Čtyřcípá hvězdička
static void agyStar(Arduino_GFX* g, int cx, int cy, int s, uint16_t col) {
  g->fillRect(cx - s / 2, cy - s / 2, s, s, col);
  g->fillRect(cx - s / 2, cy - s / 2 - s, s, s, col);
  g->fillRect(cx - s / 2, cy + s / 2, s, s, col);
  g->fillRect(cx - s / 2 - s, cy - s / 2, s, s, col);
  g->fillRect(cx + s / 2, cy - s / 2, s, s, col);
}

static void drawAgySprite(Arduino_GFX* g, int ox, int oy, int ps, int8_t eyeShift, uint8_t eyeStyle,
                          uint8_t lArm, uint8_t rArm, int8_t lLift, int8_t rLift, bool blush) {
  if (!_agyProfileReady) agyInitProfile();

  agyFeet(g, ox, oy, ps, lLift, rLift);

  // Tělo po vodorovných úsecích stejné barvy
  for (int y = 0; y <= AGY_BASE; y++) {
    int x = 0;
    while (x < AGY_W) {
      if (y < _agyTop[x]) { x++; continue; }
      uint16_t c = agyBodyColor(x, y);
      int x2 = x + 1;
      while (x2 < AGY_W && y >= _agyTop[x2] && agyBodyColor(x2, y) == c) x2++;
      g->fillRect(ox + x * ps, oy + y * ps, (x2 - x) * ps, ps, c);
      x = x2;
    }
  }

  // Třpytky uvnitř těla, střídavě blikají
  static const int8_t SPARKS[][2] = { {21, 3}, {25, 5}, {20, 5}, {22, 7}, {13, 16}, {31, 17} };
  for (int i = 0; i < 6; i++) {
    if (((_tick + i) % 3) != 0) agyCell(g, ox, oy, ps, SPARKS[i][0], SPARKS[i][1], AGY_SPARK);
  }

  if (eyeStyle == 3) {               // motání hlavy: X a O
    agyEye(g, ox, oy, ps, 14 + eyeShift, 8, 3);
    agyEye(g, ox, oy, ps, 24 + eyeShift, 8, 5);
  } else {
    agyEye(g, ox, oy, ps, 14 + eyeShift, 8, eyeStyle);
    agyEye(g, ox, oy, ps, 24 + eyeShift, 8, eyeStyle);
  }

  if (blush) {
    agyCell(g, ox, oy, ps, 13, 14, COL_BLUSH);
    agyCell(g, ox, oy, ps, 14, 14, COL_BLUSH);
    agyCell(g, ox, oy, ps, 29, 14, COL_BLUSH);
    agyCell(g, ox, oy, ps, 30, 14, COL_BLUSH);
  }

  agyArm(g, ox, oy, ps, false, lArm);
  agyArm(g, ox, oy, ps, true, rArm);
}

static void agyTick(Arduino_GFX* g, uint8_t personaState, int areaTop, int areaH) {
  int ps = 6, bps = 5, yOff = 6;     // celá obrazovka: 264 × 144 px
  if (_mode == CLAWD_PROMPT) { ps = 5; bps = 4; yOff = 12; }   // 220 × 120 px
  else if (_mode == CLAWD_PEEK) { ps = 3; bps = 3; yOff = 4; } // 132 × 72 px

  const int spriteH = AGY_H * ps;
  const int ox = LCD_WIDTH / 2 - (AGY_W / 2) * ps;
  const int baseY = areaTop + (areaH - spriteH) / 2 + yOff;
  const int groundY = baseY + spriteH + 2;
  // Vznášení: posun v polovinách buňky
  static const int8_t FLOAT[8] = { 0, -1, -2, -2, -1, 0, 1, 1 };

  int dy = 0, dx = 0;
  int8_t look = 0, lLift = 0, rLift = 0;
  uint8_t eyes = 0, lArm = 0, rArm = 0;
  bool blush = false;

  switch (personaState) {
    case B_SLEEP:
      dy = ((_tick / 6) % 2) ? ps / 2 : 0;
      eyes = 1;
      break;
    case B_IDLE: {
      uint32_t phase = _tick % 40;
      dy = FLOAT[(_tick / 2) % 8] * ps / 2;
      if (phase == 6 || phase == 23) eyes = 1;
      if (phase >= 12 && phase <= 15) look = 1;
      else if (phase >= 28 && phase <= 31) look = -1;
      if (phase >= 34 && phase <= 37) rArm = (phase % 2) ? 1 : 2;
      break;
    }
    case B_BUSY:
      dy = FLOAT[_tick % 8] * ps / 2;
      lArm = (_tick % 4 < 2) ? 1 : 0;
      rArm = (_tick % 4 < 2) ? 0 : 1;
      lLift = (_tick % 2) ? 1 : 0;
      rLift = (_tick % 2) ? 0 : 1;
      look = (int8_t)((_tick / 3) % 3) - 1;
      break;
    case B_ATTENTION:
      dy = (_tick % 4 == 0) ? -ps : 0;
      lArm = (_tick % 2) ? 1 : 2;
      rArm = (_tick % 2) ? 2 : 1;
      eyes = 4;
      break;
    case B_CELEBRATE: {
      static const int8_t JUMP[3] = { -3, -1, 0 };
      dy = JUMP[_tick % 3] * ps;
      lArm = rArm = 1;
      eyes = 2;
      blush = true;
      break;
    }
    case B_DIZZY: {
      static const int8_t WOB[4] = { -10, 0, 10, 0 };
      dx = WOB[_tick % 4];
      lArm = (_tick % 2) ? 1 : 0;
      rArm = (_tick % 2) ? 0 : 1;
      eyes = 3;
      break;
    }
    case B_HEART:
      dy = (_tick % 4 < 2) ? -ps : 0;
      lArm = rArm = 1;
      eyes = 2;
      blush = true;
      break;
  }

  // Stín zůstává na zemi, při vznášení se zužuje
  int lift = dy < 0 ? -dy : 0;
  int shW = 18 * ps - lift * 2;
  g->fillRoundRect(LCD_WIDTH / 2 - shW / 2 + dx, groundY, shW, max(3, ps / 2), 2, 0x1082);

  const int ox2 = ox + dx, oy2 = baseY + dy;
  drawAgySprite(g, ox2, oy2, ps, look, eyes, lArm, rArm, lLift, rLift, blush);

  const int cx = LCD_WIDTH / 2 + dx;
  switch (personaState) {
    case B_SLEEP:
      if (_mode != CLAWD_PEEK) {
        int zStep = (_tick / 4) % 4;
        g->setTextColor(COL_SLEEP_Z, COL_BG);
        g->setTextSize(_mode == CLAWD_FULL ? 2 : 1);
        g->setCursor(cx + 12 * ps, oy2 - zStep * 4);
        g->print("z");
        if (zStep >= 1) {
          g->setTextColor(AGY_EDGE, COL_BG);
          g->setTextSize(_mode == CLAWD_FULL ? 3 : 2);
          g->setCursor(cx + 14 * ps + 10, oy2 - 18 - zStep * 5);
          g->print("Z");
        }
      }
      break;
    case B_BUSY: {
      // Přemýšlení: hvězdičky střídavě vlevo a vpravo nad tělem
      int s = max(2, bps - 1);
      if (_tick % 4 < 2) agyStar(g, ox2 + 8 * ps, oy2 + 2 * ps, s, AGY_SPARK);
      else               agyStar(g, ox2 + 36 * ps, oy2 + 3 * ps, s, AGY_SPARK);
      agyStar(g, cx, oy2 - 3 * ps, (_tick % 2) ? s : s + 1, AGY_EDGE);
      break;
    }
    case B_ATTENTION:
      drawAlertBadge(g, cx, oy2 - 3 * bps - 2, bps, (_tick % 2) == 0);
      break;
    case B_CELEBRATE:
      agyStar(g, ox2 + 4 * ps, oy2 + 2 * ps - (int)(_tick % 3) * 2, bps, COL_SPARKLE);
      agyStar(g, ox2 + 40 * ps, oy2 + 4 * ps - (int)((_tick + 1) % 3) * 2, bps, AGY_SPARK);
      break;
    case B_DIZZY: {
      int starX = cx + ((_tick % 4 < 2) ? -12 * ps : 12 * ps);
      g->fillRect(starX, oy2 - 2 * bps, 2 * bps, 2 * bps, COL_SPARKLE);
      g->fillRect(cx - (starX - cx), oy2, bps + 1, bps + 1, AGY_SPARK);
      break;
    }
    case B_HEART: {
      int liftH = (_tick % 6) * 4;
      drawPixelHeart(g, cx, oy2 + 2 * bps - liftH, bps, COL_HEART_RED);
      if (_mode == CLAWD_FULL) {
        drawPixelHeart(g, cx - 14 * ps, oy2 + 3 * ps - liftH / 2, max(2, bps - 2), COL_BLUSH);
        drawPixelHeart(g, cx + 14 * ps, oy2 + 2 * ps - liftH / 2, max(2, bps - 2), COL_BLUSH);
      }
      break;
    }
  }
}

void buddyTick(uint8_t personaState) {
  Arduino_GFX* g = hwCanvas();
  if (!g) return;

  uint32_t now = millis();
  bool ticked = false;
  if ((int32_t)(now - _nextTickAt) >= 0) {
    _nextTickAt = now + TICK_MS;
    _tick++;
    ticked = true;
  }

  if (personaState > B_HEART) personaState = B_IDLE;
  if (!ticked && personaState == _lastState && _mode == _lastMode) {
    return;
  }
  _lastState = personaState;
  _lastMode  = _mode;

  // Viewport geometry per screen mode
  int areaTop = 52;
  int areaH   = 222; // Home screen: y=52..274
  int uW = 16, uH = 26, ps = 5;

  if (_mode == CLAWD_PROMPT) {
    areaH = 162;     // Approval prompt: y=52..214
    uW = 12; uH = 19; ps = 4;
  } else if (_mode == CLAWD_PEEK) {
    areaH = 104;     // Compact header: y=52..156
    uW = 9; uH = 14; ps = 3;
  }

  // Od konce horní lišty (y=50): pruh 50–51 jinak držel zbytky srdíček
  g->fillRect(0, areaTop - 2, LCD_WIDTH, areaH + 2, COL_BG);

  if (_char == CHAR_AGY) {
    agyTick(g, personaState, areaTop, areaH);
    return;
  }

  int spriteH = 5 * uH;
  int baseCx  = 240;
  int baseTop = areaTop + (areaH - spriteH) / 2 + (_mode == CLAWD_FULL ? 12 : 6);

  switch (personaState) {
    case B_SLEEP: {
      // Gentle breathing bob + floating Zzz
      int bob = ((_tick / 6) % 2 == 0) ? 2 : 0;
      drawClawdSprite(g, baseCx, baseTop + bob, uW, uH - (bob ? 1 : 0), 0, 1, 1, 1, 3, false);

      int zStep = (_tick / 4) % 4;
      if (_mode != CLAWD_PEEK) {
        g->setTextColor(COL_SLEEP_Z, COL_BG);
        g->setTextSize(_mode == CLAWD_FULL ? 2 : 1);
        g->setCursor(baseCx + 5 * uW, baseTop - 10 - zStep * 4);
        g->print("z");
        if (zStep >= 1) {
          g->setTextColor(COL_CLAWD_LIGHT, COL_BG);
          g->setTextSize(_mode == CLAWD_FULL ? 3 : 2);
          g->setCursor(baseCx + 6 * uW + 10, baseTop - 28 - zStep * 5);
          g->print("Z");
        }
      }
      break;
    }

    case B_IDLE: {
      // Periodic crab scuttle, blink, eye glance & occasional friendly claw wave
      uint32_t phase = _tick % 40;
      int dx = 0;
      uint8_t legs = 0;
      if (phase >= 10 && phase < 18) {
        dx = ((phase - 10) < 4) ? (int)(phase - 9) * 3 : (int)(17 - phase) * 3;
        legs = (phase % 2 == 0) ? 1 : 2;
      } else if (phase >= 26 && phase < 34) {
        dx = -(((phase - 26) < 4) ? (int)(phase - 25) * 3 : (int)(33 - phase) * 3);
        legs = (phase % 2 == 0) ? 2 : 1;
      }

      bool blink = (phase == 6 || phase == 23);
      int8_t look = 0;
      if (phase >= 12 && phase <= 15) look = 1;
      else if (phase >= 28 && phase <= 31) look = -1;

      int8_t rArm = (phase == 36 || phase == 37) ? -1 : 0;
      drawClawdSprite(g, baseCx + dx, baseTop, uW, uH, look, blink ? 1 : 0, 0, rArm, legs, false);
      break;
    }

    case B_BUSY: {
      // Authentic Claude Code "Thinking..." animation: energetic crab scuttle + spinning starburst!
      int dx = ((_tick % 4) < 2) ? -4 : 4;
      uint8_t legs = (_tick % 2 == 0) ? 1 : 2;
      int8_t lArm = (_tick % 4 < 2) ? -1 : 0;
      int8_t rArm = (_tick % 4 < 2) ? 0 : -1;
      int8_t look = ((_tick / 3) % 3) - 1;

      drawClawdSprite(g, baseCx + dx, baseTop, uW, uH, look, 0, lArm, rArm, legs, false);
      drawClaudeSpinner(g, baseCx, baseTop - 4 * ps - 4, ps, _tick);
      break;
    }

    case B_ATTENTION: {
      // Waiting for approval: alert jump + waving claws + pulsing '!' badge
      int jump = (_tick % 4 == 0) ? -6 : 0;
      int8_t lArm = (_tick % 2 == 0) ? -1 : 0;
      int8_t rArm = (_tick % 2 == 0) ? 0 : -1;
      drawClawdSprite(g, baseCx, baseTop + jump, uW, uH, 0, 4, lArm, rArm, jump ? 4 : 0, false);
      // Odznak „!“ nesmí sahat do horní lišty
      int by = baseTop + jump - 4 * ps - 4;
      if (by < areaTop + 3 * ps) by = areaTop + 3 * ps;
      drawAlertBadge(g, baseCx, by, ps, (_tick % 2) == 0);
      break;
    }

    case B_CELEBRATE: {
      // Happy celebration bounce + ^^ eyes + cheering claws + pixel sparkles!
      int jump = ((_tick % 3) == 0) ? -12 : (((_tick % 3) == 1) ? -5 : 0);
      drawClawdSprite(g, baseCx, baseTop + jump, uW, uH, 0, 2, -1, -1, jump < -6 ? 4 : 0, true);

      // Confetti sparkles around Clawd
      drawClaudeSpinner(g, baseCx - 7 * uW, baseTop - 2 * ps, max(2, ps - 1), _tick);
      drawClaudeSpinner(g, baseCx + 7 * uW, baseTop - 3 * ps, max(2, ps - 1), _tick + 3);
      break;
    }

    case B_DIZZY: {
      // Shaken cube: wobble side-to-side + XO eyes + orbiting stars
      static const int8_t WOB[4] = { -10, 0, 10, 0 };
      int dx = WOB[_tick % 4];
      drawClawdSprite(g, baseCx + dx, baseTop, uW, uH, 0, 3, (_tick % 2) ? -1 : 1, (_tick % 2) ? 1 : -1, 1 + (_tick % 2), false);

      int starX = baseCx + ((_tick % 4 < 2) ? -4 * uW : 4 * uW);
      g->fillRect(starX, baseTop - 4 * ps, 2 * ps, 2 * ps, COL_SPARKLE);
      g->fillRect(baseCx - (starX - baseCx), baseTop - 2 * ps, ps + 1, ps + 1, COL_CLAWD_LIGHT);
      break;
    }

    case B_HEART: {
      // Petted / quick approval: blushing Clawd + rising pixel hearts!
      int bounce = (_tick % 4 < 2) ? -4 : 0;
      drawClawdSprite(g, baseCx, baseTop + bounce, uW, uH, 0, 2, -1, -1, 0, true);

      int lift = (_tick % 6) * 4;
      // Stoupající srdíčko končí pod horní lištou
      int hy = baseTop - 4 * ps - lift;
      if (hy < areaTop + 3 * ps) hy = areaTop + 3 * ps;
      drawPixelHeart(g, baseCx, hy, ps, COL_HEART_RED);
      if (_mode == CLAWD_FULL) {
        drawPixelHeart(g, baseCx - 6 * uW, baseTop - ps - (lift / 2), max(2, ps - 2), COL_BLUSH);
        drawPixelHeart(g, baseCx + 6 * uW, baseTop - 2 * ps - (lift / 2), max(2, ps - 2), COL_BLUSH);
      }
      break;
    }
  }
}
