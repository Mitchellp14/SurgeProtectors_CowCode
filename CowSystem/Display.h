// Display.h
#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <SD.h> //may not be needed
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
  //TFT pin assignments
  static constexpr int TFT_CS  = 2;
  static constexpr int TFT_DC  = 21;
  static constexpr int TFT_RST = -1;

  // SPI pin assignments
  static constexpr int SPI_SCK  = 19;
  static constexpr int SPI_MISO = 20;
  static constexpr int SPI_MOSI = 18;

  //Added by Avery 3-29-26, setting both the chip select for TFT and SD to the same thing:
  static constexpr int SD_CS    = 2; //This is also kind of a SPI pin but it'll be labeled as SD_CS to indicate it's the chip select for SD card

  Adafruit_ILI9341 tft = Adafruit_ILI9341(&SPI, TFT_DC, TFT_CS, TFT_RST);

  void printLine(int16_t x, int16_t y, const String& text);
  void drawCow(int16_t x, int16_t y, int scale);
};