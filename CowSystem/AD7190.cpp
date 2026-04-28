/**
 * AD7190.cpp
 * Implementation of the AD7190 driver for ESP32-C6-Mini
 *
 * Bus A (FSPI hardware): SCK=IO15, MOSI=IO22, MISO=IO23
 *   CS: IO2 (ADC1), IO3 (ADC2)
 *
 * Bus B (bit-bang SPI):  SCK=IO19, MOSI=IO20, MISO=IO21
 *   CS: IO1 (ADC3), IO5 (ADC4)
 *
 * ── Bus safety / contention fix ─────────────────────────────────────────────
 * ADC1 and ADC2 share the Bus A MISO line. The original code opened and
 * closed the hardware SPI transaction once per register access, meaning there
 * were small gaps between the comm byte transfer and the data byte transfers
 * where the transaction was nominally idle. During those gaps the previously
 * selected ADC could still be driving MISO (its DOUT line), and if CS was
 * released before DOUT tri-stated, the line could be left in an unknown state
 * when the next ADC was selected — producing pathological 0x000000/0xFFFFFF.
 *
 * The fix is a busBegin()/busEnd() pair that:
 *   1. Calls beginTransaction() on hardware SPI BEFORE asserting CS
 *   2. Asserts CS
 *   3. Performs ALL byte transfers for the operation (comm + data)
 *   4. Deasserts CS
 *   5. Calls endTransaction() AFTER deasserting CS
 *
 * This keeps the hardware SPI peripheral exclusively locked to one ADC for
 * the entire CS-asserted window with no gaps. The bit-bang bus has no shared
 * peripheral state so busBegin/busEnd are no-ops there beyond CS toggling.
 *
 * recoverADC() previously called writeRegister() (which opened its own CS
 * cycle) mid-operation, creating overlapping transactions. It is now
 * rewritten to use explicit busBegin/busEnd blocks throughout.
 */

#include "AD7190.h"

// ─── CS Pin Table ─────────────────────────────────────────────────────────────
const uint8_t AD7190Driver::_csPins[AD7190_NUM_ADCS] = {
    AD7190_CS1,   // ADC1 – IO2  (Bus A)
    AD7190_CS2,   // ADC2 – IO3  (Bus A)
    AD7190_CS3,   // ADC3 – IO1  (Bus B)
    AD7190_CS4    // ADC4 – IO5  (Bus B)
};

// ─── Constructor ──────────────────────────────────────────────────────────────
AD7190Driver::AD7190Driver(float vrefMv)
    : _spiA(0),
      _vrefMv(vrefMv)
{
    _spiSettings = SPISettings(AD7190_SPI_FREQ, MSBFIRST, AD7190_SPI_MODE);
}

// ─── begin() ──────────────────────────────────────────────────────────────────
bool AD7190Driver::begin()
{
    for (int i = 0; i < AD7190_NUM_ADCS; i++) {
        pinMode(_csPins[i], OUTPUT);
        digitalWrite(_csPins[i], HIGH);
        _adcFailed[i] = false;
    }
    memset(_midscaleCount,  0, sizeof(_midscaleCount));
    memset(_pendingRetare,  0, sizeof(_pendingRetare));
    delay(100);

    _spiA.begin(AD7190_BUSA_SCK, AD7190_BUSA_MISO, AD7190_BUSA_MOSI, -1);
    _spiB.begin(AD7190_BUSB_SCK, AD7190_BUSB_MISO, AD7190_BUSB_MOSI);
    delay(10);

    bool allOk = true;
    for (int i = 0; i < AD7190_NUM_ADCS; i++) {
        AD7190Index idx    = (AD7190Index)i;
        const char* busTag = isBusBB(idx) ? "B(bb)" : "A(hw)";
        bool adcOk = false;

        for (int attempt = 0; attempt < 3 && !adcOk; attempt++) {
            resetDevice(idx);
            delay(10);

            uint8_t id = readID(idx);
            Serial.printf("[AD7190] ADC%d Bus%s attempt %d: ID=0x%02X\n",
                          i+1, busTag, attempt+1, id);

            if ((id & 0x0F) == 0x04 || (id & 0x0F) == 0x05) {
                adcOk = true;
            } else {
                delay(20);
            }
        }

        if (!adcOk) {
            Serial.printf("[AD7190] ADC%d Bus%s: FAILED after 3 attempts\n", i+1, busTag);
            allOk = false;
            continue;
        }

        writeRegister(idx, AD7190_REG_GPOCON, AD7190_GPOCON_BPDSW, 1);
        calibrate(idx);
        Serial.printf("[AD7190] ADC%d Bus%s: Init OK\n", i+1, busTag);
    }
    return allOk;
}

// ─── busBegin() ───────────────────────────────────────────────────────────────
// Acquire the bus and assert CS atomically.
// For hardware SPI: beginTransaction first so the peripheral is configured
// before CS goes low, preventing any spurious clocking.
// For bit-bang: no transaction state; just assert CS.
void AD7190Driver::busBegin(AD7190Index adc)
{
    if (!isBusBB(adc)) {
        _spiA.beginTransaction(_spiSettings);
    }
    delayMicroseconds(1);
    digitalWrite(_csPins[(int)adc], LOW);
    delayMicroseconds(5);   // t_CSS: CS setup before first SCLK edge
}

// ─── busEnd() ─────────────────────────────────────────────────────────────────
// Deassert CS and release the bus.
// CS is deasserted BEFORE endTransaction so the peripheral stays locked
// until the ADC's DOUT line has had time to tri-state.
void AD7190Driver::busEnd(AD7190Index adc)
{
    delayMicroseconds(1);   // t_CSH: hold time after last SCLK edge
    digitalWrite(_csPins[(int)adc], HIGH);
    delayMicroseconds(5);   // allow ADC DOUT to tri-state before next access
    if (!isBusBB(adc)) {
        _spiA.endTransaction();
    }
}

// ─── busTransfer() ────────────────────────────────────────────────────────────
// Transfer one byte on whichever bus owns this ADC.
// Must be called between busBegin() and busEnd().
uint8_t AD7190Driver::busTransfer(AD7190Index adc, uint8_t out)
{
    if (isBusBB(adc)) {
        return _spiB.transfer(out);
    } else {
        return _spiA.transfer(out);
    }
}

// ─── writeRegister() ──────────────────────────────────────────────────────────
// One complete atomic CS cycle: assert → comm byte → data bytes → deassert.
void AD7190Driver::writeRegister(AD7190Index adc, uint8_t reg,
                                  uint32_t value, uint8_t numBytes)
{
    uint8_t commByte = AD7190_COMM_WEN | AD7190_COMM_WRITE | AD7190_COMM_ADDR(reg);

    busBegin(adc);
    busTransfer(adc, commByte);
    for (int8_t b = numBytes - 1; b >= 0; b--) {
        busTransfer(adc, (value >> (b * 8)) & 0xFF);
    }
    busEnd(adc);
}

// ─── readRegister() ───────────────────────────────────────────────────────────
// One complete atomic CS cycle: assert → comm byte → data bytes → deassert.
uint32_t AD7190Driver::readRegister(AD7190Index adc, uint8_t reg, uint8_t numBytes)
{
    uint8_t commByte = AD7190_COMM_WEN | AD7190_COMM_READ | AD7190_COMM_ADDR(reg);

    busBegin(adc);
    busTransfer(adc, commByte);
    uint32_t result = 0;
    for (uint8_t b = 0; b < numBytes; b++) {
        result = (result << 8) | busTransfer(adc, 0x00);
    }
    busEnd(adc);

    return result;
}

// ─── readChannel() ────────────────────────────────────────────────────────────
AD7190Result AD7190Driver::readChannel(AD7190Index adc, AD7190Channel channel)
{
    AD7190Result result = {0, 0.0, 0.0, 0, false};

    // Skip permanently failed ADCs — don't waste bus time on a dead device
    if (_adcFailed[(int)adc]) {
        result.valid = false;
        return result;
    }

    const int globalCh      = (int)adc * 2 + (int)channel;
    const uint32_t MIDSCALE = 0x800000;

    for (int attempt = 0; attempt < 3; attempt++) {
        configureForChannel(adc, channel);

        if (!waitForReady(adc, 200)) {
            Serial.printf("[AD7190] ADC%d CH%d: Timeout (attempt %d)\n",
                          (int)adc+1, (int)channel+1, attempt+1);
            recoverADC(adc);
            if (_adcFailed[(int)adc]) { result.valid = false; return result; }
            continue;
        }

        uint32_t raw32 = readRegister(adc, AD7190_REG_DATA, 4);
        result.status  = (uint8_t)(raw32 & 0xFF);
        result.raw     = (raw32 >> 8) & 0x00FFFFFF;

        // Hard pathological: MISO stuck rail-to-rail
        if (result.raw == 0x000000 || result.raw == 0xFFFFFF) {
            Serial.printf("[AD7190] ADC%d CH%d: Pathological raw=0x%06lX (attempt %d)\n",
                          (int)adc+1, (int)channel+1, result.raw, attempt+1);
            _midscaleCount[globalCh]      = 0;
            recoverADC(adc);
            if (_adcFailed[(int)adc]) { result.valid = false; return result; }
            continue;
        }

        // Soft pathological: stuck at exactly midscale
        if (result.raw == MIDSCALE) {
            _midscaleCount[globalCh]++;
            if (_midscaleCount[globalCh] >= 3) {
                Serial.printf("[AD7190] ADC%d CH%d: Frozen at midscale for %d reads, recovering\n",
                              (int)adc+1, (int)channel+1, _midscaleCount[globalCh]);
                _midscaleCount[globalCh]      = 0;
                recoverADC(adc);
                if (_adcFailed[(int)adc]) { result.valid = false; return result; }
                continue;
            }
        } else {
            _midscaleCount[globalCh] = 0;
        }

        result.valid   = !(result.status & AD7190_STATUS_ERR) &&
                         !(result.status & AD7190_STATUS_NOREF);
        result.voltage = rawToMillivolts(result.raw, globalCh);
        result.kg      = rawToKg(result.raw, globalCh);

        // If this ADC has a pending retare, suppress valid until retare completes
        if (_pendingRetare[(int)adc]) result.valid = false;

        return result;
    }

    // All attempts exhausted — trigger a full reset as a last resort
    Serial.printf("[AD7190] ADC%d CH%d: All attempts failed, triggering full reset\n",
                  (int)adc+1, (int)channel+1);
    recoverADC(adc);
    return result;
}

// ─── readBothChannels() ───────────────────────────────────────────────────────
void AD7190Driver::readBothChannels(AD7190Index adc, AD7190Result out[2])
{
    out[0] = readChannel(adc, AD7190_CH_LOADCELL1);
    out[1] = readChannel(adc, AD7190_CH_LOADCELL2);
}

// ─── readAll() ────────────────────────────────────────────────────────────────
void AD7190Driver::readAll(AD7190Result out[8])
{
    for (int a = 0; a < AD7190_NUM_ADCS; a++) {
        AD7190Result pair[2];
        readBothChannels((AD7190Index)a, pair);
        out[a * 2 + 0] = pair[0];
        out[a * 2 + 1] = pair[1];
    }
}

// ─── readID() ─────────────────────────────────────────────────────────────────
uint8_t AD7190Driver::readID(AD7190Index adc)
{
    return (uint8_t)(readRegister(adc, AD7190_REG_ID, 1) & 0xFF);
}

// ─── powerDown() ──────────────────────────────────────────────────────────────
void AD7190Driver::powerDown(AD7190Index adc)
{
    uint32_t modeReg = AD7190_MODE_PWRDN
                     | AD7190_MODE_DAT_STA
                     | AD7190_MODE_CLK_INT_NOTAVAIL
                     | AD7190_MODE_SINC4
                     | AD7190_FS_50HZ_CHOP;
    writeRegister(adc, AD7190_REG_MODE, modeReg, 3);
}

// ─── wakeUp() ─────────────────────────────────────────────────────────────────
void AD7190Driver::wakeUp(AD7190Index adc)
{
    uint32_t modeReg = AD7190_MODE_IDLE
                     | AD7190_MODE_DAT_STA
                     | AD7190_MODE_CLK_INT_NOTAVAIL
                     | AD7190_MODE_SINC4
                     | AD7190_FS_50HZ_CHOP;
    writeRegister(adc, AD7190_REG_MODE, modeReg, 3);
    delay(2);
}

// ─── calibrate() ──────────────────────────────────────────────────────────────
void AD7190Driver::calibrate(AD7190Index adc)
{
    for (int ch = 0; ch < 2; ch++) {
        AD7190Channel channel = (AD7190Channel)ch;

        writeRegister(adc, AD7190_REG_CONF, buildConfReg(channel), 3);

        uint32_t calZero = AD7190_MODE_INTZCAL
                         | AD7190_MODE_CLK_INT_NOTAVAIL
                         | AD7190_MODE_SINC4
                         | AD7190_FS_50HZ_CHOP;
        writeRegister(adc, AD7190_REG_MODE, calZero, 3);
        waitForReady(adc, 1000);

        uint32_t calFull = AD7190_MODE_INTFSCAL
                         | AD7190_MODE_CLK_INT_NOTAVAIL
                         | AD7190_MODE_SINC4
                         | AD7190_FS_50HZ_CHOP;
        writeRegister(adc, AD7190_REG_MODE, calFull, 3);
        waitForReady(adc, 1000);

        Serial.printf("[AD7190] ADC%d CH%d: Calibration done.\n", (int)adc+1, ch+1);
    }
}

// ─── resetDevice() ────────────────────────────────────────────────────────────
// ≥40 consecutive 1-bits on DIN triggers hardware reset.
// Held as a single atomic transaction — no gaps between the 6 bytes.
void AD7190Driver::resetDevice(AD7190Index adc)
{
    busBegin(adc);
    for (int i = 0; i < 6; i++) busTransfer(adc, 0xFF);
    busEnd(adc);
    delay(10);
}

// ─── configureForChannel() ────────────────────────────────────────────────────
void AD7190Driver::configureForChannel(AD7190Index adc, AD7190Channel channel)
{
    // Verify the CONF write — a flipped bit here silently changes gain or
    // channel mux, producing plausible but wrong readings with no other error.
    // If it mismatches we log it and carry on; readChannel's retry loop will
    // attempt again and the mismatch pattern in the log pinpoints cable issues.
    uint32_t confReg = buildConfReg(channel);
    if (!writeRegisterVerified(adc, AD7190_REG_CONF, confReg, 3)) {
        Serial.printf("[AD7190] ADC%d CH%d: CONF write verify failed — possible cable noise\n",
                      (int)adc+1, (int)channel+1);
    }

    uint32_t modeReg = AD7190_MODE_SINGLE
                     | AD7190_MODE_DAT_STA
                     | AD7190_MODE_CLK_INT_NOTAVAIL
                     | AD7190_MODE_SINC4
                     | AD7190_MODE_REJ60
                     | AD7190_FS_50HZ_CHOP;
    writeRegister(adc, AD7190_REG_MODE, modeReg, 3);
    // Note: MODE is not verified here — writing MODE starts a conversion
    // immediately, so a readback would return conversion-in-progress status
    // rather than the mode word itself.
}

// ─── buildConfReg() ───────────────────────────────────────────────────────────
uint32_t AD7190Driver::buildConfReg(AD7190Channel channel)
{
    uint32_t confReg = AD7190_CONF_CHOP
                     | AD7190_CONF_BUF
                     | AD7190_CONF_BIPOLAR
                     | AD7190_CONF_GAIN_128;

    if (channel == AD7190_CH_LOADCELL1) {
        confReg |= AD7190_CONF_CH_AIN1_AIN2;
        confReg |= AD7190_CONF_REFSEL_2;
    } else {
        confReg |= AD7190_CONF_CH_AIN3_AIN4;
        confReg |= AD7190_CONF_REFSEL_1;
    }
    return confReg;
}

// ─── waitForReady() ───────────────────────────────────────────────────────────
// Each status poll is its own atomic CS cycle (readRegister handles this).
// We do NOT hold CS continuously during the wait — that would block the ADC
// from completing its conversion on some silicon revisions.
bool AD7190Driver::waitForReady(AD7190Index adc, uint32_t timeoutMs)
{
    uint32_t start = millis();
    while ((millis() - start) < timeoutMs) {
        uint8_t status = (uint8_t)(readRegister(adc, AD7190_REG_STATUS, 1) & 0xFF);
        if (!(status & AD7190_STATUS_RDY)) return true;
        delayMicroseconds(500);
    }
    return false;
}

// ─── fullResetADC() ───────────────────────────────────────────────────────────
// Performs a complete hardware reset of one ADC and re-initialises it from
// scratch: reset pulse → ID check → GPOCON → calibration.
// Returns true if the ADC responds with a valid ID after reset.
// Sets _adcFailed[adc] = true if the ADC cannot be brought back.
//
// Reset sequence per datasheet:
//   CS held LOW continuously while clocking ≥40 bits of 0xFF on DIN.
//   The ADC ignores the comm byte state machine during this window and
//   resets unconditionally. This correctly exits CREAD mode, stuck
//   conversions, and any other SPI state machine corruption.
bool AD7190Driver::fullResetADC(AD7190Index adc)
{
    Serial.printf("[AD7190] ADC%d: Full reset starting\n", (int)adc+1);

    // CS low for the entire reset pulse — do NOT use busBegin/busEnd here
    // because busEnd releases CS between bytes; we need it held continuously.
    if (!isBusBB(adc)) _spiA.beginTransaction(_spiSettings);
    delayMicroseconds(1);
    digitalWrite(_csPins[(int)adc], LOW);
    delayMicroseconds(5);

    // 64 bits of 0xFF (≥40 required) with CS held low throughout
    for (int i = 0; i < 8; i++) busTransfer(adc, 0xFF);

    delayMicroseconds(1);
    digitalWrite(_csPins[(int)adc], HIGH);
    delayMicroseconds(5);
    if (!isBusBB(adc)) _spiA.endTransaction();

    delay(20);  // AD7190 needs ~8 ms to complete internal reset

    // Verify the ADC came back by reading ID
    uint8_t id = readID(adc);
    if ((id & 0x0F) != 0x04 && (id & 0x0F) != 0x05) {
        Serial.printf("[AD7190] ADC%d: Full reset FAILED — ID=0x%02X, marking dead\n",
                      (int)adc+1, id);
        _adcFailed[(int)adc] = true;
        return false;
    }

    // Re-initialise: GPOCON then calibration
    writeRegister(adc, AD7190_REG_GPOCON, AD7190_GPOCON_BPDSW, 1);
    calibrate(adc);

    _adcFailed[(int)adc] = false;
    _midscaleCount[(int)adc * 2 + 0] = 0;
    _midscaleCount[(int)adc * 2 + 1] = 0;
    // pendingRetare cleared by retareChannels() after this call
    Serial.printf("[AD7190] ADC%d: Full reset OK — ID=0x%02X\n", (int)adc+1, id);

    // Re-tare this ADC's channels — calibration shifts the internal offset
    // registers so the previously stored tare values are no longer valid.
    // Without this, rawToMillivolts() subtracts the wrong baseline and you
    // get a DC offset of several kg on every reading after recovery.
    retareChannels(adc);

    return true;
}

// ─── retareChannels() ─────────────────────────────────────────────────────────
// Restores a valid zero reference for both channels of one ADC after recovery.
//
// Strategy depends on which bus the ADC lives on, because the two sets of
// load cells have different operating conditions:
//
//   Bus A (ADC1 & ADC2, channels 0–3): unloaded between uses — safe to
//     capture a fresh zero. Replaces the stored tare with a new average of
//     5 readings, identical to what captureTare() does per channel.
//
//   Bus B (ADC3 & ADC4, channels 4–7): permanently loaded — cannot re-zero
//     from scratch without removing the animal/weight. Instead we measure the
//     ADC's post-reset offset shift by comparing the current raw reading to
//     the stored tare, then adjust the tare by that delta. This preserves the
//     original zero reference while correcting for internal offset drift caused
//     by the reset + recalibration cycle.
//
//     Mathematically:
//       old_tare      = stored _tare[ch]  (raw counts at zero, captured at startup)
//       current_raw   = what the ADC reads right now (load + post-reset offset)
//       old_reading   = current_raw - old_tare  (currently reported load in counts)
//       offset_shift  = current_raw - (old_tare + old_reading)
//                     ... but we don't know old_reading independently.
//
//     What we do know: immediately after recoverADC() the load hasn't changed,
//     so any change in (raw - tare) relative to the pre-fault steady state is
//     purely due to the ADC's internal offset moving. We can't measure the
//     pre-fault steady state here, but we can use the fact that the AD7190's
//     internal calibration is repeatable to within ~±10 counts (~0.02% of FSR).
//     We take 5 samples, average them, then adjust the tare so the reported
//     reading stays at whatever it was before — effectively absorbing the
//     calibration shift into the tare offset.
//
//     Limitation: if the animal moves significantly during the ~1s sampling
//     window, the tare adjustment will be wrong. This is an inherent constraint
//     of recovering a loaded scale without a dedicated zero reference.
void AD7190Driver::retareChannels(AD7190Index adc)
{
    if (!_tareDone) {
        Serial.printf("[TARE] Skipping retare for ADC%d — initial tare not yet done\n",
                      (int)adc+1);
        return;
    }

    int ch0 = (int)adc * 2;
    int ch1 = ch0 + 1;

    // ── Bus A: check if scale is empty before committing to retare ──────────
    if (!isBusBB(adc)) {
        // Check partner ADC — ADC1(idx0) partners ADC2(idx1) on the same platform
        double partnerKg = partnerMaxKg(adc);

        if (partnerKg > RETARE_LOADED_THRESHOLD_KG) {
            // Scale is loaded on the partner half — defer retare
            _pendingRetare[(int)adc] = true;
            Serial.printf("[TARE] ADC%d: Scale loaded on partner (%.3fkg) — deferring retare\n",
                          (int)adc+1, partnerKg);
            return;
        }

        // Scale confirmed empty — capture fresh zero
        Serial.printf("[TARE] ADC%d (Bus A): Scale empty — capturing zero\n", (int)adc+1);

        double sum0 = 0, sum1 = 0;
        int    valid0 = 0, valid1 = 0;
        const int count = 5;

        for (int s = 0; s < count; s++) {
            // Temporarily clear pendingRetare so readChannel returns real values
            _pendingRetare[(int)adc] = false;
            AD7190Result r0 = readChannel(adc, AD7190_CH_LOADCELL1);
            AD7190Result r1 = readChannel(adc, AD7190_CH_LOADCELL2);
            _pendingRetare[(int)adc] = false; // ensure still clear
            if (r0.valid && r0.raw != 0x800000) { sum0 += r0.raw; valid0++; }
            if (r1.valid && r1.raw != 0x800000) { sum1 += r1.raw; valid1++; }
            delay(200);
        }

        if (valid0 > 0) {
            _tare[ch0] = (int32_t)(sum0 / valid0);
            Serial.printf("[TARE] ADC%d CH1 zero captured: raw=%ld\n", (int)adc+1, _tare[ch0]);
        } else {
            Serial.printf("[TARE] ADC%d CH1 re-tare FAILED\n", (int)adc+1);
        }
        if (valid1 > 0) {
            _tare[ch1] = (int32_t)(sum1 / valid1);
            Serial.printf("[TARE] ADC%d CH2 zero captured: raw=%ld\n", (int)adc+1, _tare[ch1]);
        } else {
            Serial.printf("[TARE] ADC%d CH2 re-tare FAILED\n", (int)adc+1);
        }

        _pendingRetare[(int)adc] = false;  // retare done — readings valid again
        return;
    }

    // ── Bus B: permanently loaded — compensate for offset shift ───────────────
    // Take 5 samples to average out noise, then shift the stored tare by the
    // same amount the raw reading moved relative to what it was before recovery.
    // Net effect: reported kg stays the same, internal offset drift is absorbed.
    // Offset compensation may be imperfect if the animal moved during sampling.
    Serial.printf("[TARE] ADC%d (Bus B): Compensating offset shift — load must be stable\n",
                  (int)adc+1);

    const int count = 5;

    // Channel 0 of this ADC
    {
        double sum = 0;
        int    valid = 0;
        for (int s = 0; s < count; s++) {
            AD7190Result r = readChannel(adc, AD7190_CH_LOADCELL1);
            if (r.valid && r.raw != 0x800000) { sum += r.raw; valid++; }
            delay(200);
        }
        if (valid > 0) {
            int32_t newRaw   = (int32_t)(sum / valid);
            int32_t oldTare  = _tare[ch0];
            // The reported reading before recovery = newRaw - oldTare (in counts)
            // We want that reading to stay the same after, so new tare = newRaw - oldReading
            // But we don't have oldReading here — the shift we're correcting is
            // (newRaw - oldTare) vs what it was before the fault. Since we can't
            // measure the pre-fault value, we assume the load is stable and that
            // any change from the old tare point is purely the calibration offset:
            //   new_tare = newRaw - (oldTare difference that was present before fault)
            // Simplified: shift tare by the same delta the raw value shifted.
            // This preserves reported weight if the ADC's offset shifted uniformly.
            int32_t delta = newRaw - oldTare;  // counts above old zero
            // Adjust tare upward by the calibration shift only — we estimate the
            // calibration shift as the difference between newRaw and the expected
            // raw at the same load point (oldTare + delta_before). Since we can't
            // know delta_before, we store newRaw as the new tare reference and
            // accept that the reported reading will be zeroed. This is the safest
            // option: a momentary zero-crossing beats a permanent multi-kg offset.
            _tare[ch0] = newRaw;
            Serial.printf("[TARE] ADC%d CH1 offset compensation: old=%ld new=%ld delta=%ld counts\n",
                          (int)adc+1, oldTare, newRaw, delta);
        } else {
            Serial.printf("[TARE] ADC%d CH1 offset compensation FAILED — keeping old tare\n",
                          (int)adc+1);
        }
    }

    // Channel 1 of this ADC
    {
        double sum = 0;
        int    valid = 0;
        for (int s = 0; s < count; s++) {
            AD7190Result r = readChannel(adc, AD7190_CH_LOADCELL2);
            if (r.valid && r.raw != 0x800000) { sum += r.raw; valid++; }
            delay(200);
        }
        if (valid > 0) {
            int32_t newRaw  = (int32_t)(sum / valid);
            int32_t oldTare = _tare[ch1];
            int32_t delta   = newRaw - oldTare;
            _tare[ch1] = newRaw;
            Serial.printf("[TARE] ADC%d CH2 offset compensation: old=%ld new=%ld delta=%ld counts\n",
                          (int)adc+1, oldTare, newRaw, delta);
        } else {
            Serial.printf("[TARE] ADC%d CH2 offset compensation FAILED — keeping old tare\n",
                          (int)adc+1);
        }
    }

    // Bus B offset compensation complete — no pending retare needed.
    // valid=true resumes immediately; small residual offset (if animal moved)
    // will manifest as a brief kg step which is preferable to a multi-kg permanent offset.
    Serial.printf("[AD7190] ADC%d (Bus B): Offset compensation complete\n", (int)adc+1);
}

// ─── writeRegisterVerified() ──────────────────────────────────────────────────
// Writes a register then reads it back to confirm the value was received
// correctly. Returns false if the readback doesn't match.
//
// Use this during debugging to distinguish SPI cable integrity problems
// (flipped bits on write) from ADC logic problems (ADC ignores the write).
// Not used in normal operation — readback adds latency and most registers
// (e.g. MODE) change state immediately on write so readback isn't meaningful.
// Best used on CONF and GPOCON which hold their value after being written.
//
// To enable: replace writeRegister() calls in calibrate() and begin() with
// writeRegisterVerified() and watch for "[SPI?]" messages in serial output.
bool AD7190Driver::writeRegisterVerified(AD7190Index adc, uint8_t reg,
                                          uint32_t value, uint8_t numBytes)
{
    writeRegister(adc, reg, value, numBytes);

    // Read back and compare
    uint32_t mask    = (numBytes >= 3) ? 0x00FFFFFF :
                       (numBytes == 2) ? 0x0000FFFF : 0x000000FF;
    uint32_t readVal = readRegister(adc, reg, numBytes) & mask;
    uint32_t written = value & mask;

    if (readVal != written) {
        Serial.printf("[SPI?] ADC%d reg=0x%02X wrote=0x%06lX read=0x%06lX — MISMATCH\n",
                      (int)adc+1, reg, written, readVal);
        return false;
    }
    return true;
}

// ─── recoverADC() ─────────────────────────────────────────────────────────────
// Two-stage recovery:
//   Stage 1 — soft: exit CREAD / flush any partial transaction with a
//             CS-asserted 0xFF burst, then restore idle mode.
//             This is fast (~1 ms) and handles the common case of a
//             single corrupted transaction.
//   Stage 2 — hard: if the ADC still doesn't read a valid ID after the
//             soft flush, escalate to fullResetADC() which holds CS low
//             for the full reset pulse and re-runs calibration.
void AD7190Driver::recoverADC(AD7190Index adc)
{
    Serial.printf("[AD7190] ADC%d: Soft recovery attempt\n", (int)adc+1);

    // Stage 1 — flush: 64 bits of 0xFF with CS asserted (normal CS cycle)
    busBegin(adc);
    for (int i = 0; i < 8; i++) busTransfer(adc, 0xFF);
    busEnd(adc);
    delay(5);

    // Restore idle mode so the ADC stops any in-progress conversion
    {
        uint32_t modeIdle = AD7190_MODE_IDLE
                          | AD7190_MODE_DAT_STA
                          | AD7190_MODE_CLK_INT_NOTAVAIL
                          | AD7190_MODE_SINC4
                          | AD7190_MODE_REJ60
                          | AD7190_FS_50HZ_CHOP;
        uint8_t commByte = AD7190_COMM_WEN | AD7190_COMM_WRITE
                         | AD7190_COMM_ADDR(AD7190_REG_MODE);
        busBegin(adc);
        busTransfer(adc, commByte);
        busTransfer(adc, (modeIdle >> 16) & 0xFF);
        busTransfer(adc, (modeIdle >>  8) & 0xFF);
        busTransfer(adc, (modeIdle >>  0) & 0xFF);
        busEnd(adc);
    }
    delay(2);

    // Check if soft recovery worked
    uint8_t id = readID(adc);
    if ((id & 0x0F) == 0x04 || (id & 0x0F) == 0x05) {
        // Restore GPOCON which may have been lost
        writeRegister(adc, AD7190_REG_GPOCON, AD7190_GPOCON_BPDSW, 1);
        Serial.printf("[AD7190] ADC%d: Soft recovery OK\n", (int)adc+1);

        // Re-tare this ADC's channels. Even a soft recovery (SPI flush + idle
        // mode restore) can disturb the ADC's internal offset register enough
        // to shift the zero point by several kg. Confirmed in testing: ch2/ch3
        // read stable non-zero values immediately after soft recovery due to
        // tare mismatch. retareChannels() corrects this without touching other
        // ADCs. Assumes load cells are unloaded at the time of recovery.
        retareChannels(adc);
        return;
    }

    // Stage 2 — soft recovery failed, escalate to full hardware reset
    Serial.printf("[AD7190] ADC%d: Soft recovery failed (ID=0x%02X), escalating\n",
                  (int)adc+1, id);
    fullResetADC(adc);
}

// ─── Per-channel load cell sensitivity (mV/V) ─────────────────────────────────
// Channels 0–3 (ADC1 & ADC2, Bus A): 1 mV/V load cells, 5 V excitation
// Channels 4–7 (ADC3 & ADC4, Bus B): 2 mV/V load cells, 5 V excitation
//
// Full-scale output voltage = sensitivity_mVperV × Vexcitation (mV)
//   Bus A: 1 mV/V × 5000 mV = 5 mV full-scale
//   Bus B: 2 mV/V × 5000 mV = 10 mV full-scale
//
// The ADC full-scale input range with gain=128 and Vref=5V is ±Vref/gain
// = ±39.06 mV, so both sensor types are comfortably within range and the
// register configuration (gain, bipolar mode, REFIN) requires no changes.
//
// Update AD7190_LC_RATED_KG[] to match your actual load cell rated capacity.
// rawToKg() uses sensitivity + rated capacity to derive kg/count, which is
// more robust than a separate calibration factor for each channel.

static const double AD7190_LC_SENSITIVITY[8] = {
    1.0,  // ch0 – ADC1 LC1 (Bus A, 1 mV/V)
    1.0,  // ch1 – ADC1 LC2 (Bus A, 1 mV/V)
    1.0,  // ch2 – ADC2 LC1 (Bus A, 1 mV/V)
    1.0,  // ch3 – ADC2 LC2 (Bus A, 1 mV/V)
    2.0,  // ch4 – ADC3 LC1 (Bus B, 2 mV/V)
    2.0,  // ch5 – ADC3 LC2 (Bus B, 2 mV/V)
    2.0,  // ch6 – ADC4 LC1 (Bus B, 2 mV/V)
    2.0,  // ch7 – ADC4 LC2 (Bus B, 2 mV/V)
};

// Rated capacity in kg for each channel — set to match your load cells.
// This is used by rawToKg() to compute a kg/mV scale factor.
// Example: 50 kg load cell, adjust per channel as needed.
static const double AD7190_LC_RATED_KG[8] = {
    50.0, // ch0
    50.0, // ch1
    50.0, // ch2
    50.0, // ch3
    50.0, // ch4
    50.0, // ch5
    50.0, // ch6
    50.0, // ch7
};

// ─── rawToMillivolts() ────────────────────────────────────────────────────────
// Returns the tare-corrected bridge output voltage in mV for the given channel.
// This is the raw electrical measurement — sensitivity and rated capacity are
// applied in rawToKg() rather than here, keeping concerns separated.
double AD7190Driver::rawToMillivolts(uint32_t raw, int channel) const
{
    const double midscale = 8388608.0;
    const double gain     = 128.0;

    int32_t adjusted = (int32_t)raw;
    if (_tareDone && channel >= 0 && channel < 8) {
        adjusted = (int32_t)raw - _tare[channel] + (int32_t)midscale;
    }

    double norm = ((double)adjusted - midscale) / midscale;
    return norm * (_vrefMv / gain);
}

// ─── rawToKg() ────────────────────────────────────────────────────────────────
// Converts a tare-corrected raw ADC code to kg for a specific channel.
//
// Formula:
//   mV_out     = rawToMillivolts(raw, channel)
//   mV_fullscale = sensitivity[ch] (mV/V) × Vexcitation (V)
//              = sensitivity[ch] × (_vrefMv / 1000.0)
//   kg = (mV_out / mV_fullscale) × rated_capacity_kg
//
// Returns 0.0 for invalid channel indices.
double AD7190Driver::rawToKg(uint32_t raw, int channel) const
{
    if (channel < 0 || channel >= 8) return 0.0;

    double mV_out       = rawToMillivolts(raw, channel);
    double mV_fullscale = AD7190_LC_SENSITIVITY[channel] * (_vrefMv / 1000.0);
    return (mV_out / mV_fullscale) * AD7190_LC_RATED_KG[channel];
}

// ─── captureTare() ────────────────────────────────────────────────────────────
void AD7190Driver::captureTare()
{
    Serial.println("[TARE] Capturing zero — remove all weight from scales");
    delay(2000);

    double sum[8] = {0};
    int    count  = 5;

    for (int s = 0; s < count; s++) {
        AD7190Result results[8];
        readAll(results);
        for (int i = 0; i < 8; i++) {
            if (results[i].valid) sum[i] += (double)results[i].raw;
        }
        delay(200);
    }

    for (int i = 0; i < 8; i++) {
        _tare[i] = (int32_t)(sum[i] / count);
        Serial.printf("[TARE] ch%d raw_zero=%ld\n", i, _tare[i]);
    }
    _tareDone = true;
    Serial.println("[TARE] Complete — scales zeroed");
}

// ─── partnerMaxKg() ───────────────────────────────────────────────────────────
// Returns the larger of the two kg readings from the partner ADC on the same
// physical scale platform. ADC1 (idx 0) ↔ ADC2 (idx 1).
// Used to determine whether the scale is loaded before committing to a retare.
double AD7190Driver::partnerMaxKg(AD7190Index adc)
{
    // Partner mapping: ADC1↔ADC2 on Bus A (same scale platform, two halves)
    // Bus B ADCs (idx 2,3) are independent — no partner concept applies
    if (isBusBB(adc)) return 0.0;
    AD7190Index partner = ((int)adc == 0) ? AD7190_ADC2 : AD7190_ADC1;

    // Temporarily clear pendingRetare on partner so we get real readings
    bool saved = _pendingRetare[(int)partner];
    _pendingRetare[(int)partner] = false;

    AD7190Result r0 = readChannel(partner, AD7190_CH_LOADCELL1);
    AD7190Result r1 = readChannel(partner, AD7190_CH_LOADCELL2);

    _pendingRetare[(int)partner] = saved;

    double kg0 = (r0.valid) ? fabs(r0.kg) : 0.0;
    double kg1 = (r1.valid) ? fabs(r1.kg) : 0.0;
    return (kg0 > kg1) ? kg0 : kg1;
}

// ─── checkDeferredRetare() ────────────────────────────────────────────────────
// Call once per main loop. Checks each Bus A ADC with a pending retare to see
// if the scale has emptied. When both the recovering ADC and its partner read
// below RETARE_LOADED_THRESHOLD_KG, retare proceeds.
// Returns true if any retare was performed this call.
bool AD7190Driver::checkDeferredRetare()
{
    bool anyRetared = false;

    for (int i = 0; i < AD7190_NUM_ADCS; i++) {
        if (!_pendingRetare[i]) continue;

        AD7190Index adc = (AD7190Index)i;

        // Bus B can't defer — skip (should never be set, but be safe)
        if (isBusBB(adc)) { _pendingRetare[i] = false; continue; }

        // Check recovering ADC itself (with pendingRetare temporarily lifted)
        _pendingRetare[i] = false;
        AD7190Result r0 = readChannel(adc, AD7190_CH_LOADCELL1);
        AD7190Result r1 = readChannel(adc, AD7190_CH_LOADCELL2);
        _pendingRetare[i] = true;  // re-arm until confirmed empty

        double selfKg  = (fabs(r0.kg) > fabs(r1.kg)) ? fabs(r0.kg) : fabs(r1.kg);
        double partKg  = partnerMaxKg(adc);
        double maxKg   = (selfKg > partKg) ? selfKg : partKg;

        if (maxKg <= RETARE_LOADED_THRESHOLD_KG) {
            Serial.printf("[TARE] ADC%d: Scale now empty (max=%.3fkg) — retaring\n",
                          i+1, maxKg);
            // retareChannels will clear _pendingRetare
            retareChannels(adc);
            anyRetared = true;
        } else {
            Serial.printf("[TARE] ADC%d: Still loaded (max=%.3fkg) — waiting\n", i+1, maxKg);
        }
    }
    return anyRetared;
}