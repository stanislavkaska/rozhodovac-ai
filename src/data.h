// Vychází z anthropics/claude-desktop-buddy (Copyright 2026 Anthropic, PBC),
// licence MIT, viz LICENSE a LICENSES/MIT-claude-desktop-buddy.txt.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>
#include "ble_bridge.h"
#include "hw_waveshare.h"
#include "xfer.h"

// Nejvyšší počet voleb otázky, který krabička zobrazí (hook a daemon mají stejný limit)
#define PROMPT_MAX_OPTS 8

struct TamaState {
  uint8_t  sessionsTotal;
  uint8_t  sessionsRunning;
  uint8_t  sessionsWaiting;
  bool     recentlyCompleted;
  uint32_t tokensToday;
  uint32_t lastUpdated;
  char     msg[128];
  bool     connected;
  char     lines[16][160];
  uint8_t  nLines;
  uint16_t lineGen;
  char     promptId[48];
  char     promptTool[64];
  char     promptHint[320];
  char     promptOptA[48];
  char     promptOptB[48];
  char     promptOpts[PROMPT_MAX_OPTS][96];   // popisek volby, na obrazovce otázky až 2 řádky
  uint8_t  promptOptCount;
  // Obrazovka otázky ("q":1, most pro Claude Code): volby pod sebou s popisem,
  // vícenásobný výběr, tlačítka ZPĚT / TERMINÁL / POTVRDIT, posouvání voleb
  bool     promptQ;
  bool     promptMulti;   // vícenásobný výběr: klepnutí volbu zaškrtne, odešle POTVRDIT
  bool     promptBack;    // tlačítko ZPĚT (předchozí otázka)
  bool     promptTerm;    // tlačítko TERMINÁL (odpovědět v terminálu)
  bool     promptCont;    // další krok téže otázky: bez zvuku nového dotazu
  uint8_t  promptSelInit; // předvybrané volby (bitová maska), např. po návratu ZPĚT
  char     promptHead[64];    // záhlaví karty, např. "2/3 · Knihovna"
  char     promptOk[24];      // popisek tlačítka POTVRDIT
  char     promptDescs[PROMPT_MAX_OPTS][192];   // popis volby (tlumeně pod popiskem)
  int32_t  promptTtl;   // zbývající sekundy do vypršení dotazu podle mostu, -1 = neznámé (Claude)
  int32_t  promptAge;   // jak dlouho už dotaz čeká v sekundách, -1 = neznámé
  uint32_t promptRxMs;  // millis() při posledním snímku s dotazem (k přepočtu ttl/age)
  bool     promptDanger; // riziková akce: příznak "danger" od mostu, u Claude Desktopu vzor v příkazu
  uint8_t  promptCh;     // kanál, ze kterého dotaz přišel (CH_USB / CH_BLE), odpověď jde jen tam
  bool     agyClient;   // snapshot poslal bridge pro AGY ("src":"agy"), jinak Claude
};

// Kanály, kterými chodí data: USB sériová linka a Bluetooth LE
enum DataChannel : uint8_t { CH_USB = 0, CH_BLE = 1, CH_NONE = 0xFF };

static uint32_t _lastLiveMs = 0;
static bool     _demoMode   = false;
static bool     _demoEnded  = false;   // po vypnutí dema se převezme skutečný stav

// Klienta drží jeden kanál (primární). Když běží most na USB (AGY nebo Claude
// Code v terminálu) a zároveň Claude Desktop na BLE, snímky druhého kanálu se
// ignorují, jinak by se maskot přepínal s každým snímkem a snímek bez dotazu
// by smazal dotaz toho druhého. Primární kanál se přepne, když druhý přinese
// dotaz a primární žádný nemá, když na druhém agent pracuje a na primárním ne
// (Claude Desktop posílá keepalive každých 10 s, jeho kanál by jinak nikdy
// neztichl a práce v terminálu by nebyla vidět), nebo když primární 20 s nic
// nepošle.
static uint8_t  _primaryCh = CH_NONE;
static uint32_t _chSnapMs[2] = { 0, 0 };
static const uint32_t CH_SILENT_MS = 20000;

// Potvrzení odpovědi od mostu ({"ack":"permission","id":...}); čte main.cpp
static char     _permAckId[48] = "";
static bool     _permAckOk = false;
static bool     _permAckNew = false;
static uint8_t  _permAckCh = CH_NONE;   // kanál, kterým potvrzení přišlo

inline bool dataTakePermissionAck(char* idOut, size_t idLen, bool& ok, uint8_t& ch) {
  if (!_permAckNew) return false;
  _permAckNew = false;
  strncpy(idOut, _permAckId, idLen - 1);
  idOut[idLen - 1] = 0;
  ok = _permAckOk;
  ch = _permAckCh;
  return true;
}
static uint8_t  _demoIdx    = 0;
static uint32_t _demoNext   = 0;

struct _Fake {
  const char* n;
  uint8_t t, r, w;
  bool c;
  uint32_t tok;
  const char* tool;
  const char* hint;
};

static const _Fake _FAKES[] = {
  {"klid (1 relace)",    1, 0, 0, false, 12400,  "", ""},
  {"pracuje (3 relace)", 3, 3, 0, false, 89200,  "", ""},
  {"čeká na schválení",  2, 1, 1, false, 45600,  "run_command", "Nahrát firmware: pio run -t upload --upload-port COM8"},
  {"dokončeno!",         1, 0, 0, true,  142000, "", ""},
  {"spánek",             0, 0, 0, false, 0,      "", ""},
};

// Skutečný stav klienta. V demo režimu se data čtou dál sem, aby se neztratil
// dotaz ani se nezahltily buffery USB a BLE. Po vypnutí dema se převezme.
static TamaState _real = {};

inline void dataSetDemo(bool on, const TamaState* cur) {
  if (on && !_demoMode && cur) _real = *cur;
  if (!on && _demoMode) _demoEnded = true;
  _demoMode = on;
  if (on) { _demoIdx = 0; _demoNext = millis() + 8000; }
}
inline bool dataDemo() { return _demoMode; }

inline void dataAdvanceDemo() {
  if (_demoMode) {
    _demoIdx = (_demoIdx + 1) % 5;
    _demoNext = millis() + 10000;
  }
}

inline bool dataConnected() {
  return _lastLiveMs != 0 && (millis() - _lastLiveMs) <= 30000;
}

inline const char* dataScenarioName() {
  if (_demoMode) return "DEMO";
  if (dataConnected()) return _primaryCh == CH_BLE ? "BLE" : "USB";
  return "OFFLINE";
}

static bool _rtcValid = false;
inline bool dataRtcValid() { return _rtcValid; }
// Hodiny RTC jsou zálohované baterií: smysluplný čas po zapnutí se bere jako platný
inline void dataMarkRtcValid() { _rtcValid = true; }

// strncpy, který neusekne vícebajtový znak UTF-8 (jinak by se na konci ukázal "?")
static void _copyUtf8(char* dst, const char* src, size_t size) {
  if (!src) src = "";
  size_t n = strlen(src);
  if (n > size - 1) {
    n = size - 1;
    while (n > 0 && (((unsigned char)src[n]) & 0xC0) == 0x80) n--;
  }
  memcpy(dst, src, n);
  dst[n] = 0;
}

static void _clearPrompt(TamaState* out) {
  out->promptId[0] = 0;
  out->promptTool[0] = 0;
  out->promptHint[0] = 0;
  out->promptOptA[0] = 0;
  out->promptOptB[0] = 0;
  out->promptOptCount = 0;
  out->promptTtl = -1;
  out->promptAge = -1;
  out->promptDanger = false;
  out->promptCh = CH_NONE;
  out->promptQ = out->promptMulti = out->promptBack = out->promptTerm = out->promptCont = false;
  out->promptSelInit = 0;
  out->promptHead[0] = 0;
  out->promptOk[0] = 0;
  for (int i = 0; i < PROMPT_MAX_OPTS; i++) {
    out->promptOpts[i][0] = 0;
    out->promptDescs[i][0] = 0;
  }
}

// Vzory rizikových příkazů pro Claude Desktop, který rizikové akce neoznačuje
// (most je označí sám příznakem "danger"). Hledá se v celém příkazu, ne jen
// v části, která se vejde do bufferu. Vrací pozici prvního nálezu, -1 = nic.
static int _dangerPos(const char* s) {
  static const char* const PATTERNS[] = {
    "rm -rf", "rm -fr", "rm -r ", "rm --recursive", "push --force", "push -f", "push --delete",
    "push --mirror", "reset --hard", "clean -f", "clean --force", "branch --delete --force",
    "drop table", "drop database", "drop schema", "truncate table", "remove-item",
    "rd /s", "rmdir /s", "del /s", "del /q", "format-volume", "clear-disk", "diskpart",
    "mkfs", "dd if=", "shutdown", "restart-computer"
  };
  if (!s || !s[0]) return -1;
  int best = -1;
  for (const char* pat : PATTERNS) {
    size_t pl = strlen(pat);
    for (const char* p = s; *p; p++) {
      size_t j = 0;
      while (j < pl && p[j] && (char)tolower((unsigned char)p[j]) == pat[j]) j++;
      if (j == pl) {
        int pos = (int)(p - s);
        if (best < 0 || pos < best) best = pos;
        break;
      }
    }
  }
  return best;
}

static void _applyJson(const char* line, TamaState* out, uint8_t ch) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;

  // Potvrzení odeslané odpovědi od mostu
  const char* ack = doc["ack"];
  if (ack) {
    if (strcmp(ack, "permission") == 0) {
      const char* id = doc["id"];
      strncpy(_permAckId, id ? id : "", sizeof(_permAckId) - 1);
      _permAckId[sizeof(_permAckId) - 1] = 0;
      _permAckOk = doc["ok"] | false;
      _permAckCh = ch;
      _permAckNew = true;
    }
    return;
  }

  if (xferCommand(doc, ch)) { _lastLiveMs = millis(); return; }

  JsonArray t = doc["time"];
  if (!t.isNull() && t.size() == 2) {
    time_t local = (time_t)t[0].as<uint32_t>() + (int32_t)t[1];
    struct tm lt;
    gmtime_r(&local, &lt);
    HwDateTime dt;
    dt.year   = (uint16_t)(lt.tm_year + 1900);
    dt.month  = (uint8_t)(lt.tm_mon + 1);
    dt.day    = (uint8_t)lt.tm_mday;
    dt.wday   = (uint8_t)lt.tm_wday;
    dt.hour   = (uint8_t)lt.tm_hour;
    dt.minute = (uint8_t)lt.tm_min;
    dt.second = (uint8_t)lt.tm_sec;
    hwRtcSet(dt);
    extern uint32_t _clkLastRead;
    _clkLastRead = 0;
    _rtcValid = true;
    _lastLiveMs = millis();
    return;
  }

  // Turn events from Claude Desktop can be ignored for TamaState snapshot fields
  if (doc["evt"].is<const char*>()) {
    _lastLiveMs = millis();
    return;
  }

  // Snímek stavu musí mít aspoň jedno pole stavu. Úlomek řádku po zahozeném
  // paketu BLE (třeba jen vnořený objekt dotazu) by jinak prošel jako snímek
  // bez dotazu a čekající dotaz smazal.
  if (doc["total"].isNull() && doc["running"].isNull() && doc["waiting"].isNull()
      && doc["msg"].isNull() && doc["entries"].isNull() && doc["prompt"].isNull()) return;

  // Výběr primárního kanálu (viz _primaryCh)
  uint32_t nowMs = millis();
  _chSnapMs[ch] = nowMs;
  if (_primaryCh != ch) {
    bool primarySilent = _primaryCh == CH_NONE || (nowMs - _chSnapMs[_primaryCh]) > CH_SILENT_MS;
    bool otherPrompt = !doc["prompt"].isNull() && !out->promptId[0];
    bool otherBusy = (doc["running"] | 0) > 0 && out->sessionsRunning == 0 && !out->promptId[0];
    if (!(primarySilent || otherPrompt || otherBusy)) return;
    // Každý kanál má vlastní kumulativní počítadlo tokenů (Claude Desktop i most
    // posílají "tokens" od svého startu), rozdíl mezi nimi se nesmí přičíst
    if (_primaryCh != CH_NONE) statsResetTokenSync();
    _primaryCh = ch;
  }

  // Most pro AGY značí své zprávy polem "src"; Claude Desktop ani most pro
  // Claude Code v terminálu ho neposílají (maskot Clawd)
  const char* src = doc["src"];
  bool agy = src && strcmp(src, "agy") == 0;
  // Po výpadku snímků nebo změně klienta má kumulativní počítadlo tokenů jiný základ
  static uint32_t lastSnapMs = 0;
  if (agy != out->agyClient || lastSnapMs == 0 || (nowMs - lastSnapMs) > 30000) statsResetTokenSync();
  lastSnapMs = nowMs;
  out->agyClient = agy;

  out->sessionsTotal     = doc["total"]     | out->sessionsTotal;
  out->sessionsRunning   = doc["running"]   | out->sessionsRunning;
  out->sessionsWaiting   = doc["waiting"]   | out->sessionsWaiting;
  out->recentlyCompleted = doc["completed"] | false;
  uint32_t bridgeTokens  = doc["tokens"]    | 0;
  if (doc["tokens"].is<uint32_t>()) statsOnBridgeTokens(bridgeTokens);
  out->tokensToday = doc["tokens_today"] | out->tokensToday;

  const char* m = doc["msg"];
  if (m) _copyUtf8(out->msg, m, sizeof(out->msg));

  // Nová zpráva = změnil se počet řádků nebo nejnovější řádek (porovnává se
  // s minulým snímkem, ne se stavovou hláškou msg, ta se od historie liší
  // skoro vždy a displej by se budil s každým heartbeatem daemonu)
  JsonArray la = doc["entries"];
  if (!la.isNull()) {
    uint8_t n = 0;
    bool changed = false;
    for (JsonVariant v : la) {
      if (n >= 16) break;
      const char* s = v.as<const char*>();
      if (!s) s = "";
      // Zkrácení podle celých znaků UTF-8 (na konci řádku jinak svítil „?“)
      char row[sizeof(out->lines[0])];
      _copyUtf8(row, s, sizeof(row));
      if (n == 0 && strcmp(out->lines[0], row) != 0) changed = true;
      memcpy(out->lines[n], row, sizeof(row));
      n++;
    }
    if (n != out->nLines || changed) {
      out->lineGen++;
    }
    out->nLines = n;
  }

  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char* pid = pr["id"];
    const char* pt  = pr["tool"];
    const char* ph  = pr["hint"];
    const char* oa  = pr["optA"];
    const char* ob  = pr["optB"];
    _copyUtf8(out->promptId,   pid ? pid : "", sizeof(out->promptId));
    _copyUtf8(out->promptTool, pt  ? pt  : "", sizeof(out->promptTool));
    _copyUtf8(out->promptHint, ph  ? ph  : "", sizeof(out->promptHint));
    // Most (AGY i Claude Code v terminálu) posílá zbývající čas a stáří dotazu
    // a rizikovou akci označí sám, Claude Desktop nic z toho
    bool fromBridge = pr["ttl"].is<int>();
    out->promptTtl  = fromBridge ? pr["ttl"].as<int32_t>() : -1;
    out->promptAge  = pr["age"].is<int>() ? pr["age"].as<int32_t>() : -1;
    out->promptRxMs = nowMs;
    out->promptCh   = ch;
    int dangerAt = fromBridge ? -1 : _dangerPos(ph);
    out->promptDanger = (pr["danger"] | false) || dangerAt >= 0;
    // Riziková část dlouhého příkazu by byla za koncem karty (ta ukáže asi
    // 130 znaků): karta pak ukáže výřez od kousku před ní
    if (dangerAt > 90) {
      const char* from = ph + dangerAt - 30;
      while (from > ph && (((unsigned char)*from) & 0xC0) == 0x80) from--;
      memcpy(out->promptHint, "…", 3);
      _copyUtf8(out->promptHint + 3, from, sizeof(out->promptHint) - 3);
    }

    JsonArray opts = pr["options"];
    if (!opts.isNull() && opts.size() > 0) {
      uint8_t count = 0;
      for (JsonVariant v : opts) {
        if (count >= PROMPT_MAX_OPTS) break;
        const char* s = v.as<const char*>();
        _copyUtf8(out->promptOpts[count], s ? s : "", sizeof(out->promptOpts[count]));
        count++;
      }
      out->promptOptCount = count;
      _copyUtf8(out->promptOptA, count > 0 ? out->promptOpts[0] : "", sizeof(out->promptOptA));
      _copyUtf8(out->promptOptB, count > 1 ? out->promptOpts[1] : "", sizeof(out->promptOptB));
    } else {
      _copyUtf8(out->promptOptA, oa ? oa : "", sizeof(out->promptOptA));
      _copyUtf8(out->promptOptB, ob ? ob : "", sizeof(out->promptOptB));
      if (out->promptOptA[0] && out->promptOptB[0]) {
        out->promptOptCount = 2;
        _copyUtf8(out->promptOpts[0], out->promptOptA, sizeof(out->promptOpts[0]));
        _copyUtf8(out->promptOpts[1], out->promptOptB, sizeof(out->promptOpts[1]));
      } else {
        out->promptOptCount = 0;
      }
    }

    // Obrazovka otázky (most pro Claude Code). Popisy voleb jdou v poli "descs"
    // ve stejném pořadí jako "options", předvybrané volby v "sel" (pořadí od 0).
    out->promptQ     = pr["q"] | false;
    out->promptMulti = pr["multi"] | false;
    out->promptBack  = pr["back"] | false;
    out->promptTerm  = pr["term"] | false;
    out->promptCont  = pr["cont"] | false;
    _copyUtf8(out->promptHead, pr["head"] | "", sizeof(out->promptHead));
    _copyUtf8(out->promptOk, pr["ok"] | "", sizeof(out->promptOk));
    JsonArray descs = pr["descs"];
    for (int i = 0; i < PROMPT_MAX_OPTS; i++) {
      const char* d = (!descs.isNull() && i < (int)descs.size()) ? descs[i].as<const char*>() : nullptr;
      _copyUtf8(out->promptDescs[i], d ? d : "", sizeof(out->promptDescs[i]));
    }
    out->promptSelInit = 0;
    JsonArray sel = pr["sel"];
    if (!sel.isNull()) {
      for (JsonVariant v : sel) {
        int i = v | -1;
        if (i >= 0 && i < PROMPT_MAX_OPTS) out->promptSelInit |= (uint8_t)(1u << i);
      }
    }
  } else {
    _clearPrompt(out);
  }

  out->lastUpdated = millis();
  _lastLiveMs = millis();
}

// Řádkový buffer pro JSON zprávy. Řádek delší než buffer se celý zahodí:
// useknutý JSON by stejně neprošel parserem a navíc by se ztratil i prompt.
template<size_t N>
struct _LineBuf {
  char buf[N];
  uint16_t len = 0;
  bool overflow = false;

  void push(char c, TamaState* out, uint8_t ch) {
    if (c == '\n' || c == '\r') {
      if (len > 0 && !overflow) {
        buf[len] = 0;
        if (buf[0] == '{') _applyJson(buf, out, ch);
      }
      len = 0;
      overflow = false;
    } else if (len < N - 1) {
      buf[len++] = c;
    } else {
      overflow = true;
    }
  }
};

static _LineBuf<4096> _usbLine, _btLine;

// Stav ukázky: jen žádost o schválení, otázky demo nemá
static void _demoFill(TamaState* out, uint32_t now) {
  const _Fake& f = _FAKES[_demoIdx];
  out->sessionsTotal = f.t;
  out->sessionsRunning = f.r;
  out->sessionsWaiting = f.w;
  out->recentlyCompleted = f.c;
  out->tokensToday = f.tok;
  out->lastUpdated = now;
  out->connected = true;
  out->agyClient = false;
  snprintf(out->msg, sizeof(out->msg), "Ukázka: %s", f.n);
  _clearPrompt(out);
  if (f.tool[0]) {
    snprintf(out->promptId, sizeof(out->promptId), "demo_%u", _demoIdx);
    strncpy(out->promptTool, f.tool, sizeof(out->promptTool) - 1);
    out->promptTool[sizeof(out->promptTool) - 1] = 0;
    strncpy(out->promptHint, f.hint, sizeof(out->promptHint) - 1);
    out->promptHint[sizeof(out->promptHint) - 1] = 0;
  }
}

// Vrací true, když demo režim právě skončil kvůli skutečnému dotazu
inline bool dataPoll(TamaState* out) {
  uint32_t now = millis();
  bool demoInterrupted = false;

  // Demo vypnuté z menu: skutečný stav se převezme dřív, než se čtou nová
  // data, jinak by je hned přepsal
  if (_demoEnded && !_demoMode) {
    _demoEnded = false;
    *out = _real;
  }

  // Data se čtou vždy, v demu do stínového stavu _real
  TamaState* dst = _demoMode ? &_real : out;
  while (Serial.available()) {
    _usbLine.push((char)Serial.read(), dst, CH_USB);
  }
  while (bleAvailable()) {
    int c = bleRead();
    if (c < 0) break;
    _btLine.push((char)c, dst, CH_BLE);
  }

  if (_demoMode) {
    if (_real.promptId[0] && dataConnected()) {
      // Skutečný dotaz má přednost před ukázkou
      _demoMode = false;
      _demoEnded = true;
      demoInterrupted = true;
    } else {
      if ((int32_t)(now - _demoNext) >= 0) {
        _demoIdx = (_demoIdx + 1) % 5;
        _demoNext = now + 9000;
      }
      _demoFill(out, now);
      return false;
    }
  }

  if (_demoEnded) {
    _demoEnded = false;
    *out = _real;
  }

  out->connected = dataConnected();
  if (!out->connected) {
    _primaryCh = CH_NONE;
    out->sessionsTotal = 0;
    out->sessionsRunning = 0;
    out->sessionsWaiting = 0;
    out->recentlyCompleted = false;
    out->promptId[0] = 0;
    out->agyClient = false;
    out->lastUpdated = now;
    strncpy(out->msg, "Čekám na Claude / AGY (BLE nebo USB)…", sizeof(out->msg) - 1);
    out->msg[sizeof(out->msg) - 1] = 0;
  }
  return demoInterrupted;
}
