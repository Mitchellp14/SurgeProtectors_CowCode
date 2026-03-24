// Display.h
#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include "types.h"

class Display {
public:
  void begin();

  void update(bool wifiConnected,
              bool firebaseReady,
              const String& currentTag,
              const GasReading& gas,
              uint32_t epoch);

private:
  static constexpr int TFT_CS  = 2;
  static constexpr int TFT_DC  = 21;
  static constexpr int TFT_RST = -1;

  static constexpr int SPI_SCK  = 19;
  static constexpr int SPI_MISO = 20;
  static constexpr int SPI_MOSI = 18;

  Adafruit_ILI9341 tft = Adafruit_ILI9341(&SPI, TFT_DC, TFT_CS, TFT_RST);

  void printLine(int16_t x, int16_t y, const String& text);
  void drawCow(int16_t x, int16_t y, int scale);
};