// Scheduler.h
// Tiny millis()-based scheduler helper.
// Each Task has an interval and the last time it ran.
// due() returns true when it’s time to run the task again (interval=0 => every loop).

#pragma once
#include <Arduino.h>

struct Task {
  uint32_t intervalMs; // 0 = every loop
  uint32_t lastMs;
};

inline bool due(Task &t, uint32_t nowMs) {
  if (t.intervalMs == 0) return true;

  // Run when enough time has elapsed, then update lastMs
  if ((uint32_t)(nowMs - t.lastMs) >= t.intervalMs) {
    t.lastMs = nowMs;
    return true;
  }
  return false;
}