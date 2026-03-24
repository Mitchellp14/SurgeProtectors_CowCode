/**
 * weigh_system.cpp
 * ============================================================
 * Implementation — see weigh_system.h for full documentation.
 * ============================================================
 */

#include "loadCells.h"

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

// Hardware SPI bus object (Bus A — FSPI/SPI2 on ESP32-C6)
static SPIClass  *_busA = nullptr;
static SPISettings _spiSettings(WS_SPI_FREQ, WS_SPI_ORDER, WS_SPI_MODE);

// CS pins in ADC order: [0]=A1(CS1), [1]=A2(CS2), [2]=B1(CS3), [3]=B2(CS4)
static const uint8_t _csPin[4] = { WS_CS1_PIN, WS_CS2_PIN,
                                    WS_CS3_PIN, WS_BUSB_CS4_PIN };

// ---------------------------------------------------------------------------
// Low-level bit-bang SPI helpers for Bus B
// All transactions are MSB-first, Mode 3 (CPOL=1, CPHA=1):
//   Idle clock HIGH, data captured on rising edge, shifted on falling edge.
// ---------------------------------------------------------------------------

static void _bbSpiBegin(uint8_t csPin)
{
    digitalWrite(WS_SCLK_PIN, HIGH);   // idle state for Mode 3
    digitalWrite(csPin, LOW);
    delayMicroseconds(1);
}

static void _bbSpiEnd(uint8_t csPin)
{
    delayMicroseconds(1);
    digitalWrite(csPin, HIGH);
}

/**
 * Transfer one byte over bit-bang bus B.
 * Returns the byte simultaneously clocked in from MISO.
 */
static uint8_t _bbSpiTransferByte(uint8_t out)
{
    uint8_t in = 0;
    for (int i = 7; i >= 0; --i) {
        // Set MOSI *before* rising edge (Mode 3: shift on falling, sample on rising)
        // Clock is currently HIGH (idle).
        digitalWrite(WS_SCLK_PIN, LOW);                            // falling edge → slave shifts out
        digitalWrite(WS_BUSB_MOSI, (out >> i) & 0x01 ? HIGH : LOW);
        delayMicroseconds(1);
        digitalWrite(WS_SCLK_PIN, HIGH);                           // rising edge  → master samples
        if (digitalRead(WS_BUSB_MISO)) in |= (1u << i);
        delayMicroseconds(1);
    }
    return in;
}

/**
 * Write N bytes to a Bus B device; ignore incoming data.
 */
static void _bbSpiWrite(uint8_t csPin, const uint8_t *buf, size_t len)
{
    _bbSpiBegin(csPin);
    for (size_t i = 0; i < len; ++i) _bbSpiTransferByte(buf[i]);
    _bbSpiEnd(csPin);
}

/**
 * Write cmdBuf (cmdLen bytes) then read rdLen bytes into rdBuf.
 * CS held low for the whole transaction.
 */
static void _bbSpiWriteRead(uint8_t csPin,
                             const uint8_t *cmdBuf, size_t cmdLen,
                             uint8_t *rdBuf,  size_t rdLen)
{
    _bbSpiBegin(csPin);
    for (size_t i = 0; i < cmdLen; ++i) _bbSpiTransferByte(cmdBuf[i]);
    for (size_t i = 0; i < rdLen;  ++i) rdBuf[i] = _bbSpiTransferByte(0xFF);
    _bbSpiEnd(csPin);
}

// ---------------------------------------------------------------------------
// AD7190 register-level helpers
// (Abstracted so the same logic works for both hardware and bit-bang buses.)
// ---------------------------------------------------------------------------

/**
 * Perform a full reset of one AD7190 (write 40 × 1-bits over DIN).
 * Works for both buses.
 */
static void _adcReset(bool busB, uint8_t csPin)
{
    static const uint8_t ones[5] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    if (busB) {
        _bbSpiWrite(csPin, ones, 5);
    } else {
        _busA->beginTransaction(_spiSettings);
        digitalWrite(csPin, LOW);
        for (int i = 0; i < 5; ++i) _busA->transfer(0xFF);
        digitalWrite(csPin, HIGH);
        _busA->endTransaction();
    }
    delay(2);   // allow internal reset sequence (~500 µs typ.)
}

/**
 * Write a 24-bit register value.
 *
 * AD7190 communications register byte format:
 *   Bit 7  : WEN (write enable, always 0)
 *   Bit 6  : R/W (0=write, 1=read)
 *   Bits 5-3: Register address
 *   Bits 2-0: Count (0 for single, but ignored in byte writes)
 *
 * For a 3-byte (24-bit) register write, send comm byte then 3 data bytes.
 */
static void _adcWriteReg(bool busB, uint8_t csPin,
                          uint8_t regAddr, uint32_t value, uint8_t nBytes)
{
    // Communications register write: WEN=0, R/W=0, ADDR, CREAD=0
    uint8_t comm = (regAddr & 0x07) << 3;
    uint8_t buf[5];
    buf[0] = comm;
    for (int i = nBytes - 1; i >= 0; --i) {
        buf[1 + (nBytes - 1 - i)] = (value >> (8 * i)) & 0xFF;
    }

    if (busB) {
        _bbSpiWrite(csPin, buf, 1 + nBytes);
    } else {
        _busA->beginTransaction(_spiSettings);
        digitalWrite(csPin, LOW);
        for (size_t i = 0; i < 1 + nBytes; ++i) _busA->transfer(buf[i]);
        digitalWrite(csPin, HIGH);
        _busA->endTransaction();
    }
}

/**
 * Read a register.  Returns 0 on timeout.
 */
static uint32_t _adcReadReg(bool busB, uint8_t csPin,
                              uint8_t regAddr, uint8_t nBytes)
{
    // Communications register: WEN=0, R/W=1, ADDR
    uint8_t comm = 0x40 | ((regAddr & 0x07) << 3);

    uint8_t rdBuf[4] = {};
    if (busB) {
        _bbSpiWriteRead(csPin, &comm, 1, rdBuf, nBytes);
    } else {
        _busA->beginTransaction(_spiSettings);
        digitalWrite(csPin, LOW);
        _busA->transfer(comm);
        for (uint8_t i = 0; i < nBytes; ++i) rdBuf[i] = _busA->transfer(0xFF);
        digitalWrite(csPin, HIGH);
        _busA->endTransaction();
    }

    uint32_t result = 0;
    for (uint8_t i = 0; i < nBytes; ++i) result = (result << 8) | rdBuf[i];
    return result;
}

/**
 * Poll DOUT/RDY (low when conversion complete) with a timeout.
 *
 * For Bus A: poll the hardware MISO pin.
 * For Bus B: poll the software MISO pin.
 *
 * Returns true when ready, false on timeout.
 */
static bool _waitReady(bool busB, uint8_t csPin, uint32_t timeoutMs = 500)
{
    uint8_t rdyPin = busB ? WS_BUSB_MISO : WS_BUSA_MISO;
    uint32_t t0 = millis();

    // Pull CS low; the AD7190 drives DOUT/RDY low when ready
    digitalWrite(csPin, LOW);
    while (digitalRead(rdyPin) == HIGH) {
        if ((millis() - t0) > timeoutMs) {
            digitalWrite(csPin, HIGH);
            return false;
        }
        delayMicroseconds(100);
    }
    digitalWrite(csPin, HIGH);
    return true;
}

// ---------------------------------------------------------------------------
// One-chip init sequence
// ---------------------------------------------------------------------------

static bool _initOneADC(bool busB, uint8_t csPin)
{
    // 1. Reset
    _adcReset(busB, csPin);

    // 2. Read ID register (reg 4, 1 byte). Lower nibble must be 0x4 for AD7190.
    //    Library defines: ID_AD7190 = 0x4, AD7190_ID_MASK = 0x0F
    uint32_t id = _adcReadReg(busB, csPin, AD7190_REG_ID, 1);
    if ((id & AD7190_ID_MASK) != ID_AD7190) {
        Serial.printf("[WS] ADC cs=%d: ID check failed (got 0x%02X, want 0x_4)\n",
                      csPin, (uint8_t)id);
        return false;
    }

    // 3. Configure register:
    //    - Bipolar mode: leave AD7190_CONF_UNIPOLAR clear (WS_ADC_POLARITY==0 means bipolar)
    //    - Gain = WS_ADC_GAIN
    //    - Buffer enabled (recommended with external REFIN)
    //    - Start on AIN1(+)/AIN2(-) — channel will be switched before each read
    uint32_t confReg = AD7190_CONF_GAIN(WS_ADC_GAIN)
                     | AD7190_CONF_BUF
                     | AD7190_CONF_CHAN(AD7190_CH_AIN1P_AIN2M);  // channel index 0
    if (WS_ADC_POLARITY != 0) confReg |= AD7190_CONF_UNIPOLAR;
    _adcWriteReg(busB, csPin, AD7190_REG_CONF, confReg, 3);

    // 4. Mode register:
    //    - Internal zero-scale calibration first (improves offset)
    //    - Use internal 4.92 MHz clock
    //    - Data rate filter = WS_ADC_FILTER_RATE
    uint32_t modeReg = AD7190_MODE_SEL(AD7190_MODE_CAL_INT_ZERO)
                     | AD7190_MODE_CLKSRC(AD7190_CLK_INT)
                     | AD7190_MODE_RATE(WS_ADC_FILTER_RATE);
    _adcWriteReg(busB, csPin, AD7190_REG_MODE, modeReg, 3);
    // Wait for calibration to finish (DOUT/RDY goes low)
    if (!_waitReady(busB, csPin, 2000)) {
        Serial.printf("[WS] ADC cs=%d: zero-cal timeout\n", csPin);
        return false;
    }

    // 5. Full-scale calibration
    modeReg = AD7190_MODE_SEL(AD7190_MODE_CAL_INT_FULL)
            | AD7190_MODE_CLKSRC(AD7190_CLK_INT)
            | AD7190_MODE_RATE(WS_ADC_FILTER_RATE);
    _adcWriteReg(busB, csPin, AD7190_REG_MODE, modeReg, 3);
    if (!_waitReady(busB, csPin, 2000)) {
        Serial.printf("[WS] ADC cs=%d: full-scale-cal timeout\n", csPin);
        return false;
    }

    Serial.printf("[WS] ADC cs=%d: init OK (ID=0x%02X)\n", csPin, (uint8_t)id);
    return true;
}

// ---------------------------------------------------------------------------
// Single-channel read helper
// Selects channel, triggers single conversion, waits for RDY, reads data.
// channel: use AD7190_CH_AIN1_AIN2 (AIN0/1) or AD7190_CH_AIN3_AIN4 (AIN2/3)
// refin: which REFIN to use — embedded in the conf register REFSEL bit.
// ---------------------------------------------------------------------------

/**
 * AD7190 Configuration Register REFSEL bit (AD7190_CONF_REFSEL = bit 20):
 *   Bit CLEAR (0) = REFIN1+/REFIN1-
 *   Bit SET   (1) = REFIN2+/REFIN2-
 *
 * AD7190_CONF_REFSEL is already defined by the library header as (1 << 20).
 * We just alias it for readability:
 *
 *   AIN1(+)/AIN2(-) [ch01]  → REFIN2  → set   AD7190_CONF_REFSEL
 *   AIN3(+)/AIN4(-) [ch23]  → REFIN1  → clear AD7190_CONF_REFSEL
 */
#define WS_REFSEL_REFIN2   (AD7190_CONF_REFSEL)   // bit set
#define WS_REFSEL_REFIN1   (0UL)                   // bit clear

static bool _readChannel(bool busB, uint8_t csPin,
                          uint8_t chanIdx, uint32_t refSel,
                          uint32_t *rawOut)
{
    // Build configuration word with selected channel index and reference
    // AD7190_CONF_CHAN(x) shifts the 8-bit channel bitmask into bits [15:8].
    // Channel indices: AD7190_CH_AIN1P_AIN2M=0, AD7190_CH_AIN3P_AIN4M=1
    // The macro expects a bitmask where bit N = enable channel N, so we
    // pass (1 << chanIdx) to select exactly one channel.
    uint32_t confReg = AD7190_CONF_GAIN(WS_ADC_GAIN)
                     | AD7190_CONF_BUF
                     | AD7190_CONF_CHAN(1u << chanIdx)
                     | refSel;
    _adcWriteReg(busB, csPin, AD7190_REG_CONF, confReg, 3);

    // Trigger a single conversion
    uint32_t modeReg = AD7190_MODE_SEL(AD7190_MODE_SINGLE)
                     | AD7190_MODE_CLKSRC(AD7190_CLK_INT)
                     | AD7190_MODE_RATE(WS_ADC_FILTER_RATE);
    _adcWriteReg(busB, csPin, AD7190_REG_MODE, modeReg, 3);

    // Wait for conversion (DOUT/RDY low)
    if (!_waitReady(busB, csPin, 500)) {
        Serial.printf("[WS] cs=%d ch=%d: conversion timeout\n", csPin, chanIdx);
        *rawOut = 0;
        return false;
    }

    // Read data register (3 bytes = 24-bit result)
    *rawOut = _adcReadReg(busB, csPin, AD7190_REG_DATA, 3);
    return true;
}

// ---------------------------------------------------------------------------
// Public API implementation
// ---------------------------------------------------------------------------

bool ws_spi_init(void)
{
    // --- Bus A: hardware FSPI (SPI2) ---
    _busA = new SPIClass(FSPI);
    // begin(SCLK, MISO, MOSI, SS=-1)  — we control CS manually
    _busA->begin(WS_SCLK_PIN, WS_BUSA_MISO, WS_BUSA_MOSI, -1);

    // Configure hardware CS pins as outputs, idle HIGH
    pinMode(WS_CS1_PIN, OUTPUT); digitalWrite(WS_CS1_PIN, HIGH);
    pinMode(WS_CS2_PIN, OUTPUT); digitalWrite(WS_CS2_PIN, HIGH);

    // --- Bus B: software (bit-bang) SPI ---
    // SCLK is shared with Bus A (IO19), already configured by SPIClass::begin()
    // but we set it manually here to ensure direction is correct regardless.
    pinMode(WS_SCLK_PIN,    OUTPUT); digitalWrite(WS_SCLK_PIN, HIGH);
    pinMode(WS_BUSB_MOSI,   OUTPUT); digitalWrite(WS_BUSB_MOSI, HIGH);
    pinMode(WS_BUSB_MISO,   INPUT);
    pinMode(WS_CS3_PIN,     OUTPUT); digitalWrite(WS_CS3_PIN, HIGH);
    pinMode(WS_BUSB_CS4_PIN,OUTPUT); digitalWrite(WS_BUSB_CS4_PIN, HIGH);

    Serial.println("[WS] SPI buses initialised (Bus A=FSPI/HW, Bus B=bit-bang)");
    return true;
}

bool ws_adc_init_all(void)
{
    bool ok = true;

    // ADC index mapping: [0]=A1/CS1/BusA, [1]=A2/CS2/BusA,
    //                    [2]=B1/CS3/BusB, [3]=B2/CS4/BusB
    const bool isBusB[4]   = { false, false, true, true };
    const uint8_t cs[4]    = { WS_CS1_PIN, WS_CS2_PIN,
                                WS_CS3_PIN, WS_BUSB_CS4_PIN };

    for (int i = 0; i < 4; ++i) {
        if (!_initOneADC(isBusB[i], cs[i])) {
            Serial.printf("[WS] ADC[%d] (cs=%d) FAILED init\n", i, cs[i]);
            ok = false;
        }
    }
    return ok;
}

bool ws_read_bus_a(WeighReading *adc1_out, WeighReading *adc2_out)
{
    bool anyValid = false;

    // Helper lambda-style struct for Bus A reads
    struct { uint8_t cs; WeighReading *out; } targets[2] = {
        { WS_CS1_PIN, adc1_out },
        { WS_CS2_PIN, adc2_out }
    };

    for (auto &t : targets) {
        if (t.out == nullptr) continue;

        // AIN1(+)/AIN2(-) differential, referenced to REFIN2
        t.out->valid_ch01 = _readChannel(false, t.cs,
                                          AD7190_CH_AIN1P_AIN2M,  // index 0
                                          WS_REFSEL_REFIN2,
                                          &t.out->raw_ch01);

        // AIN3(+)/AIN4(-) differential, referenced to REFIN1
        t.out->valid_ch23 = _readChannel(false, t.cs,
                                          AD7190_CH_AIN3P_AIN4M,  // index 1
                                          WS_REFSEL_REFIN1,
                                          &t.out->raw_ch23);

        if (t.out->valid_ch01 || t.out->valid_ch23) anyValid = true;
    }
    return anyValid;
}

bool ws_read_bus_b(WeighReading *adc3_out, WeighReading *adc4_out)
{
    bool anyValid = false;

    struct { uint8_t cs; WeighReading *out; } targets[2] = {
        { WS_CS3_PIN,      adc3_out },
        { WS_BUSB_CS4_PIN, adc4_out }
    };

    for (auto &t : targets) {
        if (t.out == nullptr) continue;

        // AIN1(+)/AIN2(-) differential, referenced to REFIN2
        t.out->valid_ch01 = _readChannel(true, t.cs,
                                          AD7190_CH_AIN1P_AIN2M,  // index 0
                                          WS_REFSEL_REFIN2,
                                          &t.out->raw_ch01);

        // AIN3(+)/AIN4(-) differential, referenced to REFIN1
        t.out->valid_ch23 = _readChannel(true, t.cs,
                                          AD7190_CH_AIN3P_AIN4M,  // index 1
                                          WS_REFSEL_REFIN1,
                                          &t.out->raw_ch23);

        if (t.out->valid_ch01 || t.out->valid_ch23) anyValid = true;
    }
    return anyValid;
}

bool ws_read_all(WeighReading readings[4])
{
    bool okA = ws_read_bus_a(&readings[0], &readings[1]);
    bool okB = ws_read_bus_b(&readings[2], &readings[3]);

    // "All valid" means every individual channel on every ADC succeeded
    for (int i = 0; i < 4; ++i) {
        if (!readings[i].valid_ch01 || !readings[i].valid_ch23) return false;
    }
    return okA && okB;
}

void ws_print_reading(const char *label, const WeighReading &r)
{
    Serial.printf("[WS] %s | ch01(REFIN2): 0x%06lX (%s) | ch23(REFIN1): 0x%06lX (%s)\n",
                  label,
                  r.raw_ch01, r.valid_ch01 ? "OK" : "ERR",
                  r.raw_ch23, r.valid_ch23 ? "OK" : "ERR");
}
