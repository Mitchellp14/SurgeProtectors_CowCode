/**
 * AD7190_Example.ino
 *
 * Demonstrates reading all 8 load cell channels from 4× AD7190 ADCs
 * on an ESP32-C6-Mini.
 *
 * Weigh System 1: ADC1 (CS=IO5), ADC2 (CS=IO1)
 * Weigh System 2: ADC3 (CS=IO3), ADC4 (CS=IO2)
 *
 * Each ADC:
 *   Load Cell 1 → AIN1+/AIN2-  with REFIN2+/REFIN2-
 *   Load Cell 2 → AIN3+/AIN4-  with REFIN1+/REFIN1-
 *   BPDSW enabled: excitation ground routed through ADC's internal switch to AGND
 *
 * Settings: SINC4, CHOP enabled, Gain=128, ~50 Hz ODR, REJ60 notch active
 *
 * Vref: set to your actual reference voltage (e.g. 5000.0 mV for 5 V excitation)
 */

#define AD7190_DEBUG_CONFIG

#include "AD7190.h"

// ─── Reference Voltage ────────────────────────────────────────────────────────
// Set this to your actual REFIN voltage in millivolts.
// If load cells use ratiometric reference (sense lines tied to REFIN),
// set this to the excitation voltage measured at the load cell.
static const float VREF_MV = 5000.0f;

// ─── Driver Instance ──────────────────────────────────────────────────────────
AD7190Driver adcs(VREF_MV);

// ─── Calibration / Tare State ────────────────────────────────────────────────
// Store zero-load (tare) readings for each of the 8 channels
// Index: [adcIndex * 2 + channelIndex]
static double tare_mV[8]       = {0};
static bool   tare_captured[8] = {false};

// ─── Helper: print a result ───────────────────────────────────────────────────
void printResult(const char* label, const AD7190Result& r)
{
    if (!r.valid) {
        Serial.printf("  %-20s  ERROR (status=0x%02X)\n", label, r.status);
        return;
    }
    Serial.printf("  %-20s  raw=0x%06lX  %+8.4f mV\n", label, r.raw, r.voltage);
}

// ─── setup() ─────────────────────────────────────────────────────────────────
void setup()
{
    // Force all CS pins high IMMEDIATELY, before Serial or anything else
    pinMode(2, OUTPUT); digitalWrite(2, HIGH);  // CS4 – IO2 strapping pin
    pinMode(3, OUTPUT); digitalWrite(3, HIGH);  // CS3 – IO3 strapping pin
    pinMode(5, OUTPUT); digitalWrite(5, HIGH);  // CS1
    pinMode(1, OUTPUT); digitalWrite(1, HIGH);  // CS2
    
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== AD7190 4-ADC Weigh Scale System ===");

    // Initialise all 4 ADCs (resets, verifies ID, enables BPDSW, calibrates)
    bool ok = adcs.begin();
    if (!ok) {
        Serial.println("[ERROR] One or more ADCs failed to initialise. Check wiring.");
        // Continue anyway – partial system still useful for debugging
    }

    Serial.println("[INFO] Setup complete. Starting measurements.\n");
    delay(200);
}

// ─── loop() ──────────────────────────────────────────────────────────────────
void loop()
{
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

    // At 50 Hz per channel, reading 2 channels per ADC takes ~40 ms per ADC,
    // and 4 ADCs = ~160 ms total. No extra delay needed; the conversion wait
    // inside readChannel() provides the pacing.
}

// ─── Optional: Serial command handler ────────────────────────────────────────
// Uncomment serialEvent() below to handle 't' (tare) and 'c' (calibrate)
// commands from Serial Monitor.
/*
void serialEvent()
{
    while (Serial.available()) {
        char c = Serial.read();
        if (c == 't' || c == 'T') {
            // Retrigger tare on next loop
            memset(tare_captured, false, sizeof(tare_captured));
            Serial.println("[CMD] Tare will be captured on next reading.");
        }
        if (c == 'c' || c == 'C') {
            Serial.println("[CMD] Running calibration on all ADCs...");
            for (int i = 0; i < AD7190_NUM_ADCS; i++) {
                adcs.calibrate((AD7190Index)i);
            }
            Serial.println("[CMD] Calibration complete.");
        }
    }
}
*/
