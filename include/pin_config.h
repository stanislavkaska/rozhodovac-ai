#pragma once

// Waveshare ESP32-S3-Touch-AMOLED-2.16 hardware pinout (verified against schematic Rev2)

#ifndef XPOWERS_CHIP_AXP2101
#define XPOWERS_CHIP_AXP2101
#endif

// QSPI AMOLED Display (CO5300, 480x480)
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_SCLK    38
#define LCD_RESET   39
#define LCD_CS      12
#define LCD_WIDTH   480
#define LCD_HEIGHT  480

// Shared I2C Bus (AXP2101 0x34, CST9220 0x5A, QMI8658 0x6B, PCF85063 0x51, ES8311 0x18)
#define IIC_SDA     15
#define IIC_SCL     14

// Capacitive Touch (CST9220)
#define TP_INT      11
#define TP_RST      40

// Top Physical Buttons (active-low with external 10K pull-ups)
// Key2 = BOOT (GPIO0), Key3 = IO18 (GPIO18), Key1 = PWR (via AXP2101 IRQ)
#define BTN_BOOT_PIN  0
#define BTN_USER_PIN  18

// Audio Codec ES8311 + NS4150B Speaker PA
#define I2S_MCLK_PIN  42
#define I2S_BCLK_PIN  9
#define I2S_LRCK_PIN  45
#define I2S_DOUT_PIN  8
#define I2S_DIN_PIN   10
#define PA_ENABLE_PIN 46
