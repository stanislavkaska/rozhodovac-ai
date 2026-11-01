// Vychází z anthropics/claude-desktop-buddy (Copyright 2026 Anthropic, PBC),
// licence MIT, viz LICENSE a LICENSES/MIT-claude-desktop-buddy.txt.
#pragma once
#include <Arduino.h>
#include <Preferences.h>

// Level podle tokenů: první level stojí 50K, každý další o polovinu víc.
// Skutečná spotřeba Claude jde do milionů denně, s pevnými 50K by level
// stoupal pořád a melodie by se ozývala každou chvíli.
static const uint32_t TOKENS_FIRST_LEVEL = 50000;
static const uint16_t LEVEL_MAX = 999;

struct Stats {
  uint32_t napSeconds;
  uint16_t approvals;   // schválené akce
  uint16_t denials;     // zamítnuté akce
  uint16_t answers;     // odpovědi na otázky s volbami (nejsou to schválení ani zamítnutí)
  uint16_t velocity[8]; // doby posledních 8 schválení a odpovědí v sekundách
  uint8_t  velIdx;
  uint8_t  velCount;
  uint8_t  decBits;     // posledních 8 schválení a zamítnutí, bit 1 = zamítnutí (nejnovější v bitu 0)
  uint8_t  decCount;
  uint16_t level;
  uint64_t tokens;
};

// Level a rozpracovaný level pro daný počet tokenů
struct LevelInfo { uint16_t level; uint64_t into; uint64_t need; };

inline LevelInfo levelForTokens(uint64_t t) {
  LevelInfo li = { 0, t, TOKENS_FIRST_LEVEL };
  while (li.into >= li.need && li.level < LEVEL_MAX) {
    li.into -= li.need;
    li.level++;
    li.need = li.need * 3 / 2;
  }
  return li;
}

static Stats _stats;
static Preferences _prefs;
static bool _dirty = false;
static uint32_t _dirtySinceMs = 0;   // tokeny bez jiné události se ukládají nejpozději po 5 minutách

inline void statsLoad() {
  _prefs.begin("buddy", true);
  _stats.napSeconds = _prefs.getUInt("nap", 0);
  _stats.approvals  = _prefs.getUShort("appr", 0);
  _stats.denials    = _prefs.getUShort("deny", 0);
  _stats.answers    = _prefs.getUShort("answ", 0);
  _stats.velIdx     = _prefs.getUChar("vidx", 0);
  _stats.velCount   = _prefs.getUChar("vcnt", 0);
  _stats.decBits    = _prefs.getUChar("dbit", 0);
  _stats.decCount   = _prefs.getUChar("dcnt", 0);
  // Tokeny jsou od verze s rostoucí křivkou 64bitové, starší uložení je 32bitové
  if (_prefs.isKey("tok64")) _stats.tokens = _prefs.getULong64("tok64", 0);
  else                       _stats.tokens = _prefs.getUInt("tok", 0);
  size_t got = _prefs.getBytes("vel", _stats.velocity, sizeof(_stats.velocity));
  if (got != sizeof(_stats.velocity)) memset(_stats.velocity, 0, sizeof(_stats.velocity));
  _prefs.end();
  // Poškozené hodnoty z flash nesmí ukazovat mimo pole velocity[8]
  if (_stats.velIdx >= 8)  _stats.velIdx = 0;
  if (_stats.velCount > 8) _stats.velCount = 8;
  if (_stats.decCount > 8) _stats.decCount = 8;
  _stats.level = levelForTokens(_stats.tokens).level;
}

inline void statsSave() {
  if (!_dirty) return;
  _prefs.begin("buddy", false);
  _prefs.putUInt("nap", _stats.napSeconds);
  _prefs.putUShort("appr", _stats.approvals);
  _prefs.putUShort("deny", _stats.denials);
  _prefs.putUShort("answ", _stats.answers);
  _prefs.putUChar("vidx", _stats.velIdx);
  _prefs.putUChar("vcnt", _stats.velCount);
  _prefs.putUChar("dbit", _stats.decBits);
  _prefs.putUChar("dcnt", _stats.decCount);
  _prefs.putULong64("tok64", _stats.tokens);
  _prefs.putBytes("vel", _stats.velocity, sizeof(_stats.velocity));
  _prefs.end();
  _dirty = false;
}

static void _statsRecordVelocity(uint32_t secondsToRespond) {
  _stats.velocity[_stats.velIdx] = (secondsToRespond > 65535UL) ? (uint16_t)65535 : (uint16_t)secondsToRespond;
  _stats.velIdx = (_stats.velIdx + 1) % 8;
  if (_stats.velCount < 8) _stats.velCount++;
}

// Výsledek rozhodnutí do okna posledních 8 (odpovědi na otázky sem nepatří,
// druhá volba otázky není zamítnutí)
static void _statsRecordDecision(bool denied) {
  _stats.decBits = (uint8_t)((_stats.decBits << 1) | (denied ? 1 : 0));
  if (_stats.decCount < 8) _stats.decCount++;
}

inline void statsOnApproval(uint32_t secondsToRespond) {
  _stats.approvals++;
  _statsRecordVelocity(secondsToRespond);
  _statsRecordDecision(false);
  _dirty = true;
  statsSave();
}

// Odpověď na otázku: počítá se zvlášť, do nálady jde jen rychlost odpovědi
inline void statsOnAnswer(uint32_t secondsToRespond) {
  _stats.answers++;
  _statsRecordVelocity(secondsToRespond);
  _dirty = true;
  statsSave();
}

static uint32_t _lastBridgeTokens = 0;
static bool _tokensSynced = false;
static bool _levelUpPending = false;

// Kumulativní počítadlo tokenů má každý klient své (AGY odhad, Claude skutečnost).
// Po změně klienta nebo po výpadku spojení se proto vezme nový výchozí stav,
// jinak by se přičetl celý rozdíl mezi počítadly a level by poskočil o desítky.
inline void statsResetTokenSync() { _tokensSynced = false; }

inline void statsOnBridgeTokens(uint32_t bridgeTotal) {
  if (!_tokensSynced) {
    _lastBridgeTokens = bridgeTotal;
    _tokensSynced = true;
    return;
  }
  if (bridgeTotal < _lastBridgeTokens) {
    _lastBridgeTokens = bridgeTotal;
    return;
  }
  uint32_t delta = bridgeTotal - _lastBridgeTokens;
  _lastBridgeTokens = bridgeTotal;
  if (delta == 0) return;

  _stats.tokens += delta;
  if (!_dirty) _dirtySinceMs = millis();
  _dirty = true;
  uint16_t lvlAfter = levelForTokens(_stats.tokens).level;
  if (lvlAfter > _stats.level) {
    _stats.level = lvlAfter;
    _levelUpPending = true;
    statsSave();
  }
}

// Volá hlavní smyčka: neuložené tokeny zapíše nejpozději po 5 minutách
inline void statsTick() {
  if (_dirty && (millis() - _dirtySinceMs) >= 300000UL) statsSave();
}

inline bool statsPollLevelUp() {
  bool r = _levelUpPending;
  _levelUpPending = false;
  return r;
}

inline void statsOnDenial() {
  _stats.denials++;
  _statsRecordDecision(true);
  _dirty = true;
  statsSave();
}
inline void statsMarkDirty() { _dirty = true; }

inline void statsOnNapEnd(uint32_t seconds) {
  _stats.napSeconds += seconds;
  _dirty = true;
  statsSave();
}

inline uint16_t statsMedianVelocity() {
  if (_stats.velCount == 0) return 0;
  uint16_t tmp[8];
  memcpy(tmp, _stats.velocity, sizeof(tmp));
  uint8_t n = _stats.velCount;
  for (uint8_t i = 1; i < n; i++) {
    uint16_t k = tmp[i]; int8_t j = i - 1;
    while (j >= 0 && tmp[j] > k) { tmp[j+1] = tmp[j]; j--; }
    tmp[j+1] = k;
  }
  return tmp[n / 2];
}

inline uint8_t statsMoodTier() {
  uint16_t vel = statsMedianVelocity();
  int8_t tier;
  if (_stats.velCount == 0) tier = 3;   // zatím žádné rozhodnutí
  else if (vel < 15) tier = 4;
  else if (vel < 30) tier = 3;
  else if (vel < 60) tier = 2;
  else if (vel < 120) tier = 1;
  else tier = 0;
  // Převaha zamítnutí z posledních 8 schválení a zamítnutí, ne z celé historie
  uint8_t n = _stats.decCount;
  uint8_t mask = (n >= 8) ? 0xFF : (uint8_t)((1u << n) - 1);
  uint8_t d = (uint8_t)__builtin_popcount(_stats.decBits & mask);
  uint8_t a = n - d;
  if (n >= 3) {
    if (d > a) tier -= 2;
    else if (d * 2 > a) tier -= 1;
  }
  if (tier < 0) tier = 0;
  return (uint8_t)tier;
}

static uint32_t _lastNapEndMs = 0;
static uint8_t  _energyAtNap  = 4;

inline void statsOnWake() { _lastNapEndMs = millis(); _energyAtNap = 5; }

inline uint8_t statsEnergyTier() {
  uint32_t hoursSince = (millis() - _lastNapEndMs) / 3600000;
  int8_t e = (int8_t)_energyAtNap - (int8_t)(hoursSince / 2);
  if (e < 0) e = 0;
  if (e > 5) e = 5;
  return (uint8_t)e;
}

inline uint8_t statsFedProgress() {
  LevelInfo li = levelForTokens(_stats.tokens);
  if (li.need == 0) return 0;
  return (uint8_t)(li.into * 10 / li.need);
}

struct Settings {
  bool sound;
  bool bt;
  bool wifi;
  bool led;
  bool hud;
  uint8_t clockRot;  // 0=auto, 1=portrait, 2=landscape
  uint8_t volume;    // 0 = tichý, 1 = střední, 2 = hlasitý
  bool clicks;       // cvakání při ovládání
  bool nightQuiet;   // noční klid 22–7 h podle RTC
  uint8_t brightness; // 0..4, výchozí 3 (3/4 jasu šetří baterii)
};

static Settings _settings = { true, true, false, true, true, 0, 1, true, true, 3 };

inline void settingsLoad() {
  _prefs.begin("buddy", true);
  _settings.sound    = _prefs.getBool("s_snd", true);
  _settings.bt       = _prefs.getBool("s_bt",  true);
  _settings.wifi     = _prefs.getBool("s_wifi",false);
  _settings.led      = _prefs.getBool("s_led", true);
  _settings.hud      = _prefs.getBool("s_hud", true);
  _settings.clockRot = _prefs.getUChar("s_crot", 0);
  if (_settings.clockRot > 2) _settings.clockRot = 0;
  _settings.volume     = _prefs.getUChar("s_vol", 1);
  if (_settings.volume > 2) _settings.volume = 1;
  _settings.clicks     = _prefs.getBool("s_clk", true);
  _settings.nightQuiet = _prefs.getBool("s_night", true);
  _settings.brightness = _prefs.getUChar("s_bri", 3);
  if (_settings.brightness > 4) _settings.brightness = 3;
  _prefs.end();
}

inline void settingsSave() {
  _prefs.begin("buddy", false);
  _prefs.putBool("s_snd",  _settings.sound);
  _prefs.putBool("s_bt",   _settings.bt);
  _prefs.putBool("s_wifi", _settings.wifi);
  _prefs.putBool("s_led",  _settings.led);
  _prefs.putBool("s_hud",  _settings.hud);
  _prefs.putUChar("s_crot", _settings.clockRot);
  _prefs.putUChar("s_vol",   _settings.volume);
  _prefs.putBool("s_clk",    _settings.clicks);
  _prefs.putBool("s_night",  _settings.nightQuiet);
  _prefs.putUChar("s_bri",   _settings.brightness);
  _prefs.end();
}

static char _petName[24] = "Rozhodovac";
static char _ownerName[32] = "";

inline void petNameLoad() {
  _prefs.begin("buddy", true);
  _prefs.getString("petname", _petName, sizeof(_petName));
  _prefs.getString("owner", _ownerName, sizeof(_ownerName));
  _prefs.end();
}

// Kopíruje jméno bez uvozovek, zpětných lomítek a řídicích znaků (jde dál do JSONu
// ve statusu). Bajty UTF-8 nad 0x7F zůstávají, takže čeština projde; useknutý
// vícebajtový znak na konci bufferu se zahodí celý.
static void _safeCopy(char* dst, size_t dstLen, const char* src) {
  size_t j = 0;
  for (size_t i = 0; src[i] && j < dstLen - 1; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c != '"' && c != '\\' && c >= 0x20 && c != 0x7F) dst[j++] = (char)c;
  }
  size_t lead = j;
  while (lead > 0 && (((unsigned char)dst[lead - 1]) & 0xC0) == 0x80) lead--;
  if (lead > 0) {
    unsigned char b = (unsigned char)dst[lead - 1];
    size_t need = (b >= 0xF0) ? 4 : (b >= 0xE0) ? 3 : (b >= 0xC0) ? 2 : 1;
    if ((lead - 1) + need > j) j = lead - 1;
  }
  dst[j] = 0;
}

inline void petNameSet(const char* name) {
  _safeCopy(_petName, sizeof(_petName), name);
  _prefs.begin("buddy", false);
  _prefs.putString("petname", _petName);
  _prefs.end();
}

inline const char* petName() { return _petName; }

inline void ownerSet(const char* name) {
  _safeCopy(_ownerName, sizeof(_ownerName), name);
  _prefs.begin("buddy", false);
  _prefs.putString("owner", _ownerName);
  _prefs.end();
}

inline const char* ownerName() { return _ownerName; }

inline uint8_t speciesIdxLoad() {
  _prefs.begin("buddy", true);
  uint8_t v = _prefs.getUChar("species", 0xFF);
  _prefs.end();
  return v;
}

inline void speciesIdxSave(uint8_t idx) {
  _prefs.begin("buddy", false);
  _prefs.putUChar("species", idx);
  _prefs.end();
}

inline Settings& settings() { return _settings; }
inline const Stats& stats() { return _stats; }
