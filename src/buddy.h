#pragma once
#include <stdint.h>

enum ClawdSizeMode : uint8_t {
  CLAWD_FULL   = 0, // Home screen (256x130 px in y=52..274)
  CLAWD_PROMPT = 1, // Approval prompt (192x95 px in y=52..214)
  CLAWD_PEEK   = 2  // Compact header above Info / Pet / Clock cards (144x70 px in y=52..156)
};

// Maskot podle připojeného klienta: Clawd pro Claude, Agy pro Antigravity CLI
enum BuddyCharacter : uint8_t {
  CHAR_CLAWD = 0,
  CHAR_AGY   = 1
};

void buddyInit();
void buddyInvalidate();
void buddySetMode(ClawdSizeMode mode);
void buddySetCharacter(BuddyCharacter ch);
void buddyTick(uint8_t personaState);
const char* buddySpeciesName();
