// Vychází z anthropics/claude-desktop-buddy (Copyright 2026 Anthropic, PBC),
// licence MIT, viz LICENSE a LICENSES/MIT-claude-desktop-buddy.txt.
#pragma once
#include <Arduino.h>
#include "ble_bridge.h"
#include "hw_waveshare.h"
#include <ArduinoJson.h>
#include "stats.h"

// Odpověď jde jen kanálem, kterým přišel příkaz (0 = USB, 1 = BLE, jinak oběma).
// Most se ptá na stav každých 8 s a Claude Desktop na BLE ty odpovědi nepotřebuje.
static void _xWrite(uint8_t ch, const char* b, int len) {
  if (len <= 0) return;
  if (ch != 1) Serial.write(b, len);
  if (ch != 0) bleWrite((const uint8_t*)b, len);
}

static void _xAck(uint8_t ch, const char* what, bool ok, uint32_t n = 0) {
  char b[80];
  int len = snprintf(b, sizeof(b), "{\"ack\":\"%s\",\"ok\":%s,\"n\":%lu}\n", what, ok ? "true" : "false", (unsigned long)n);
  _xWrite(ch, b, len);
}

inline bool xferCommand(JsonDocument& doc, uint8_t ch) {
  const char* cmd = doc["cmd"];
  if (!cmd) return false;

  if (strcmp(cmd, "name") == 0) {
    const char* n = doc["name"];
    if (n) petNameSet(n);
    _xAck(ch, "name", n != nullptr);
    return true;
  }

  if (strcmp(cmd, "species") == 0) {
    _xAck(ch, "species", true);
    return true;
  }

  if (strcmp(cmd, "unpair") == 0) {
    bleClearBonds();
    _xAck(ch, "unpair", true);
    return true;
  }

  if (strcmp(cmd, "owner") == 0) {
    const char* n = doc["name"];
    if (n) ownerSet(n);
    _xAck(ch, "owner", n != nullptr);
    return true;
  }

  if (strcmp(cmd, "status") == 0) {
    const HwPowerInfo& pw = hwPower();
    int iBat = pw.charging ? -150 : 40;
    char b[480];
    int len = snprintf(b, sizeof(b),
      // "ui":2 = firmware umí obrazovku otázky (q, multi, descs, back, term) a řádek do 4 KB
      "{\"ack\":\"status\",\"ok\":true,\"n\":0,\"data\":{\"ui\":2,"
      "\"name\":\"%s\",\"owner\":\"%s\",\"sec\":%s,"
      "\"bat\":{\"pct\":%d,\"mV\":%d,\"mA\":%d,\"usb\":%s},"
      "\"sys\":{\"up\":%lu,\"heap\":%lu,\"psram\":%lu,\"fsFree\":%lu,\"fsTotal\":%lu},"
      "\"stats\":{\"appr\":%u,\"deny\":%u,\"answ\":%u,\"vel\":%u,\"nap\":%lu,\"lvl\":%u}"
      "}}\n",
      petName(), ownerName(), bleSecure() ? "true" : "false",
      pw.batteryPct, pw.batteryMv, iBat, pw.usbPresent ? "true" : "false",
      (unsigned long)(millis() / 1000),
      (unsigned long)ESP.getFreeHeap(),
      (unsigned long)ESP.getFreePsram(),
      10092544UL,
      10092544UL,
      stats().approvals, stats().denials, stats().answers, statsMedianVelocity(),
      (unsigned long)stats().napSeconds, stats().level
    );
    _xWrite(ch, b, len);
    return true;
  }

  // Jakýkoli příkaz (i ozvěna vlastního "permission") se spotřebuje: jako snímek
  // bez "prompt" by jinak smazal čekající dotaz
  return true;
}

inline bool xferActive()       { return false; }
inline uint32_t xferProgress() { return 0; }
inline uint32_t xferTotal()    { return 0; }
