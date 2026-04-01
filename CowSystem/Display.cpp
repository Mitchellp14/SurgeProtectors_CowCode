// Display.cpp
#include "Display.h"

void Display::begin() {
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  delay(50);

  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(ILI9341_DARKGREY);
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE);

  printLine(10, 10, "CowSystem Display");
  tft.drawLine(10, 32, 310, 32, ILI9341_BLACK);
}

void Display::printLine(int16_t x, int16_t y, const String& text) {
  tft.fillRect(x, y, 190, 18, ILI9341_DARKGREY);
  tft.setCursor(x, y);
  tft.print(text);
}

void Display::drawCow(int16_t x, int16_t y, int scale) {
  const uint16_t colors[4] = {
    ILI9341_DARKGREY, // 0, not drawn
    ILI9341_BLACK,    // 1
    ILI9341_WHITE,    // 2
    ILI9341_PINK      // 3
  };

  const uint8_t cow[15][25] = {
    {1,1,1,1,0,0,1,1,1,0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,2,2,2,1,1,2,2,2,1,0,0,0,0,0,0,0,0,0,0},
    {0,1,1,1,1,2,2,2,2,2,2,1,1,1,1,1,1,1,1,1,0,0,0,0,0},
    {0,0,0,1,2,2,2,2,2,2,2,1,2,2,2,2,1,1,1,1,1,1,1,0,0},
    {0,0,0,1,2,1,2,2,1,2,2,1,2,2,2,2,2,1,1,1,2,1,0,1,0},
    {0,0,0,1,2,1,2,2,1,2,2,1,2,2,2,2,2,2,2,2,2,1,0,1,0},
    {0,0,0,1,3,3,3,3,3,3,2,1,2,2,1,1,1,2,2,2,2,1,0,1,0},
    {0,0,1,3,3,1,3,3,1,3,3,1,2,1,1,1,1,2,2,2,2,1,0,1,0},
    {0,0,1,3,3,3,3,3,3,3,3,1,2,1,1,1,2,2,2,2,2,1,0,1,1},
    {0,0,0,1,3,3,3,3,3,3,1,2,2,2,2,2,2,2,2,2,2,1,0,1,1},
    {0,0,0,0,1,1,1,1,1,1,2,2,2,1,1,3,3,3,1,1,2,1,0,1,0},
    {0,0,0,0,0,0,0,1,2,1,1,1,2,1,2,1,3,3,3,1,2,1,0,0,0},
    {0,0,0,0,0,0,0,1,2,1,0,1,2,1,0,1,2,1,0,1,2,1,0,0,0},
    {0,0,0,0,0,0,0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1,0,0,0},
    {0,0,0,0,0,0,0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1,0,0,0}
  };

  for (int row = 0; row < 15; row++) {
    for (int col = 0; col < 25; col++) {
      uint8_t pixel = cow[row][col];
      if (pixel != 0) {
        tft.fillRect(x + col * scale, y + row * scale, scale, scale, colors[pixel]);
      }
    }
  }
}

void Display::update(bool wifiConnected,
                     bool firebaseReady,
                     const String& currentTag,
                     const GasReading& gas,
                     uint32_t epoch) {
  tft.fillScreen(ILI9341_DARKGREY);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);

  printLine(10, 10, "CowSystem Status");
  tft.drawLine(10, 32, 310, 32, ILI9341_BLACK);

  printLine(10, 45, "WiFi: " + String(wifiConnected ? "OK" : "DOWN"));
  printLine(10, 65, "Firebase: " + String(firebaseReady ? "READY" : "WAIT"));

  String tagText = currentTag.length() > 0 ? currentTag : "NONE";
  printLine(10, 95, "RFID: " + tagText);

  if (gas.scd_valid) {
    printLine(10, 125, "CO2: " + String((int)gas.co2ppm) + " ppm");
  } else {
    printLine(10, 125, "CO2: (no SCD30)");
  }

  if (gas.ch4_valid) {
    printLine(10, 155, "CH4: " + String(gas.methane_ppm) + " ppm");
  } else {
    printLine(10, 155, "CH4: INVALID/WARM");
  }

  printLine(10, 185, "t=" + String(epoch));

  drawCow(215, 70, 4);
}