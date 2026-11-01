#pragma once

#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"

// Debounced physical GPIO button (active-low)
// Pin vzorkuje časovač každých 5 ms nezávisle na hlavní smyčce, která kvůli
// kreslení celé obrazovky trvá desítky ms. Stisk i puštění se zapamatují, takže
// krátké cvaknutí ani rychlé opakované stisky mezi dvěma průchody smyčky nezmizí.
struct HwButton {
  uint8_t  pin = 0;

  // Stav vzorkování (zapisuje časovač, čte update() pod zámkem)
  bool     rawLast = false;
  bool     stable = false;
  uint32_t rawChangeMs = 0;
  uint32_t stableSinceMs = 0;
  uint8_t  pressCnt = 0;
  uint8_t  releaseCnt = 0;

  // Stav pro jeden průchod smyčky
  bool     cur = false;
  bool     pressedEdge = false;
  bool     releasedEdge = false;
  uint32_t pressStartMs = 0;
  uint8_t  seenPress = 0;
  uint8_t  seenRelease = 0;

  void init(uint8_t gpioPin);
  void sample();   // volá časovač
  void update();   // volá hwUpdate() jednou za průchod smyčky

  bool isPressed() const   { return cur; }
  bool wasPressed() const  { return pressedEdge; }
  bool wasReleased() const { return releasedEdge; }
  bool pressedFor(uint32_t ms) const { return cur && (millis() - pressStartMs) >= ms; }
};

enum TouchEventType : uint8_t {
  TOUCH_NONE = 0,
  TOUCH_TAP,
  TOUCH_LONG_PRESS,
  TOUCH_SWIPE_LEFT,
  TOUCH_SWIPE_RIGHT,
  TOUCH_SWIPE_UP,
  TOUCH_SWIPE_DOWN
};

struct TouchEvent {
  TouchEventType type = TOUCH_NONE;
  int16_t x = 0;          // Tap / gesture origin X
  int16_t y = 0;          // Tap / gesture origin Y
  bool    touching = false;
  int16_t curX = 0;       // Current finger X while touching
  int16_t curY = 0;       // Current finger Y while touching
};

struct HwPowerInfo {
  bool     pmuOnline = false;
  int      batteryPct = 100;
  int      batteryMv = 4150;
  int      vbusMv = 5000;
  bool     usbPresent = true;
  bool     batteryConnected = false;
  bool     charging = false;
  bool     chargeDone = false;
  int      tempC = 25;
};

struct HwDateTime {
  uint16_t year = 2026;
  uint8_t  month = 1;
  uint8_t  day = 1;
  uint8_t  wday = 0;   // 0=Sun..6=Sat
  uint8_t  hour = 0;
  uint8_t  minute = 0;
  uint8_t  second = 0;
};

// Lifecycle
bool hwInit();
void hwUpdate();

// Display & PSRAM Canvas (480x480)
Arduino_Canvas* hwCanvas();
Arduino_CO5300* hwDisplay();
void hwFlush();
void hwSetBrightness(uint8_t val);    // 0..255 AMOLED brightness
void hwSetDisplayPower(bool on);      // CO5300 displayOn / displayOff

// UTF-8 Czech text rendering & layout helpers (6x8 monospace pitch scaled by size)
uint32_t hwNextUtf8(const char*& p);
size_t   hwUtf8Len(const char* text);
int      hwDrawUtf8(Arduino_GFX* g, const char* text, int x, int y, uint8_t size,
                    uint16_t fg, uint16_t bg, size_t maxChars = 0xFFFF);
int      hwPrintfUtf8(Arduino_GFX* g, int x, int y, uint8_t size,
                      uint16_t fg, uint16_t bg, const char* fmt, ...);
void     hwDrawTextCentered(Arduino_GFX* g, const char* text, int cx, int y, uint8_t size, uint16_t fg, uint16_t bg);
void     hwDrawTextRight(Arduino_GFX* g, const char* text, int rightX, int y, uint8_t size, uint16_t fg, uint16_t bg);

// Touch & Buttons
const TouchEvent& hwTouch();
bool hwTakePalmCover();
HwButton& hwBtnBoot();    // Top Key2 (GPIO 0) — Approve / Next Screen / Hold=Menu
HwButton& hwBtnUser();    // Top Key3 (GPIO 18) — Deny / Next Page / Confirm
bool hwTakePowerKeyShortPress();

// Power (AXP2101)
const HwPowerInfo& hwPower();
void hwPowerRefreshNow();
void hwPowerOff();

// Motion (QMI8658) & Clock (PCF85063)
bool hwImuOnline();
bool hwGetAccel(float& ax, float& ay, float& az);
bool hwRtcOnline();
void hwRtcSet(const HwDateTime& dt);
bool hwRtcGet(HwDateTime& dt);

// Speaker (ES8311 + NS4150B). Tóny jdou do fronty a přehrává je úloha na pozadí,
// takže volání nikdy neblokuje hlavní smyčku. freqHz = 0 je pauza.
// amplitude: 0..32767 (síla tónu), výchozí odpovídá střední hlasitosti.
void hwBeep(uint16_t freqHz, uint16_t durMs, uint16_t amplitude = 5500);
