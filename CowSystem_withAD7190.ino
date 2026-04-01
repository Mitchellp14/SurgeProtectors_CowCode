// CowSystem.ino
// Main entry point for the firmware.
// Wires together the modules (RFID, gas sensors, session manager, uploader)
// and runs them on a simple millis()-based schedule (no FreeRTOS).

#include <Arduino.h>
#include <WiFi.h>
#include "Scheduler.h"
#include "types.h"
#include "RfidTask.h"
#include "gasTasks.h"
#include "manager.h"
#include "uploader.h"
#include "Display.h"
#include "AD7190.h"

#define AD7190_DEBUG_CONFIG

// --- Pin assignments ---
static const int RFID_RX_PIN = 14;     // RFID: single-wire RX
static const int RFID_TX_PIN = -1;     // not used
static const uint32_t RFID_BAUD = 9600;

static const int INIR_RX_PIN = 17;     // INIR2 methane UART RX
static const int INIR_TX_PIN = 16;     // INIR2 methane UART TX

// SCD30 I2C pins
static const int I2C_SDA = 6;
static const int I2C_SCL = 7;

// ─── Reference Voltage ────────────────────────────────────────────────────────
// set this to the excitation voltage measured at the load cell.
static const float VREF_MV = 5000.0f; 

// ─── Calibration / Tare State ────────────────────────────────────────────────
// Store zero-load (tare) readings for each of the 8 channels
// Index: [adcIndex * 2 + channelIndex]
static double tare_mV[8]       = {0};
static bool   tare_captured[8] = {false};

// ─── Driver Instance ──────────────────────────────────────────────────────────
AD7190Driver adcs(VREF_MV);

// --- Modules ---
RfidTask rfid;
GasTasks gas;
SessionManager manager;
Uploader uploader;
Display display;

// --- Scheduling ---
// intervalMs = 0 means run every loop iteration.
Task tRFID   {0,    0};    // RFID should be responsive
Task tGasF   {0,    0};    // INIR2 UART parsing should run constantly
Task tGasS   {1000, 0};    // SCD30 poll rate (~1 Hz)
Task tMgr    {50,   0};    // session timing/state
Task tUpTick {0,    0};    // keep Firebase app loop alive
Task tUpload {20,   0};    // check upload trigger frequently
Task tSample10s {10000, 0};   // sample every 10 seconds
Task tMinuteChk {1000,  0};   // check minute rollover every 1 second

//Gas Accumulator for averaging every minute
struct GasAccum {
  uint32_t count = 0;
  double sumTemp = 0;
  double sumHum  = 0;
  double sumCO2  = 0;
  double sumCH4  = 0;
  uint32_t ch4Count = 0;     // only count valid CH4 samples

  // track which minute we’re accumulating
  uint32_t currentMinute = 0;
  bool minuteInitialized = false;

  // ─── Helper: print a result ───────────────────────────────────────────────────
  void printResult(const char* label, const AD7190Result& r)
  {
      if (!r.valid) {
          Serial.printf("  %-20s  ERROR (status=0x%02X)\n", label, r.status);
          return;
      }
      Serial.printf("  %-20s  raw=0x%06lX  %+8.4f mV\n", label, r.raw, r.voltage);
  }

  void resetForMinute(uint32_t minute) {
    count = 0;
    sumTemp = sumHum = sumCO2 = sumCH4 = 0;
    ch4Count = 0;
    currentMinute = minute;
    minuteInitialized = true;
  }

  void addSample(const GasReading &g) {
    // only average the pieces that are valid-ish
    if (g.scd_valid) {
      sumTemp += g.tempC;
      sumHum  += g.humidity;
      sumCO2  += g.co2ppm;
      count++;
    }
    if (g.ch4_valid) {
      sumCH4 += g.methane_ppm;
      ch4Count++;
    }
  }

  bool hasAny() const {
    return count > 0 || ch4Count > 0;
  }
};
static GasAccum gasAccum;

static bool uploadMinuteAverage(uint32_t minuteEpoch, const GasAccum &acc) {
  GasReading avg;
  avg.scd_valid = (acc.count > 0);
  if (avg.scd_valid) {
    avg.tempC    = (float)(acc.sumTemp / acc.count);
    avg.humidity = (float)(acc.sumHum  / acc.count);
    avg.co2ppm   = (float)(acc.sumCO2  / acc.count);
  }

  avg.ch4_valid = (acc.ch4Count > 0);
  avg.methane_ppm = avg.ch4_valid ? (int)lround(acc.sumCH4 / acc.ch4Count) : -1;

  // Keep these for debugging
  avg.inir_faults = 0;
  avg.inir_temp_c = NAN;

  // Use a fixed tag/category so averages don’t mix with RFID events
  // Example: RFID tag name "MINUTE_AVG"
  bool ok = uploader.uploadGasSnapshot("MINUTE_AVG", avg, minuteEpoch);
  if (ok) {
    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    display.update(wifiOk, uploader.ready(), "MINUTE_AVG", avg, minuteEpoch);
  }
  return ok;
}

void setup() {
  // Force all CS pins high IMMEDIATELY, before Serial or anything else
  pinMode(2, OUTPUT); digitalWrite(2, HIGH);  // CS4 – IO2 strapping pin
  pinMode(3, OUTPUT); digitalWrite(3, HIGH);  // CS3 – IO3 strapping pin
  pinMode(5, OUTPUT); digitalWrite(5, HIGH);  // CS1
  pinMode(1, OUTPUT); digitalWrite(1, HIGH);  // CS2

  Serial.begin(115200);
  delay(500);

  rfid.begin(RFID_RX_PIN, RFID_TX_PIN, RFID_BAUD);
  gas.begin(I2C_SDA, I2C_SCL, INIR_RX_PIN, INIR_TX_PIN);
  display.begin();

  //─── ADC Begin ─────────────────────────────────────────────────────────────────
  Serial.println("\n=== AD7190 4-ADC Weigh Scale System ===");
  // Initialise all 4 ADCs (resets, verifies ID, enables BPDSW, calibrates)
  bool ok = adcs.begin();
  if (!ok) {
      Serial.println("[ERROR] One or more ADCs failed to initialise. Check wiring.");
      // Continue anyway – partial system still useful for debugging
  }

  manager.begin(20000, 5000); // 20s session, upload every 5s
  uploader.begin();

  Serial.println("CowSystem ready.");
}

void loop() {
  uint32_t now = millis();

  // --- Regular task ticks ---
  if (due(tRFID, now))   rfid.tick();
  if (due(tGasF, now))   gas.tickFast();
  if (due(tGasS, now))   gas.tickSlow(now);
  if (due(tMgr, now))    manager.tick(now);
  if (due(tUpTick, now)) uploader.tick();

  // --- RFID event -> start session ---
  String tag;
  if (rfid.consumeTag(tag)) {
    manager.startSession(tag, now);
  }

  // --- RFID session uploads (unchanged behavior) ---
  if (due(tUpload, now)) {
    if (manager.shouldUploadNow(now) && uploader.ready()) {
      GasReading gr = gas.getLatest();
      uint32_t epoch = uploader.epochNow();
      bool ok = uploader.uploadGasSnapshot(manager.currentTag(), gr, epoch);
      if (ok) {
        bool wifiOk = (WiFi.status() == WL_CONNECTED);
        display.update(wifiOk, uploader.ready(), manager.currentTag(), gr, epoch);
      }
    }
  }

  // =========================================================
  // Integration test logging plan:
  //   - sample a gas reading every 10 seconds
  //   - once per minute, upload the minute-average
  // =========================================================

  // --- 10s sampling into accumulator ---
  if (due(tSample10s, now)) {
    uint32_t epoch = uploader.epochNow();     // returns 0 if NTP isn't ready yet
    if (epoch != 0) {
      uint32_t minute = epoch / 60;

      // Initialize or roll accumulator if we somehow jumped minutes
      if (!gasAccum.minuteInitialized) {
        gasAccum.resetForMinute(minute);
      } else if (minute != gasAccum.currentMinute) {
        // finalize previous minute before switching
        if (uploader.ready() && gasAccum.hasAny()) {
          (void)uploadMinuteAverage(gasAccum.currentMinute * 60, gasAccum);
        }
        gasAccum.resetForMinute(minute);
      }

      GasReading g = gas.getLatest();
      gasAccum.addSample(g);
    }
  }

  // --- minute rollover check + upload the previous minute average ---
  if (due(tMinuteChk, now)) {
    uint32_t epoch = uploader.epochNow();
    if (epoch == 0) return;  // no time sync yet, nothing to do

    uint32_t minute = epoch / 60;

    if (!gasAccum.minuteInitialized) {
      gasAccum.resetForMinute(minute);
      return;
    }

    // minute changed -> upload the previous minute's average
    if (minute != gasAccum.currentMinute) {
      if (uploader.ready() && gasAccum.hasAny()) {
        // store at the start-of-minute timestamp (minute*60)
        (void)uploadMinuteAverage(gasAccum.currentMinute * 60, gasAccum);
      }
      gasAccum.resetForMinute(minute);
    }
  }

  //── Load cell loop ───────────────────────────────────────────────
  static uint32_t loopCount = 0;

  // ── Read all 8 channels ───────────────────────────────────────────────
  AD7190Result results[8];
  adcs.readAll(results);

  static uint32_t lastRaw[8] = {0};
  static uint8_t  frozenCount[8] = {0};

  for (int i = 0; i < 8; i++) {
      if (results[i].valid && results[i].raw == lastRaw[i]) {
          frozenCount[i]++;
          if (frozenCount[i] >= 3) {
              Serial.printf("[WARN] Channel %d frozen at 0x%06lX for %d loops\n",
                          i, results[i].raw, frozenCount[i]);
          }
      } else {
          frozenCount[i] = 0;
      }
      lastRaw[i] = results[i].raw;
  }

  // ── Print results ─────────────────────────────────────────────────────
  Serial.printf("\n─── Loop %lu ───────────────────────────────────\n", loopCount);

  const char* labels[8] = {
      "WS1-ADC1-LC1", "WS1-ADC1-LC2",
      "WS1-ADC2-LC1", "WS1-ADC2-LC2",
      "WS2-ADC3-LC1", "WS2-ADC3-LC2",
      "WS2-ADC4-LC1", "WS2-ADC4-LC2"
  };

  for (int i = 0; i < 8; i++) {
      printResult(labels[i], results[i]);
  }

  // ── Weigh System Sums (both ADCs per system summed) ───────────────────
  // Each weigh system has 2 ADCs × 2 load cells = 4 channels total.
  // You may want to sum or average depending on your mechanical setup.
  if (results[0].valid && results[1].valid &&
      results[2].valid && results[3].valid)
  {
      double ws1_total = results[0].voltage + results[1].voltage
                        + results[2].voltage + results[3].voltage;
      Serial.printf("  >> Weigh System 1 total: %+9.4f mV\n", ws1_total);
  }

  if (results[4].valid && results[5].valid &&
      results[6].valid && results[7].valid)
  {
      double ws2_total = results[4].voltage + results[5].voltage
                        + results[6].voltage + results[7].voltage;
      Serial.printf("  >> Weigh System 2 total: %+9.4f mV\n", ws2_total);
  }

  // ── Tare capture on first valid loop ─────────────────────────────────
  if (loopCount == 0) {
      Serial.println("\n[INFO] Capturing tare (zero-load baseline)...");
      for (int i = 0; i < 8; i++) {
          if (results[i].valid) {
              tare_mV[i]       = results[i].voltage;
              tare_captured[i] = true;
          }
      }
  }

  // ── Net (tare-subtracted) readings ────────────────────────────────────
  if (loopCount > 0) {
      Serial.println("  Net (tare-zeroed):");
      for (int i = 0; i < 8; i++) {
          if (results[i].valid && tare_captured[i]) {
              double net = results[i].voltage - tare_mV[i];
              Serial.printf("  %-20s  %+8.4f mV\n", labels[i], net);
          }
      }
  }

  loopCount++;
  // ── Load cell loop ───────────────────────────────────────────────

}
