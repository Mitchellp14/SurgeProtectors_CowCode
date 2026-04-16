// CowSystem.ino
#define AD7190_DEBUG_CONFIG

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

// --- Pin assignments ---
static const int RFID_RX_PIN    = 14;
static const int RFID_TX_PIN    = -1;
static const uint32_t RFID_BAUD = 9600;
static const int INIR_RX_PIN    = 16;
static const int INIR_TX_PIN    = 17;
static const int I2C_SDA        = 6;
static const int I2C_SCL        = 7;
static const float VREF_MV      = 4800.0f;

// --- CS pins (must match AD7190.h) ---
// Bus A (FSPI, SCK=IO15, MOSI=IO22, MISO=IO23): IO2=ADC1, IO3=ADC2
// Bus B (HSPI, SCK=IO19, MOSI=IO20, MISO=IO21): IO1=ADC3, IO5=ADC4
static const int CS_PINS[] = {2, 3, 1, 5};
static const int NUM_CS     = 4;

// --- Modules ---
RfidTask       rfid;
GasTasks       gas;
SessionManager manager;
Uploader       uploader;
Display        display;
AD7190Driver   adcs(VREF_MV);

// --- Shared state ---
static LoadCellReading latestLoad;

// --- Tasks ---
Task tRFID      {0,     0};   // run every loop — RFID must be responsive
Task tGasF      {0,     0};   // run every loop — INIR UART parsing
Task tGasS      {1000,  0};   // SCD30 poll ~1Hz
Task tLoad      {500,  0};   // ADC read ~2Hz, no upload
Task tLoadUpload{10000, 0};   // load cell upload every 10s
Task tMgr       {50,    0};   // session state machine
Task tUpTick    {20,     0};   // Firebase keepalive
Task tUpload    {2000,  0};   // RFID session upload every 2s
Task tSample10s {5000, 0};   // gas accumulator sample
Task tMinuteChk {1000,  0};   // minute rollover check

// --- Gas accumulator ---
struct GasAccum {
  uint32_t count = 0, ch4Count = 0;
  double sumTemp = 0, sumHum = 0, sumCO2 = 0, sumCH4 = 0;
  uint32_t currentMinute = 0;
  bool minuteInitialized = false;

  void resetForMinute(uint32_t minute) {
    count = ch4Count = 0;
    sumTemp = sumHum = sumCO2 = sumCH4 = 0;
    currentMinute = minute;
    minuteInitialized = true;
  }

  void addSample(const GasReading &g) {
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

  bool hasAny() const { return count > 0 || ch4Count > 0; }
};
static GasAccum gasAccum;

// --- Helpers ---
static void uploadMinuteAverage(uint32_t minuteEpoch, const GasAccum &acc) {
  if (!uploader.ready() || !acc.hasAny()) return;

  GasReading avg = {};
  avg.scd_valid = (acc.count > 0);
  if (avg.scd_valid) {
    avg.tempC    = (float)(acc.sumTemp / acc.count);
    avg.humidity = (float)(acc.sumHum  / acc.count);
    avg.co2ppm   = (float)(acc.sumCO2  / acc.count);
  }
  avg.ch4_valid   = (acc.ch4Count > 0);
  avg.methane_ppm = avg.ch4_valid ? (int)lround(acc.sumCH4 / acc.ch4Count) : -1;
  avg.inir_faults = 0;
  avg.inir_temp_c = NAN;

  uploader.uploadGasSnapshot("MINUTE_AVG", avg, minuteEpoch);
}

static void handleGasAccumulator(uint32_t epoch) {
  if (epoch == 0) return;
  uint32_t minute = epoch / 60;

  if (!gasAccum.minuteInitialized) {
    gasAccum.resetForMinute(minute);
    return;
  }

  // Minute rolled over — upload previous minute then reset
  if (minute != gasAccum.currentMinute) {
    uploadMinuteAverage(gasAccum.currentMinute * 60, gasAccum);
    gasAccum.resetForMinute(minute);
  }
}

// ─────────────────────────────────────────────────────────────────────────────

void setup() {
  // Drive all CS pins high before anything else to prevent SPI glitches
  for (int i = 0; i < NUM_CS; i++) {
    pinMode(CS_PINS[i], OUTPUT);
    digitalWrite(CS_PINS[i], HIGH);
  }

  Serial.begin(115200);
  delay(500);

  rfid.begin(RFID_RX_PIN, RFID_TX_PIN, RFID_BAUD);
  gas.begin(I2C_SDA, I2C_SCL, INIR_RX_PIN, INIR_TX_PIN);

  Serial.println("\n=== AD7190 Weigh Scale System (Dual SPI Bus) ===");
  Serial.println("  Bus A (FSPI): SCK=IO15  MOSI=IO22  MISO=IO23  CS: IO2(ADC1) IO3(ADC2)");
  Serial.println("  Bus B (HSPI): SCK=IO19  MOSI=IO20  MISO=IO21  CS: IO1(ADC3) IO5(ADC4)");

  if (!adcs.begin()) {
    Serial.println("[ERROR] One or more ADCs failed to initialise. Check wiring.");
  } else {
    adcs.captureTare();  // zero scales at startup
  }

  manager.begin(20000, 5000);
  uploader.begin();

  Serial.println("CowSystem ready.");
}

void loop() {
  uint32_t now = millis();

  // --- Sensor ticks ---
  if (due(tRFID, now)) rfid.tick();
  if (due(tGasF, now)) gas.tickFast();
  if (due(tGasS, now)) gas.tickSlow(now);

  // --- ADC read (no upload here) ---
  if (due(tLoad, now)) {
    AD7190Result results[8];
    adcs.readAll(results);

    static uint32_t lastRaw[8] = {0};

    for (int i = 0; i < 8; i++) {
      // Log when a channel value is unchanged — actual recovery is handled
      // inside the driver via the midscale frozen detector in readChannel()
      if (results[i].valid && results[i].raw == lastRaw[i]) {
        Serial.printf("[WARN] Channel %d unchanged at 0x%06lX\n", i, results[i].raw);
      }
      lastRaw[i] = results[i].raw;

      // Cache latest reading
      latestLoad.valid[i]   = results[i].valid;
      latestLoad.raw[i]     = results[i].raw;
      latestLoad.voltage[i] = results[i].voltage;
      latestLoad.kg[i]      = results[i].kg;
    }

    // Print kg readings for all 8 channels
    Serial.print("[LOAD]");
    for (int i = 0; i < 8; i++) {
      if (results[i].valid) {
        Serial.printf(" ch%d=%.3fkg", i, results[i].kg);
      } else {
        Serial.printf(" ch%d=INVALID", i);
      }
    }
    Serial.println();

    latestLoad.last_ms = now;
  }

  // --- Load cell upload (slow, separate from read) ---
  if (due(tLoadUpload, now)) {
    uint32_t epoch = uploader.epochNow();
    if (uploader.ready() && epoch > 0) {
      uploader.uploadLoadCellSnapshot("MINUTE_AVG_LC", latestLoad, epoch);
    }
  }

  // --- Firebase keepalive ---
  if (due(tUpTick, now)) uploader.tick();

  // --- Session manager ---
  if (due(tMgr, now)) manager.tick(now);

  // --- RFID tag consumed -> start session ---
  String tag;
  if (rfid.consumeTag(tag)) {
    manager.startSession(tag, now);
  }

  // --- RFID session upload ---
  if (due(tUpload, now)) {
    if (manager.shouldUploadNow(now) && uploader.ready()) {
      GasReading gr  = gas.getLatest();
      uint32_t epoch = uploader.epochNow();
      if (epoch > 0) {
        //bool wifiOk = (WiFi.status() == WL_CONNECTED);
        if (uploader.uploadGasSnapshot(manager.currentTag(), gr, epoch)) {
          uploader.uploadLoadCellSnapshot(manager.currentTag(), latestLoad, epoch);
        //  display.update(wifiOk, uploader.ready(), manager.currentTag(), gr, epoch);
        }
      }
    }
  }

  // --- Gas accumulator: sample every 5s ---
  if (due(tSample10s, now)) {
    uint32_t epoch = uploader.epochNow();
    if (epoch > 0) {
      handleGasAccumulator(epoch);   // rolls over if minute changed
      gasAccum.addSample(gas.getLatest());
    }
  }

  // --- Minute rollover check every 1s ---
  if (due(tMinuteChk, now)) {
    handleGasAccumulator(uploader.epochNow());
  }

  // Serial command: 't' or 'T' to retare
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 't' || c == 'T') {
      Serial.println("Retare requested...");
      adcs.captureTare();
    }
  }
}
