// gasTasks.h
// Gas sensor module interface.
// Reads SCD30 over I2C (slow poll) and INIR2 methane over UART (fast stream parse).
// Keeps the latest readings cached so other modules can grab a snapshot.

#pragma once
#include <Arduino.h>
#include "types.h"

class GasTasks {
public:
  void begin(int sdaPin, int sclPin, int inirRx, int inirTx);

  // Fast path: call every loop to keep up with the INIR2 UART stream
  void tickFast();

  // Slow path: call ~1 Hz to poll SCD30
  void tickSlow(uint32_t nowMs);

  // Returns the latest cached readings
  GasReading getLatest() const;

private:
  GasReading latest;

  // INIR2 parsing + validation
  bool parseInirFrame();
  static float k10_to_c(uint32_t k10);
  static uint32_t sum_bytes_u32(uint32_t x);
  static uint32_t inir_calc_crc_normal(uint32_t conc_ppm, uint32_t faults, uint32_t temp_k10);
  static bool parse_hex_token(const char* tok, uint32_t &out);

  // Frame parsing state (between '[' and ']')
  bool inFrame = false;
  char buf[160]{};
  size_t idx = 0;

  // Used for INIR2 warm-up gating
  unsigned long bootMs = 0;

  // SCD30 poll timing
  uint32_t lastSCDMs = 0;

  // Stored pin config
  int sda = 6, scl = 7;
  int inirRX = 17, inirTX = 16;
};
