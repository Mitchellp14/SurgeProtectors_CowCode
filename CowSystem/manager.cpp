// manager.cpp
// Session timing/state.
// Starts a logging window when a new RFID tag comes in, keeps it alive for a fixed duration,
// and triggers periodic uploads during that window.

#include "manager.h"

void SessionManager::begin(uint32_t sessionDurationMs, uint32_t uploadIntervalMs) {
  durationMs = sessionDurationMs;
  intervalMs = uploadIntervalMs;
}

void SessionManager::startSession(const String &tagId, uint32_t nowMs) {
  tag = tagId;
  active = true;
  startMs = nowMs;

  // Set to 0 so the first call to shouldUploadNow() fires immediately
  lastUploadMs = 0;

  Serial.print("Session started for RFID ");
  Serial.println(tag);
}

void SessionManager::tick(uint32_t nowMs) {
  if (!active) return;

  // Stop the session once the window has elapsed
  if ((uint32_t)(nowMs - startMs) > durationMs) {
    active = false;
    Serial.println("Session ended.");
  }
}

bool SessionManager::sessionActive(uint32_t nowMs) const {
  if (!active) return false;
  return (uint32_t)(nowMs - startMs) <= durationMs;
}

bool SessionManager::shouldUploadNow(uint32_t nowMs) {
  if (!sessionActive(nowMs)) return false;

  // First upload in a session happens immediately
  if (lastUploadMs == 0) {
    lastUploadMs = nowMs;
    return true;
  }

  // After that, upload on the fixed interval
  if ((uint32_t)(nowMs - lastUploadMs) >= intervalMs) {
    lastUploadMs = nowMs;
    return true;
  }

  return false;
}

String SessionManager::currentTag() const {
  return tag;
}