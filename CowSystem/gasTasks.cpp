// gasTasks.cpp - corrected for INIR2-ME5 ASCII hex frame format
#include "gasTasks.h"
#include <Wire.h>
#include <HardwareSerial.h>
#include "SparkFun_SCD30_Arduino_Library.h"

static SCD30 airSensor;

// Confirmed: 38400 8N1, RX=16, TX=17, Serial1
static HardwareSerial& INIR = Serial0;
static const uint32_t INIR_BAUD    = 38400;
static const uint32_t INIR_WARMUP_MS = 45000UL;

// Non-blocking line accumulator
static char     inirLineBuf[32];
static uint8_t  inirLineLen  = 0;
static bool     inFrameState = false;
static String   inirFields[6];
static uint8_t  inirFieldIdx = 0;

static void sendInirCmd(const char* cmd) {
  INIR.print(cmd);
  Serial.print("Sent to INIR: ");
  Serial.println(cmd);
  delay(300);
}

// Temperature conversion: K×10 → Celsius
float GasTasks::k10_to_c(uint32_t k10) {
  return ((float)k10 / 10.0f) - 273.15f;
}

// CRC validation
uint32_t GasTasks::sum_bytes_u32(uint32_t x) {
  return ((x >> 0) & 0xFF) + ((x >> 8) & 0xFF) +
         ((x >> 16) & 0xFF) + ((x >> 24) & 0xFF);
}

uint32_t GasTasks::inir_calc_crc_normal(uint32_t conc, uint32_t faults, uint32_t temp) {
  uint32_t crc = 0x5B;  // '['
  crc += sum_bytes_u32(conc);
  crc += sum_bytes_u32(faults);
  crc += sum_bytes_u32(temp);
  return crc;
}

void GasTasks::begin(int sdaPin, int sclPin, int inirRx, int inirTx) {
  sda    = sdaPin;
  scl    = sclPin;
  inirRX = inirRx;
  inirTX = inirTx;
  bootMs = millis();

  // SCD30 init
  Wire.begin(sda, scl);
  if (!airSensor.begin(Wire)) {
    Serial.println("SCD30 not detected. Check wiring.");
    latest.scd_valid = false;
  } else {
    latest.scd_valid = true;
    Serial.println("SCD30 initialized.");
  }

  // INIR2 init — confirmed 38400 8N1, no [C] command
  Serial.printf("[INIR] Starting Serial2 RX=%d TX=%d baud=%lu\n", 
                inirRX, inirTX, INIR_BAUD);

  INIR.begin(INIR_BAUD, SERIAL_8N1, inirRX, inirTX);
  delay(1000);
  sendInirCmd("[A]");

  Serial.printf("[INIR] Init complete, waiting for frames...\n");

  Serial.println("GasTasks initialized (SCD30 + INIR2 UART).");
}

//void GasTasks::tickFast() {
  // Keep reading frames as long as data is available
  // but don't block — only attempt a frame if we have enough bytes
//  while (INIR.available() > 0) {
//   parseInirFrame();
//  }
//}

void GasTasks::tickFast() {
  static uint32_t byteCount = 0;
  static uint32_t lastPrint = 0;

  while (INIR.available()) {
    char c = (char)INIR.read();
    byteCount++;

    // Print byte count every 5 seconds to confirm data arriving
    if (millis() - lastPrint > 5000) {
      lastPrint = millis();
      Serial.printf("[INIR] Bytes received so far: %lu\n", byteCount);
    }

    // Accumulate until newline
    if (c == '\n') {
      inirLineBuf[inirLineLen] = '\0';

      // Trim carriage return if present
      String line = String(inirLineBuf);
      line.trim();
      inirLineLen = 0;

      if (line.length() == 0) continue;

      if (!inFrameState) {
        // Look for frame start
        if (line.equalsIgnoreCase("0000005b")) {
          inFrameState = true;
          inirFieldIdx = 0;
        }
      } else {
        // Accumulate fields
        inirFields[inirFieldIdx++] = line;

        // We need 6 fields: conc, faults, temp, crc, invcrc, end(5d)
        if (inirFieldIdx >= 6) {
          inFrameState = false;

          uint32_t conc   = strtoul(inirFields[0].c_str(), nullptr, 16);
          uint32_t faults = strtoul(inirFields[1].c_str(), nullptr, 16);
          uint32_t temp   = strtoul(inirFields[2].c_str(), nullptr, 16);
          uint32_t crc    = strtoul(inirFields[3].c_str(), nullptr, 16);
          uint32_t inv    = strtoul(inirFields[4].c_str(), nullptr, 16);
          // inirFields[5] is "0000005d" end marker

          uint32_t calc  = inir_calc_crc_normal(conc, faults, temp);
          bool crc_ok    = (calc == crc) && ((~crc) == inv);
          bool warm_ok   = (millis() - bootMs) >= INIR_WARMUP_MS;

          latest.ch4_valid   = crc_ok && warm_ok;
          latest.methane_ppm = latest.ch4_valid ? (int)conc : -1;
          latest.inir_faults = faults;
          latest.inir_temp_c = k10_to_c(temp);
          latest.last_ms     = millis();

          Serial.print("INIR: CH4=");
          Serial.print(conc);
          Serial.print("ppm faults=0x");
          Serial.print(faults, HEX);
          Serial.print(" Tinir=");
          Serial.print(latest.inir_temp_c, 1);
          Serial.print("C crc=");
          Serial.print(crc_ok ? "OK" : "FAIL");
          Serial.print(" warm=");
          Serial.println(warm_ok ? "yes" : "warming up");
        }
      }

    } else if (c != '\r') {
      // Accumulate character, guard against overflow
      if (inirLineLen < sizeof(inirLineBuf) - 1) {
        inirLineBuf[inirLineLen++] = c;
      }
    }
  }
}

void GasTasks::tickSlow(uint32_t nowMs) {
  if ((uint32_t)(nowMs - lastSCDMs) < 1000) return;
  lastSCDMs = nowMs;

  if (latest.scd_valid && airSensor.dataAvailable()) {
    latest.tempC    = airSensor.getTemperature();
    latest.co2ppm   = airSensor.getCO2();
    latest.humidity = airSensor.getHumidity();
  }
  latest.last_ms = nowMs;
}

GasReading GasTasks::getLatest() const {
  return latest;
}

// parseInirFrame no longer needed — tickFast handles everything
bool GasTasks::parseInirFrame() { return false; }

//bool GasTasks::parseInirFrame() {
  // Read lines until we find frame start '0000005b'
//  String line = INIR.readStringUntil('\n');
//  line.trim();

//  if (!line.equalsIgnoreCase("0000005b")) {
//    return false;  // not a frame start, discard
//  }

  // Read 5 fields: conc, faults, temp, crc, inv_crc
  // Field 5 is ']' (0000005d) — read and discard
//  String concStr   = INIR.readStringUntil('\n'); concStr.trim();
//  String faultStr  = INIR.readStringUntil('\n'); faultStr.trim();
//  String tempStr   = INIR.readStringUntil('\n'); tempStr.trim();
//  String crcStr    = INIR.readStringUntil('\n'); crcStr.trim();
//  String invStr    = INIR.readStringUntil('\n'); invStr.trim();
//  String endStr    = INIR.readStringUntil('\n'); endStr.trim(); // 0000005d

//  uint32_t conc   = strtoul(concStr.c_str(),  nullptr, 16);
//  uint32_t faults = strtoul(faultStr.c_str(), nullptr, 16);
//  uint32_t temp   = strtoul(tempStr.c_str(),  nullptr, 16);
//  uint32_t crc    = strtoul(crcStr.c_str(),   nullptr, 16);
//  uint32_t inv    = strtoul(invStr.c_str(),   nullptr, 16);

  // Validate CRC
//  uint32_t calc   = inir_calc_crc_normal(conc, faults, temp);
//  bool crc_ok     = (calc == crc) && ((~crc) == inv);
//  bool warm_ok    = (millis() - bootMs) >= INIR_WARMUP_MS;

//  latest.ch4_valid    = crc_ok && warm_ok;
//  latest.methane_ppm  = latest.ch4_valid ? (int)conc : -1;
//  latest.inir_faults  = faults;
//  latest.inir_temp_c  = k10_to_c(temp);
//  latest.last_ms      = millis();

//  Serial.print("INIR: CH4=");
//  Serial.print(conc);
//  Serial.print("ppm faults=0x");
//  Serial.print(faults, HEX);
//  Serial.print(" Tinir=");
//  Serial.print(latest.inir_temp_c, 1);
//  Serial.print("C crc=");
//  Serial.print(crc_ok ? "OK" : "FAIL");
//  Serial.print(" warm=");
//  Serial.println(warm_ok ? "yes" : "no (warming up)");

//  return crc_ok;
//}
