#include "hw_waveshare.h"
#include <Wire.h>
#include <math.h>
#include <stdarg.h>
#include <ESP_I2S.h>
#include <esp_timer.h>

#include "XPowersLib.h"
#include "TouchDrvCSTXXX.hpp"
#include "SensorQMI8658.hpp"
#include "SensorPCF85063.hpp"
#include "es8311.h"

// ─── Display & PSRAM Framebuffer ───────────────────────────────────────────
static Arduino_DataBus* _bus = nullptr;
static Arduino_CO5300*  _gfx = nullptr;
static Arduino_Canvas*  _canvas = nullptr;
static bool             _dispOn = true;

// ─── Peripherals on Shared I2C (SDA=15, SCL=14) ────────────────────────────
static XPowersPMU       _pmu;
static TouchDrvCST92xx  _touch;
static SensorQMI8658    _imu;
static SensorPCF85063   _rtc;

static bool _pmuOk   = false;
static bool _touchOk = false;
static bool _imuOk   = false;
static bool _rtcOk   = false;
static bool _audioOk = false;

static HwButton    _btnBoot;
static HwButton    _btnUser;
static portMUX_TYPE _btnMux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t _btnTimer = nullptr;
static TouchEvent  _touchEvt;
static HwPowerInfo _pwrInfo;
static float       _lastAx = 0.0f, _lastAy = 0.0f, _lastAz = 1.0f;

static volatile bool _tpIrqPending = false;
static volatile bool _palmCovered  = false;
static bool          _pwrKeyShort  = false;

static void IRAM_ATTR _onTouchIrq() {
  _tpIrqPending = true;
}

static bool _takeTouchIrq() {
  noInterrupts();
  bool p = _tpIrqPending;
  _tpIrqPending = false;
  interrupts();
  return p;
}

// ─── C-linkage I2C helpers used by es8311.c ────────────────────────────────
extern "C" esp_err_t hwI2cWrite(uint8_t addr, const uint8_t* buf, size_t len) {
  Wire.beginTransmission(addr);
  Wire.write(buf, len);
  return (Wire.endTransmission() == 0) ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t hwI2cWriteRead(uint8_t addr, uint8_t reg, uint8_t* val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return ESP_FAIL;
  if (Wire.requestFrom((int)addr, 1) != 1) return ESP_FAIL;
  *val = (uint8_t)Wire.read();
  return ESP_OK;
}

// ─── Audio Init (ES8311 + ESP_I2S) ─────────────────────────────────────────
static const int AUDIO_SAMPLE_RATE = 16000;
static es8311_handle_t _esHandle = nullptr;
static I2SClass _i2s;

// Fronta tónů a úloha, která je přehrává na jádru 0 (Arduino loop běží na jádru 1)
struct ToneReq {
  uint16_t freq;   // Hz, 0 = pauza
  uint16_t durMs;
  uint16_t amp;
};
static QueueHandle_t _toneQ = nullptr;
static void _audioTask(void*);

// Úspora baterie: mezi melodiemi se I2S zastaví (bez hodin MCLK stojí i kodek
// ES8311 a DMA nebudí procesor). Při potížích se zvukem nastav na 0.
#ifndef AUDIO_IDLE_STOP_I2S
#define AUDIO_IDLE_STOP_I2S 1
#endif
static bool _i2sOn = false;

static bool _i2sStart() {
  if (_i2sOn) return true;
  _i2sOn = _i2s.begin(I2S_MODE_STD, AUDIO_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);
  return _i2sOn;
}

static void _i2sStop() {
  if (!_i2sOn) return;
  _i2s.end();
  _i2sOn = false;
}

static bool _initAudio() {
  pinMode(PA_ENABLE_PIN, OUTPUT);
  digitalWrite(PA_ENABLE_PIN, LOW); // keep amp muted except while chirping

  _i2s.setPins(I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN, -1, I2S_MCLK_PIN);
  // Kodek se nastavuje při běžících hodinách MCLK
  if (!_i2sStart()) {
    return false;
  }

  // Při chybě kodeku I2S zastavit, jinak by hodiny MCLK/BCLK běžely do vypnutí
  _esHandle = es8311_create(0, ES8311_ADDRRES_0);
  if (!_esHandle) { _i2sStop(); return false; }

  es8311_clock_config_t clk = {};
  clk.mclk_inverted = false;
  clk.sclk_inverted = false;
  clk.mclk_from_mclk_pin = true;
  clk.mclk_frequency = AUDIO_SAMPLE_RATE * 256;
  clk.sample_frequency = AUDIO_SAMPLE_RATE;

  if (es8311_init(_esHandle, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
    _i2sStop();
    return false;
  }
  es8311_sample_frequency_config(_esHandle, clk.mclk_frequency, clk.sample_frequency);
  es8311_microphone_config(_esHandle, false);
  es8311_voice_volume_set(_esHandle, 72, nullptr);

  _toneQ = xQueueCreate(32, sizeof(ToneReq));
  if (!_toneQ) { _i2sStop(); return false; }
  xTaskCreatePinnedToCore(_audioTask, "audio", 4096, nullptr, 2, nullptr, 0);
#if AUDIO_IDLE_STOP_I2S
  _i2sStop();
#endif
  return true;
}

// ─── Power Telemetry ───────────────────────────────────────────────────────
// Náhradní odhad nabití z napětí článku Li-Pol (když PMU procenta nedá):
// vybíjecí křivka je v okolí 3,7–3,8 V plochá, lineární odhad by lhal
static int _pctFromMv(int mv) {
  static const int16_t MV[]  = { 3300, 3500, 3600, 3700, 3750, 3800, 3850, 3900, 4000, 4100, 4200 };
  static const int8_t  PCT[] = {    0,    5,   10,   25,   35,   45,   55,   65,   80,   92,  100 };
  const int n = sizeof(MV) / sizeof(MV[0]);
  if (mv <= MV[0]) return 0;
  if (mv >= MV[n - 1]) return 100;
  for (int i = 1; i < n; i++) {
    if (mv <= MV[i]) {
      return PCT[i - 1] + (PCT[i] - PCT[i - 1]) * (mv - MV[i - 1]) / (MV[i] - MV[i - 1]);
    }
  }
  return 100;
}

void hwPowerRefreshNow() {
  if (!_pmuOk) {
    _pwrInfo.pmuOnline = false;
    _pwrInfo.usbPresent = true;
    _pwrInfo.batteryPct = 100;
    return;
  }
  _pwrInfo.pmuOnline = true;
  _pwrInfo.usbPresent = _pmu.isVbusIn();
  _pwrInfo.vbusMv = _pwrInfo.usbPresent ? (int)_pmu.getVbusVoltage() : 0;
  _pwrInfo.batteryConnected = _pmu.isBatteryConnect();
  _pwrInfo.batteryMv = _pwrInfo.batteryConnected ? (int)_pmu.getBattVoltage() : 0;
  if (_pwrInfo.batteryConnected) {
    int p = _pmu.getBatteryPercent();
    if (p < 0) {
      p = _pctFromMv(_pwrInfo.batteryMv);
    }
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    _pwrInfo.batteryPct = p;
  } else {
    _pwrInfo.batteryPct = 100;
  }
  _pwrInfo.charging = _pmu.isCharging();
  _pwrInfo.chargeDone = (_pmu.getChargerStatus() == XPOWERS_AXP2101_CHG_DONE_STATE);
  _pwrInfo.tempC = (int)_pmu.getTemperature();
}

// ─── Main Hardware Initialization ──────────────────────────────────────────
bool hwInit() {
  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setClock(400000);

  // 1. AXP2101 PMU
  _pmuOk = _pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (_pmuOk) {
    // Enable A3V3 audio rail (ALDO1 = 3.3V) and peripheral rails
    _pmu.setALDO1Voltage(3300);
    _pmu.enableALDO1();
    _pmu.setALDO2Voltage(3300);
    _pmu.enableALDO2();

    _pmu.enableTemperatureMeasure();
    _pmu.enableBattDetection();
    _pmu.enableVbusVoltageMeasure();
    _pmu.enableBattVoltageMeasure();
    _pmu.enableSystemVoltageMeasure();

    _pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    _pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
    _pmu.clearIrqStatus();
    _pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
    _pmu.enableGauge();   // procenta baterie z měřidla PMU (výchozí stav registru nespoléhat)
    hwPowerRefreshNow();
    Serial.println("[hw] AXP2101 PMU online");
  } else {
    Serial.println("[hw] WARN: AXP2101 PMU not detected");
  }

  // 2. Physical top buttons
  _btnBoot.init(BTN_BOOT_PIN);
  _btnUser.init(BTN_USER_PIN);
  {
    esp_timer_create_args_t ta = {};
    ta.callback = [](void*) { _btnBoot.sample(); _btnUser.sample(); };
    ta.name = "btn";
    if (esp_timer_create(&ta, &_btnTimer) == ESP_OK) {
      esp_timer_start_periodic(_btnTimer, 5000);
    } else {
      _btnTimer = nullptr;
      Serial.println("[hw] WARN: button timer failed");
    }
  }

  // 3. CO5300 480x480 QSPI AMOLED + 480x480 PSRAM Canvas
  _bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
  _gfx = new Arduino_CO5300(
    _bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);
  _canvas = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, _gfx);

  if (!_canvas->begin()) {
    Serial.println("[hw] ERROR: Canvas/CO5300 begin failed!");
    return false;
  }
  // Waveshare CO5300 panel MADCTL orientation
  _bus->writeC8D8(0x36, 0xA0);
  _gfx->setBrightness(200);
  _canvas->fillScreen(0x0000);
  _canvas->flush();
  Serial.printf("[hw] AMOLED 480x480 ready (PSRAM free: %lu KB)\n", (unsigned long)(ESP.getFreePsram() / 1024));

  // 4. CST9220 Capacitive Touch
  _touch.setPins(TP_RST, TP_INT);
  _touchOk = _touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (_touchOk) {
    _touch.setCoverScreenCallback([](void*) {
      _palmCovered = true;
    }, nullptr);
    _touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    _touch.setSwapXY(true);
    _touch.setMirrorXY(true, false);
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TP_INT), _onTouchIrq, FALLING);
    Serial.printf("[hw] Touch online: %s\n", _touch.getModelName());
  } else {
    Serial.println("[hw] WARN: CST9220 touch not detected");
  }

  // 5. QMI8658 6-axis IMU (address 0x6B)
  _imuOk = _imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (_imuOk) {
    _imu.configAccelerometer(
      SensorQMI8658::ACC_RANGE_4G,
      // Stačí na zatřesení a otočení displejem dolů (čte se 25× za sekundu)
      SensorQMI8658::ACC_ODR_62_5Hz,
      SensorQMI8658::LPF_MODE_0);
    _imu.enableAccelerometer();
    Serial.println("[hw] QMI8658 IMU online");
  } else {
    Serial.println("[hw] WARN: QMI8658 IMU not detected");
  }

  // 6. PCF85063 Hardware RTC
  _rtcOk = _rtc.begin(Wire, IIC_SDA, IIC_SCL);
  if (_rtcOk) {
    Serial.println("[hw] PCF85063 RTC online");
  } else {
    Serial.println("[hw] WARN: PCF85063 RTC not detected");
  }

  // 7. ES8311 Speaker Codec
  _audioOk = _initAudio();
  Serial.printf("[hw] ES8311 audio: %s\n", _audioOk ? "online" : "disabled");

  return true;
}

// ─── Touch Gesture State Machine ───────────────────────────────────────────
static bool     _wasDown = false;
static int16_t  _downX = 0, _downY = 0;
static int16_t  _lastX = 0, _lastY = 0;
static uint32_t _downMs = 0;
static uint32_t _lastPollMs = 0;
static bool     _longFired = false;
static uint8_t  _emptyReads = 0;   // puštění prstu až po dvou prázdných čteních za sebou

static void _pollTouch() {
  _touchEvt.type = TOUCH_NONE;
  if (!_touchOk) return;

  uint32_t now = millis();
  bool shouldRead = _takeTouchIrq() || (_wasDown && (now - _lastPollMs >= 35));
  if (!shouldRead) {
    if (_wasDown && !_longFired && (now - _downMs >= 650)) {
      int dx = abs(_lastX - _downX);
      int dy = abs(_lastY - _downY);
      if (dx < 28 && dy < 28) {
        _longFired = true;
        _touchEvt.type = TOUCH_LONG_PRESS;
        _touchEvt.x = _downX;
        _touchEvt.y = _downY;
      }
    }
    return;
  }

  _lastPollMs = now;
  int16_t tx[2] = {0}, ty[2] = {0};
  uint8_t n = _touch.getPoint(tx, ty, 2);

  if (n > 0) {
    _emptyReads = 0;
    _lastX = tx[0];
    _lastY = ty[0];
    if (!_wasDown) {
      _wasDown = true;
      _downX = _lastX;
      _downY = _lastY;
      _downMs = now;
      _longFired = false;
    }
    _touchEvt.touching = true;
    _touchEvt.curX = _lastX;
    _touchEvt.curY = _lastY;

    if (!_longFired && (now - _downMs >= 650)) {
      int dx = abs(_lastX - _downX);
      int dy = abs(_lastY - _downY);
      if (dx < 28 && dy < 28) {
        _longFired = true;
        _touchEvt.type = TOUCH_LONG_PRESS;
        _touchEvt.x = _downX;
        _touchEvt.y = _downY;
      }
    }
  } else {
    // getPoint vrací 0 i při chybě I2C: jedno prázdné čtení během držení
    // není puštění (jinak falešné klepnutí a přerušené podržení)
    if (_wasDown && ++_emptyReads < 2) return;
    _emptyReads = 0;
    if (_wasDown) {
      _wasDown = false;
      _touchEvt.touching = false;
      if (!_longFired) {
        int dx = _lastX - _downX;
        int dy = _lastY - _downY;
        int adx = abs(dx), ady = abs(dy);
        if (adx > 64 && adx > ady) {
          _touchEvt.type = (dx < 0) ? TOUCH_SWIPE_LEFT : TOUCH_SWIPE_RIGHT;
          _touchEvt.x = _downX;
          _touchEvt.y = _downY;
        } else if (ady > 64 && ady > adx) {
          _touchEvt.type = (dy < 0) ? TOUCH_SWIPE_UP : TOUCH_SWIPE_DOWN;
          _touchEvt.x = _downX;
          _touchEvt.y = _downY;
        } else if ((now - _downMs) < 700) {
          _touchEvt.type = TOUCH_TAP;
          _touchEvt.x = _lastX;
          _touchEvt.y = _lastY;
        }
      }
    }
  }
}

void hwUpdate() {
  _btnBoot.update();
  _btnUser.update();
  _pollTouch();

  uint32_t now = millis();

  // Poll AXP2101 short power button every 60ms & battery every 1000ms
  static uint32_t lastPmuIrqMs = 0;
  static uint32_t lastPmuBatMs = 0;
  if (_pmuOk && (now - lastPmuIrqMs >= 60)) {
    lastPmuIrqMs = now;
    // Registry přerušení se čtou přímo: knihovna by chybu I2C (sběrnici sdílí
    // dotyk) uložila jako 0xFF a ta by vypadala jako stisk PWR. Při chybě se
    // nic nevyhodnocuje ani nemaže. Maže se jen přečtené (zápis 1 maže), takže
    // stisk, který přijde mezi čtením a mazáním, zůstane pro další průchod.
    int s1 = _pmu.readRegister(XPOWERS_AXP2101_INTSTS1);
    int s2 = _pmu.readRegister(XPOWERS_AXP2101_INTSTS2);
    int s3 = _pmu.readRegister(XPOWERS_AXP2101_INTSTS3);
    if (s1 >= 0 && s2 >= 0 && s3 >= 0) {
      if (s2 & (uint8_t)(XPOWERS_AXP2101_PKEY_SHORT_IRQ >> 8)) _pwrKeyShort = true;
      if (s1) _pmu.writeRegister(XPOWERS_AXP2101_INTSTS1, (uint8_t)s1);
      if (s2) _pmu.writeRegister(XPOWERS_AXP2101_INTSTS2, (uint8_t)s2);
      if (s3) _pmu.writeRegister(XPOWERS_AXP2101_INTSTS3, (uint8_t)s3);
    }
  }
  if (_pmuOk && (now - lastPmuBatMs >= 1000)) {
    lastPmuBatMs = now;
    hwPowerRefreshNow();
  }

  // Update cached IMU reading at ~25Hz
  static uint32_t lastImuMs = 0;
  if (_imuOk && (now - lastImuMs >= 40)) {
    lastImuMs = now;
    if (_imu.getDataReady()) {
      float x, y, z;
      if (_imu.getAccelerometer(x, y, z)) {
        _lastAx = x; _lastAy = y; _lastAz = z;
      }
    }
  }
}

Arduino_Canvas* hwCanvas()  { return _canvas; }
Arduino_CO5300* hwDisplay() { return _gfx; }

void hwFlush() {
  if (_canvas && _dispOn) _canvas->flush();
}

void hwSetBrightness(uint8_t val) {
  if (_gfx) _gfx->setBrightness(val);
}

void hwSetDisplayPower(bool on) {
  if (!_gfx || _dispOn == on) return;
  _dispOn = on;
  if (on) _gfx->displayOn();
  else    _gfx->displayOff();
}

// ─── UTF-8 Czech Diacritics & Pixel Typography Renderer ────────────────────
uint32_t hwNextUtf8(const char*& p) {
  if (!p || !*p) return 0;
  uint8_t b0 = (uint8_t)*p++;
  if (b0 < 0x80) return b0;
  if ((b0 & 0xE0) == 0xC0) {
    uint8_t b1 = (uint8_t)*p;
    if ((b1 & 0xC0) != 0x80) return '?';
    p++;
    uint32_t cp = ((uint32_t)(b0 & 0x1F) << 6) | (b1 & 0x3F);
    return cp < 0x80 ? '?' : cp;   // přeplněný zápis (např. C0 80) by jinak ukončil text
  }
  if ((b0 & 0xF0) == 0xE0) {
    uint8_t b1 = (uint8_t)*p;
    if ((b1 & 0xC0) != 0x80) return '?';
    p++;
    uint8_t b2 = (uint8_t)*p;
    if ((b2 & 0xC0) != 0x80) return '?';
    p++;
    uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(b1 & 0x3F) << 6) | (b2 & 0x3F);
    return cp < 0x800 ? '?' : cp;
  }
  if ((b0 & 0xF8) == 0xF0) {
    uint32_t cp = (uint32_t)(b0 & 0x07) << 18;
    for (int s = 12; s >= 0; s -= 6) {
      uint8_t bx = (uint8_t)*p;
      if ((bx & 0xC0) != 0x80) return '?';
      p++;
      cp |= (uint32_t)(bx & 0x3F) << s;
    }
    return cp < 0x10000 ? '?' : cp;
  }
  return '?';
}

size_t hwUtf8Len(const char* text) {
  if (!text) return 0;
  size_t n = 0;
  const char* p = text;
  while (hwNextUtf8(p) != 0) n++;
  return n;
}

static void _drawCols5x7(Arduino_GFX* g, int x, int y, const uint8_t cols[5], uint8_t u, uint16_t fg, uint16_t bg) {
  if (fg != bg) {
    g->fillRect(x, y, 6 * u, 8 * u, bg);
  }
  for (int c = 0; c < 5; c++) {
    uint8_t bits = cols[c];
    if (!bits) continue;
    for (int r = 0; r < 8; r++) {
      if (bits & (1 << r)) {
        g->fillRect(x + c * u, y + r * u, u, u, fg);
      }
    }
  }
}

// Čárka (´) anchored above botY (lowest pixel at botY - 1)
static void _accAcute(Arduino_GFX* g, int x, int botY, uint8_t u, uint16_t fg) {
  if (u == 1) {
    g->fillRect(x + 2, botY - 1, 1, 1, fg);
    g->fillRect(x + 3, botY - 2, 1, 1, fg);
  } else {
    int h = u / 2;
    g->fillRect(x + 2 * u,     botY - u,     u, u, fg);
    g->fillRect(x + 2 * u + h, botY - u - h, u, u, fg);
    g->fillRect(x + 3 * u,     botY - 2 * u, u, u, fg);
  }
}

// Háček (ˇ) anchored above botY
static void _accCaron(Arduino_GFX* g, int x, int botY, uint8_t u, uint16_t fg) {
  if (u == 1) {
    g->fillRect(x + 1, botY - 2, 1, 1, fg);
    g->fillRect(x + 3, botY - 2, 1, 1, fg);
    g->fillRect(x + 2, botY - 1, 1, 1, fg);
  } else {
    int h = u / 2;
    g->fillRect(x + 2 * u,     botY - (u - h), u,     u - h, fg);
    g->fillRect(x + 2 * u - h, botY - u,       2 * u, h,     fg);
    g->fillRect(x + 1 * u,     botY - 2 * u,   u,     u,     fg);
    g->fillRect(x + 3 * u,     botY - 2 * u,   u,     u,     fg);
  }
}

// Kroužek (°) with chamfered corners and hollow center anchored above botY
static void _accRing(Arduino_GFX* g, int x, int botY, uint8_t u, uint16_t fg) {
  if (u == 1) {
    g->drawRect(x + 1, botY - 3, 3, 3, fg);
  } else {
    int s = (u + 1) / 2;
    g->fillRect(x + 2 * u,     botY - s,         u, s, fg);
    g->fillRect(x + 2 * u - s, botY - s - u,     s, u, fg);
    g->fillRect(x + 3 * u,     botY - s - u,     s, u, fg);
    g->fillRect(x + 2 * u,     botY - 2 * s - u, u, s, fg);
  }
}

// Přehláska (¨) anchored above botY
static void _accUmlaut(Arduino_GFX* g, int x, int botY, uint8_t u, uint16_t fg) {
  g->fillRect(x + 1 * u, botY - u, u, u, fg);
  g->fillRect(x + 3 * u, botY - u, u, u, fg);
}

// Vokáň (^) anchored above botY
static void _accCirc(Arduino_GFX* g, int x, int botY, uint8_t u, uint16_t fg) {
  if (u == 1) {
    g->fillRect(x + 2, botY - 2, 1, 1, fg);
    g->fillRect(x + 1, botY - 1, 1, 1, fg);
    g->fillRect(x + 3, botY - 1, 1, 1, fg);
  } else {
    int h = u / 2;
    g->fillRect(x + 2 * u,     botY - 2 * u,   u,     u - h, fg);
    g->fillRect(x + 2 * u - h, botY - u - h,   2 * u, h,     fg);
    g->fillRect(x + 1 * u,     botY - u,       u,     u,     fg);
    g->fillRect(x + 3 * u,     botY - u,       u,     u,     fg);
  }
}

// Česká klička pro malá 'ď', 'ť', 'ľ' vpravo nahoře od dříku
static void _accKlicka(Arduino_GFX* g, int x, int y, int col, uint8_t u, uint16_t fg) {
  if (u == 1) {
    g->fillRect(x + col, y - 1, 1, 2, fg);
  } else {
    int h = u / 2;
    int s = (u + 1) / 2;
    if (col >= 4) {
      g->fillRect(x + 4 * u,     y - h, u, u + h, fg);
      g->fillRect(x + 3 * u + h, y + u, u, s,     fg);
    } else {
      g->fillRect(x + 3 * u + h, y - h, u, u + h, fg);
      g->fillRect(x + 3 * u,     y + u, u, s,     fg);
    }
  }
}

static void _drawGlyphUtf8(Arduino_GFX* g, int x, int y, uint32_t cp, uint8_t u, uint16_t fg, uint16_t bg) {
  if (cp >= 0x20 && cp <= 0x7E) {
    g->drawChar(x, y, (unsigned char)cp, fg, bg);
    return;
  }

  int gap   = (u + 1) / 2;
  int loBot = y + 2 * u - gap; // lowercase body starts at y + 2*u
  int upBot = y - gap;         // uppercase body starts at y

  switch (cp) {
    // ── Malá písmena s čárkou ──
    case 0x00E1: g->drawChar(x, y, 'a', fg, bg); _accAcute(g, x, loBot, u, fg); return; // á
    case 0x00E9: g->drawChar(x, y, 'e', fg, bg); _accAcute(g, x, loBot, u, fg); return; // é
    case 0x00ED: {                                                                            // í (bez tečky!)
      static const uint8_t DOTLESS_I[5] = { 0x00, 0x44, 0x7C, 0x40, 0x00 };
      _drawCols5x7(g, x, y, DOTLESS_I, u, fg, bg);
      _accAcute(g, x, loBot, u, fg);
      return;
    }
    case 0x00F3: g->drawChar(x, y, 'o', fg, bg); _accAcute(g, x, loBot, u, fg); return; // ó
    case 0x00FA: g->drawChar(x, y, 'u', fg, bg); _accAcute(g, x, loBot, u, fg); return; // ú
    case 0x00FD: g->drawChar(x, y, 'y', fg, bg); _accAcute(g, x, loBot, u, fg); return; // ý
    case 0x013A: g->drawChar(x, y, 'l', fg, bg); _accAcute(g, x, upBot, u, fg); return; // ĺ
    case 0x0155: g->drawChar(x, y, 'r', fg, bg); _accAcute(g, x, loBot, u, fg); return; // ŕ

    // ── Malá písmena s háčkem ──
    case 0x010D: g->drawChar(x, y, 'c', fg, bg); _accCaron(g, x, loBot, u, fg); return; // č
    case 0x011B: g->drawChar(x, y, 'e', fg, bg); _accCaron(g, x, loBot, u, fg); return; // ě
    case 0x0148: g->drawChar(x, y, 'n', fg, bg); _accCaron(g, x, loBot, u, fg); return; // ň
    case 0x0159: g->drawChar(x, y, 'r', fg, bg); _accCaron(g, x, loBot, u, fg); return; // ř
    case 0x0161: g->drawChar(x, y, 's', fg, bg); _accCaron(g, x, loBot, u, fg); return; // š
    case 0x017E: g->drawChar(x, y, 'z', fg, bg); _accCaron(g, x, loBot, u, fg); return; // ž

    // ── Malá písmena s českou kličkou (ď, ť, ľ) ──
    case 0x010F: { // ď
      static const uint8_t SLIM_D[5] = { 0x38, 0x44, 0x7F, 0x00, 0x00 };
      _drawCols5x7(g, x, y, SLIM_D, u, fg, bg);
      _accKlicka(g, x, y, 4, u, fg);
      return;
    }
    case 0x0165: { // ť
      static const uint8_t SLIM_T[5] = { 0x04, 0x3F, 0x44, 0x20, 0x00 };
      _drawCols5x7(g, x, y, SLIM_T, u, fg, bg);
      _accKlicka(g, x, y, 3, u, fg);
      return;
    }
    case 0x013E: { // ľ
      static const uint8_t SLIM_L[5] = { 0x00, 0x41, 0x7F, 0x40, 0x00 };
      _drawCols5x7(g, x, y, SLIM_L, u, fg, bg);
      _accKlicka(g, x, y, 4, u, fg);
      return;
    }

    // ── Malé ů s kroužkem + přehlásky / vokáň ──
    case 0x016F: g->drawChar(x, y, 'u', fg, bg); _accRing(g, x, loBot, u, fg);   return; // ů
    case 0x00E4: g->drawChar(x, y, 'a', fg, bg); _accUmlaut(g, x, loBot, u, fg); return; // ä
    case 0x00F6: g->drawChar(x, y, 'o', fg, bg); _accUmlaut(g, x, loBot, u, fg); return; // ö
    case 0x00FC: g->drawChar(x, y, 'u', fg, bg); _accUmlaut(g, x, loBot, u, fg); return; // ü
    case 0x00F4: g->drawChar(x, y, 'o', fg, bg); _accCirc(g, x, loBot, u, fg);   return; // ô

    // ── Velká písmena s čárkou ──
    case 0x00C1: g->drawChar(x, y, 'A', fg, bg); _accAcute(g, x, upBot, u, fg); return; // Á
    case 0x00C9: g->drawChar(x, y, 'E', fg, bg); _accAcute(g, x, upBot, u, fg); return; // É
    case 0x00CD: g->drawChar(x, y, 'I', fg, bg); _accAcute(g, x, upBot, u, fg); return; // Í
    case 0x00D3: g->drawChar(x, y, 'O', fg, bg); _accAcute(g, x, upBot, u, fg); return; // Ó
    case 0x00DA: g->drawChar(x, y, 'U', fg, bg); _accAcute(g, x, upBot, u, fg); return; // Ú
    case 0x00DD: g->drawChar(x, y, 'Y', fg, bg); _accAcute(g, x, upBot, u, fg); return; // Ý
    case 0x0139: g->drawChar(x, y, 'L', fg, bg); _accAcute(g, x, upBot, u, fg); return; // Ĺ
    case 0x0154: g->drawChar(x, y, 'R', fg, bg); _accAcute(g, x, upBot, u, fg); return; // Ŕ

    // ── Velká písmena s háčkem ──
    case 0x010C: g->drawChar(x, y, 'C', fg, bg); _accCaron(g, x, upBot, u, fg); return; // Č
    case 0x010E: g->drawChar(x, y, 'D', fg, bg); _accCaron(g, x, upBot, u, fg); return; // Ď
    case 0x011A: g->drawChar(x, y, 'E', fg, bg); _accCaron(g, x, upBot, u, fg); return; // Ě
    case 0x0147: g->drawChar(x, y, 'N', fg, bg); _accCaron(g, x, upBot, u, fg); return; // Ň
    case 0x0158: g->drawChar(x, y, 'R', fg, bg); _accCaron(g, x, upBot, u, fg); return; // Ř
    case 0x0160: g->drawChar(x, y, 'S', fg, bg); _accCaron(g, x, upBot, u, fg); return; // Š
    case 0x0164: g->drawChar(x, y, 'T', fg, bg); _accCaron(g, x, upBot, u, fg); return; // Ť
    case 0x017D: g->drawChar(x, y, 'Z', fg, bg); _accCaron(g, x, upBot, u, fg); return; // Ž
    case 0x013D: g->drawChar(x, y, 'L', fg, bg); _accCaron(g, x, upBot, u, fg); return; // Ľ

    // ── Velké Ů s kroužkem + přehlásky / vokáň ──
    case 0x016E: g->drawChar(x, y, 'U', fg, bg); _accRing(g, x, upBot, u, fg);   return; // Ů
    case 0x00C4: g->drawChar(x, y, 'A', fg, bg); _accUmlaut(g, x, upBot, u, fg); return; // Ä
    case 0x00D6: g->drawChar(x, y, 'O', fg, bg); _accUmlaut(g, x, upBot, u, fg); return; // Ö
    case 0x00DC: g->drawChar(x, y, 'U', fg, bg); _accUmlaut(g, x, upBot, u, fg); return; // Ü
    case 0x00D4: g->drawChar(x, y, 'O', fg, bg); _accCirc(g, x, upBot, u, fg);   return; // Ô

    // ── Užitečné typografické symboly a interpunkce ──
    case 0x00A0: g->drawChar(x, y, ' ', fg, bg); return; // pevná mezera
    case 0x00B0:                                               // ° (stupeň)
      if (fg != bg) g->fillRect(x, y, 6 * u, 8 * u, bg);
      _accRing(g, x, y + 3 * u, u, fg);
      return;
    case 0x2013: case 0x2014: case 0x2212:                     // – — −
      g->drawChar(x, y, '-', fg, bg);
      return;
    case 0x2018: case 0x2019: case 0x201A:                     // ‘ ’ ‚
      g->drawChar(x, y, '\'', fg, bg);
      return;
    case 0x201C: case 0x201D: case 0x201E:                     // “ ” „
      g->drawChar(x, y, '"', fg, bg);
      return;
    case 0x2026: {                                             // …
      static const uint8_t ELLIPSIS[5] = { 0x40, 0x00, 0x40, 0x00, 0x40 };
      _drawCols5x7(g, x, y, ELLIPSIS, u, fg, bg);
      return;
    }
    case 0x2022: case 0x00B7: {                                // • ·
      static const uint8_t BULLET[5] = { 0x00, 0x18, 0x18, 0x00, 0x00 };
      _drawCols5x7(g, x, y, BULLET, u, fg, bg);
      return;
    }
    case 0x2192: {                                             // →
      static const uint8_t ARR_R[5] = { 0x08, 0x08, 0x2A, 0x1C, 0x08 };
      _drawCols5x7(g, x, y, ARR_R, u, fg, bg);
      return;
    }
    case 0x2190: {                                             // ←
      static const uint8_t ARR_L[5] = { 0x08, 0x1C, 0x2A, 0x08, 0x08 };
      _drawCols5x7(g, x, y, ARR_L, u, fg, bg);
      return;
    }
    case 0x2191: {                                             // ↑
      static const uint8_t ARR_U[5] = { 0x04, 0x02, 0x7F, 0x02, 0x04 };
      _drawCols5x7(g, x, y, ARR_U, u, fg, bg);
      return;
    }
    case 0x2193: {                                             // ↓
      static const uint8_t ARR_D[5] = { 0x10, 0x20, 0x7F, 0x20, 0x10 };
      _drawCols5x7(g, x, y, ARR_D, u, fg, bg);
      return;
    }
    case 0x2713: case 0x2714: {                                // ✓ ✔
      static const uint8_t CHECK[5] = { 0x10, 0x20, 0x10, 0x08, 0x04 };
      _drawCols5x7(g, x, y, CHECK, u, fg, bg);
      return;
    }
    case 0x00D7: case 0x2715: case 0x2716:                     // × ✕ ✖
      g->drawChar(x, y, 'x', fg, bg);
      return;

    default:
      g->drawChar(x, y, (cp < 0x20) ? ' ' : '?', fg, bg);
      return;
  }
}

int hwDrawUtf8(Arduino_GFX* g, const char* text, int x, int y, uint8_t size,
               uint16_t fg, uint16_t bg, size_t maxChars) {
  if (!g || !text) return x;
  uint8_t u = (size == 0) ? 1 : size;
  g->setTextSize(u);
  int cx = x;
  size_t drawn = 0;
  const char* p = text;
  while (*p && drawn < maxChars) {
    uint32_t cp = hwNextUtf8(p);
    if (!cp) break;
    _drawGlyphUtf8(g, cx, y, cp, u, fg, bg);
    cx += 6 * u;
    drawn++;
  }
  return cx;
}

int hwPrintfUtf8(Arduino_GFX* g, int x, int y, uint8_t size,
                 uint16_t fg, uint16_t bg, const char* fmt, ...) {
  if (!g || !fmt) return x;
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return hwDrawUtf8(g, buf, x, y, size, fg, bg);
}

void hwDrawTextCentered(Arduino_GFX* g, const char* text, int cx, int y, uint8_t size, uint16_t fg, uint16_t bg) {
  if (!g || !text) return;
  uint8_t u = (size == 0) ? 1 : size;
  int len = (int)hwUtf8Len(text);
  int w = len * 6 * u;
  hwDrawUtf8(g, text, cx - w / 2, y, u, fg, bg);
}

void hwDrawTextRight(Arduino_GFX* g, const char* text, int rightX, int y, uint8_t size, uint16_t fg, uint16_t bg) {
  if (!g || !text) return;
  uint8_t u = (size == 0) ? 1 : size;
  int len = (int)hwUtf8Len(text);
  int w = len * 6 * u;
  hwDrawUtf8(g, text, rightX - w, y, u, fg, bg);
}

const TouchEvent& hwTouch() { return _touchEvt; }

bool hwTakePalmCover() {
  noInterrupts();
  bool c = _palmCovered;
  _palmCovered = false;
  interrupts();
  return c;
}

void HwButton::init(uint8_t gpioPin) {
  pin = gpioPin;
  pinMode(pin, INPUT_PULLUP);
  rawLast = stable = cur = (digitalRead(pin) == LOW);
  rawChangeMs = stableSinceMs = pressStartMs = millis();
}

void HwButton::sample() {
  bool r = (digitalRead(pin) == LOW);
  uint32_t now = millis();
  portENTER_CRITICAL(&_btnMux);
  if (r != rawLast) {
    rawLast = r;
    rawChangeMs = now;
  } else if (r != stable && (now - rawChangeMs) >= 15) {
    stable = r;
    stableSinceMs = now;
    if (r) pressCnt++;
    else   releaseCnt++;
  }
  portEXIT_CRITICAL(&_btnMux);
}

void HwButton::update() {
  // Bez časovače (nepovedl se založit) se vzorkuje aspoň tady
  if (!_btnTimer) sample();
  portENTER_CRITICAL(&_btnMux);
  cur = stable;
  pressStartMs = stableSinceMs;
  pressedEdge = (pressCnt != seenPress);
  releasedEdge = (releaseCnt != seenRelease);
  seenPress = pressCnt;
  seenRelease = releaseCnt;
  portEXIT_CRITICAL(&_btnMux);
}

HwButton& hwBtnBoot() { return _btnBoot; }
HwButton& hwBtnUser() { return _btnUser; }

bool hwTakePowerKeyShortPress() {
  bool v = _pwrKeyShort;
  _pwrKeyShort = false;
  return v;
}

const HwPowerInfo& hwPower() { return _pwrInfo; }

void hwPowerOff() {
  hwSetDisplayPower(false);
  if (_pmuOk) {
    delay(80);
    _pmu.shutdown();
  }
}

bool hwImuOnline() { return _imuOk; }

bool hwGetAccel(float& ax, float& ay, float& az) {
  if (!_imuOk) { ax = 0.0f; ay = 0.0f; az = 1.0f; return false; }
  ax = _lastAx; ay = _lastAy; az = _lastAz;
  return true;
}

bool hwRtcOnline() { return _rtcOk; }

void hwRtcSet(const HwDateTime& dt) {
  if (!_rtcOk) return;
  _rtc.setDateTime(dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
}

bool hwRtcGet(HwDateTime& dt) {
  if (!_rtcOk) return false;
  RTC_DateTime r = _rtc.getDateTime();
  dt.year   = r.getYear();
  dt.month  = r.getMonth();
  dt.day    = r.getDay();
  dt.hour   = r.getHour();
  dt.minute = r.getMinute();
  dt.second = r.getSecond();
  // Sakamoto day-of-week (0=Sun..6=Sat)
  static const uint8_t t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  uint16_t y = dt.year;
  uint8_t  m = dt.month;
  if (m < 1 || m > 12) m = 1;
  if (m < 3) y -= 1;
  dt.wday = (y + y/4 - y/100 + y/400 + t[m - 1] + dt.day) % 7;
  return true;
}

// Vygeneruje jeden tón (nebo ticho) do I2S; blokuje jen audio úlohu
static void _playTone(const ToneReq& t) {
  int totalSamples = (AUDIO_SAMPLE_RATE * (int)t.durMs) / 1000;
  int16_t frameBuf[128 * 2]; // stereo interleaved
  float phase = 0.0f;
  float step = (2.0f * 3.14159265f * (float)t.freq) / (float)AUDIO_SAMPLE_RATE;
  float amp = (float)t.amp;

  int writtenSamples = 0;
  while (writtenSamples < totalSamples) {
    int chunk = totalSamples - writtenSamples;
    if (chunk > 128) chunk = 128;
    for (int i = 0; i < chunk; i++) {
      int16_t s = 0;
      if (t.freq) {
        // Soft envelope at start/end to avoid speaker pop
        int idx = writtenSamples + i;
        float env = 1.0f;
        if (idx < 64) env = (float)idx / 64.0f;
        else if (totalSamples - idx < 64) env = (float)(totalSamples - idx) / 64.0f;
        s = (int16_t)(sinf(phase) * amp * env);
        phase += step;
        if (phase > 6.2831853f) phase -= 6.2831853f;
      }
      frameBuf[i * 2]     = s;
      frameBuf[i * 2 + 1] = s;
    }
    _i2s.write((const uint8_t*)frameBuf, chunk * 4);
    writtenSamples += chunk;
  }
}

static void _audioTask(void*) {
  ToneReq t;
  int16_t silence[128 * 2];
  memset(silence, 0, sizeof(silence));
  for (;;) {
    // Úloha spí, dokud nepřijde tón (žádná zátěž procesoru)
    if (xQueueReceive(_toneQ, &t, portMAX_DELAY) != pdTRUE) continue;
    if (!_i2sStart()) continue;
    // Krátké ticho po rozběhu hodin, teprve pak zesilovač (bez lupnutí)
    _i2s.write((const uint8_t*)silence, sizeof(silence));
    // Zesilovač zapnutý po celou melodii, ne zvlášť pro každý tón
    digitalWrite(PA_ENABLE_PIN, HIGH);
    do {
      _playTone(t);
    } while (xQueueReceive(_toneQ, &t, pdMS_TO_TICKS(25)) == pdTRUE);
    // write() se vrací po zkopírování do DMA, ne po přehrání. Tichem (~130 ms)
    // se celý DMA buffer dohraje, teprve pak se vypne zesilovač a hodiny;
    // jinak by se uřízl konec melodie.
    for (int i = 0; i < 16; i++) _i2s.write((const uint8_t*)silence, sizeof(silence));
    digitalWrite(PA_ENABLE_PIN, LOW);
#if AUDIO_IDLE_STOP_I2S
    _i2sStop();
#endif
  }
}

void hwBeep(uint16_t freqHz, uint16_t durMs, uint16_t amplitude) {
  if (!_audioOk || !_toneQ || durMs == 0) return;
  if (freqHz != 0 && freqHz < 80) return;
  if (durMs > 600) durMs = 600;
  ToneReq t = { freqHz, durMs, amplitude };
  xQueueSend(_toneQ, &t, 0);   // plná fronta = tón se zahodí, smyčka nečeká
}
