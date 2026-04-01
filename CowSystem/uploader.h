// uploader.h
// Firebase uploader interface.
// Owns WiFi/Firebase setup and provides a simple “upload this snapshot” function.
// Sensor modules stay separate — they just hand over their latest readings.

#pragma once
#include <Arduino.h>
#include "types.h"

class Uploader {
public:
  // Connect WiFi, sync time, init Firebase/RTDB
  void begin();

  // Keep Firebase state machine running (call every loop)
  void tick();

  // True once Firebase auth/connection is ready to send
  bool ready() const;

  // Current epoch time from NTP (returns 0 if time isn't available yet)
  uint32_t epochNow();

  // Push one gas snapshot under the given RFID tag + timestamp
  bool uploadGasSnapshot(const String &rfidTag, const GasReading &gas, uint32_t epoch); //NOTE: If there is a runtime error, get rid of String RFIDtag datatype

  // Push one load cell snapshot under the given RFID tag + timestamp
  bool uploadLoadCellSnapshot(const String &rfidTag, const LoadCellReading &load, uint32_t epoch);

private:
  void initWiFi();
};