// RfidTask.h
// Simple interface for the RFID reader.
// begin(): sets up the UART + RFID library
// tick(): runs the parser (call often)
// consumeTag(): returns a new tag once, then clears it

#pragma once
#include <Arduino.h>

class RfidTask {
public:
  void begin(int rxPin, int txPin, uint32_t baud);
  void tick();
  bool consumeTag(String &tagOut);

  // Used internally by the library callback (see RfidTask.cpp)
  void onTagRead(uint32_t id);

private:
  String pendingTag;
  volatile bool hasTag = false;
};