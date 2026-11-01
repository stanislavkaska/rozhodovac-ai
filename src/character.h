#pragma once
#include <stdint.h>

struct Palette {
  uint16_t body, bg, text, textDim, ink;
};

// Official Claude Code Terracotta Coral-Orange (#D97757 -> RGB565 0xDBAA) on pure AMOLED black
inline const Palette& characterPalette() {
  static const Palette p = {
    0xDBAA, // body:    #D97757 Claude terracotta orange
    0x0000, // bg:      #000000 pure AMOLED black
    0xFFFF, // text:    #FFFFFF crisp white
    0xAD55, // textDim: #A8AAAD warm silver-gray
    0xF4CF  // ink:     #F09A7E light coral highlight
  };
  return p;
}
