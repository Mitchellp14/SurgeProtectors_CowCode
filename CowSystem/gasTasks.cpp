// gasTasks.cpp
// Gas sensor module:
// - SCD30 over I2C (polled ~1 Hz)
// - INIR2 methane over UART (parsed continuously)
// Keeps the latest readings cached so the uploader can grab a snapshot anytime.

#include "gasTasks.h"
#include <Wire.h>
#include "SparkFun_SCD30_Arduino_Library.h"

// SCD30 instance (I2C)
static SCD30 airSensor;

// INIR2 UART on Serial2
static HardwareSerial INIRSerial(1);

static const uint32_t INIR_BAUD = 38400;
static const uint32_t INIR_WARMUP_MS = 45000UL;

void GasTasks::begin(int sdaPin, int sclPin, int inirRx, int inirTx) {
  sda = sdaPin;
  scl = sclPin;
  inirRX = inirRx;
  inirTX = inirTx;

  bootMs = millis();

  // SCD30 init (I2C)
  Wire.begin(sda, scl);
  if (!airSensor.begin()) {
    Serial.println("SCD30 not detected. Check wiring.");
    latest.scd_valid = false;
  } else {
    latest.scd_valid = true;
  }

  // INIR2 init (UART, 8N2)
  INIRSerial.begin(INIR_BAUD, SERIAL_8N2, inirRX, inirTX);

  Serial.println("GasTasks initialized (SCD30 + INIR2 UART).");
}

void GasTasks::tickFast() {
  // Keep up with the INIR2 UART stream (don’t block; just consume available bytes)
  while (INIRSerial.available() > 0) {
    (void)parseInirFrame();
  }
}

void GasTasks::tickSlow(uint32_t nowMs) {
  // SCD30 updates slowly, so poll around 1 Hz
  if ((uint32_t)(nowMs - lastSCDMs) < 1000) return;
  lastSCDMs = nowMs;

  if (latest.scd_valid) {
    latest.tempC = airSensor.getTemperature();
    latest.co2ppm = airSensor.getCO2();
    latest.humidity = airSensor.getHumidity();
  }

  latest.last_ms = nowMs;
}

GasReading GasTasks::getLatest() const {
  return latest;
}

// ---------- INIR2 parsing helpers ----------

float GasTasks::k10_to_c(uint32_t k10) {
  float kelvin = ((float)k10) / 10.0f;
  return kelvin - 273.15f;
}

uint32_t GasTasks::sum_bytes_u32(uint32_t x) {
  uint32_t s = 0;
  s += (x >> 0)  & 0xFF;
  s += (x >> 8)  & 0xFF;
  s += (x >> 16) & 0xFF;
  s += (x >> 24) & 0xFF;
  return s;
}

uint32_t GasTasks::inir_calc_crc_normal(uint32_t conc_ppm, uint32_t faults, uint32_t temp_k10) {
  uint32_t val_crc = 0;
  val_crc += 0x5B; // '['
  val_crc += sum_bytes_u32(conc_ppm);
  val_crc += sum_bytes_u32(faults);
  val_crc += sum_bytes_u32(temp_k10);
  return val_crc;
}

bool GasTasks::parse_hex_token(const char* tok, uint32_t &out) {
  if (!tok) return false;
  if (strlen(tok) < 3) return false;
  if (!(tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X'))) return false;

  char* endptr = nullptr;
  unsigned long v = strtoul(tok, &endptr, 16);
  if (endptr == tok) return false;

  out = (uint32_t)v;
  return true;
}

// Frame parser for INIR2 NORMAL mode.
// Consumes the UART stream until it sees a full [ ... ] frame, then updates `latest`.
bool GasTasks::parseInirFrame() {
  char c = (char)INIRSerial.read();

  // Wait for start of frame
  if (!inFrame) {
    if (c == '[') {
      inFrame = true;
      idx = 0;
    }
    return false;
  }

  // End of frame: parse and validate
  if (c == ']') {
    buf[idx] = '\0';
    inFrame = false;

    uint32_t conc = 0, faults = 0, temp = 0, crc = 0, inv = 0;

    char tmp[160];
    strncpy(tmp, buf, sizeof(tmp));
    tmp[sizeof(tmp) - 1] = '\0';

    const char* delims = " \r\n\t";
    char* tok = strtok(tmp, delims);
    int got = 0;

    // Pull out the first 5 hex tokens (0x........)
    while (tok && got < 5) {
      uint32_t v;
      if (parse_hex_token(tok, v)) {
        if (got == 0) conc = v;
        if (got == 1) faults = v;
        if (got == 2) temp = v;
        if (got == 3) crc = v;
        if (got == 4) inv = v;
        got++;
      }
      tok = strtok(nullptr, delims);
    }

    if (got < 5) return false;

    // CRC + warmup gate
    uint32_t calc = inir_calc_crc_normal(conc, faults, temp);
    bool crc_ok = (calc == crc) && ((~crc) == inv);
    bool warm_ok = (millis() - bootMs) >= INIR_WARMUP_MS;

    latest.ch4_valid = crc_ok && warm_ok;
    latest.methane_ppm = latest.ch4_valid ? (int)conc : -1;
    //latest.methane_ppm = ; //
    latest.inir_faults = faults;
    latest.inir_temp_c = k10_to_c(temp);
    latest.last_ms = millis();

    // Debug print (comment out if noisy)
    Serial.print("INIR: CH4=");
    Serial.print(conc);
    Serial.print(" ppm faults=0x");
    Serial.print(faults, HEX);
    Serial.print(" Tinir=");
    Serial.print(latest.inir_temp_c, 2);
    Serial.print("C valid=");
    Serial.println(latest.ch4_valid ? "yes" : "no");

    return true;
  }

  // Store frame content (everything between '[' and ']')
  if (idx < sizeof(buf) - 1) {
    buf[idx++] = c;
  } else {
    // Overflow: drop the frame and resync
    inFrame = false;
    idx = 0;
  }

  return false;
}