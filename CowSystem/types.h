// types.h
// Shared structs used across modules.
// Keeps the “data shapes” in one place so tasks can pass readings around cleanly.

#pragma once
#include <Arduino.h>

struct GasReading {
  // SCD30 (I2C)
  bool scd_valid = false;
  float tempC = NAN;
  float humidity = NAN;
  float co2ppm = NAN;

  // INIR2 methane (UART)
  bool ch4_valid = false;      
  int methane_ppm = -1; //maybe supposed to be uint32_t?
  uint32_t inir_faults = 0;
  float inir_temp_c = NAN;

  uint32_t last_ms = 0;        // last update time (millis)
};