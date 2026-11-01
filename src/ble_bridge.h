// Vychází z anthropics/claude-desktop-buddy (Copyright 2026 Anthropic, PBC),
// licence MIT, viz LICENSE a LICENSES/MIT-claude-desktop-buddy.txt.
#pragma once
#include <stdint.h>
#include <stddef.h>

// Nordic UART Service-compatible BLE bridge (compatible with Claude Desktop Hardware Buddy & AGY Bridge)
// Service UUID  6e400001-b5a3-f393-e0a9-e50e24dcca9e
// RX char       6e400002-b5a3-f393-e0a9-e50e24dcca9e   (desktop -> device, WRITE)
// TX char       6e400003-b5a3-f393-e0a9-e50e24dcca9e   (device -> desktop, NOTIFY)

void bleInit(const char* deviceName);
bool bleConnected();
bool bleSecure();
uint32_t blePasskey();
void bleClearBonds();
// Výsledek posledního párování: 0 = žádná novinka, 1 = úspěch, 2 = neúspěch (jednorázově)
uint8_t bleTakeAuthResult();
size_t bleAvailable();
int bleRead();
size_t bleWrite(const uint8_t* data, size_t len);
