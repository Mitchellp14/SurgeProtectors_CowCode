// manager.h
// Session manager interface.
// Keeps track of an “active logging session” tied to a specific RFID tag.
// Handles timing (session length + upload cadence). No sensor reads or Firebase calls here.

#pragma once
#include <Arduino.h>

class SessionManager {
public:
  // Configure session duration + upload interval
  void begin(uint32_t sessionDurationMs, uint32_t uploadIntervalMs);

  // Call regularly to handle session timeouts
  void tick(uint32_t nowMs);

  // Start/restart a session when a tag is scanned
  void startSession(const String &tagId, uint32_t nowMs);

  // Session status + upload trigger
  bool sessionActive(uint32_t nowMs) const;
  bool shouldUploadNow(uint32_t nowMs);   // true once per interval while active
  String currentTag() const;

private:
  String tag;
  bool active = false;

  uint32_t startMs = 0;
  uint32_t durationMs = 120000;

  uint32_t intervalMs = 5000;
  uint32_t lastUploadMs = 0;
};