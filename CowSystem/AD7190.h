/**
 * AD7190.h
 * Driver for 4x AD7190 24-bit Sigma-Delta ADCs on ESP32-C6-Mini
 *
 * Hardware layout — DUAL SPI BUS:
 *
 *   Bus A (FSPI hardware): MOSI=IO22, MISO=IO23, SCLK=IO15
 *     CS1=IO2 (ADC1), CS2=IO3 (ADC2)
 *
 *   Bus B (bit-bang):      MOSI=IO20, MISO=IO21, SCLK=IO19
 *     CS3=IO1 (ADC3), CS4=IO5 (ADC4)
 *
 * Note on Bus B: The ESP32-C6 Arduino core only reliably supports one
 * hardware SPI peripheral (FSPI / peripheral 0). SPIClass(1) does not
 * function correctly on this chip. Bus B is therefore implemented as
 * software bit-bang SPI (Mode 3: CPOL=1, CPHA=1, MSB first).
 *
 * Bus safety: All multi-step ADC operations (reset, read, recover) hold the
 * hardware SPI transaction open continuously from csSelect to csDeselect.
 * This prevents any gap on MISO between the comm byte and data bytes where
 * a second ADC on the same bus could briefly drive the line.
 *
 * Each ADC has 2 load cells:
 *   Load cell 1: AIN1+/AIN2-  with REFIN2+/REFIN2-
 *   Load cell 2: AIN3+/AIN4-  with REFIN1+/REFIN1-
 *   BPDSW (GPOCON) closes the bridge power-down switch to AGND
 *
 * Settings: SINC4, CHOP enabled, Gain=128, ~50 Hz output data rate
 */

#pragma once
#include <Arduino.h>
#include <SPI.h>

// ─── SPI Bus A Pin Definitions (ADC1 & ADC2 — hardware FSPI) ─────────────────
#define AD7190_BUSA_MOSI   22
#define AD7190_BUSA_MISO   23
#define AD7190_BUSA_SCK    15

// ─── SPI Bus B Pin Definitions (ADC3 & ADC4 — bit-bang) ──────────────────────
#define AD7190_BUSB_MOSI   20
#define AD7190_BUSB_MISO   21
#define AD7190_BUSB_SCK    19

// ─── Chip Select Pins ─────────────────────────────────────────────────────────
#define AD7190_CS1         2    // Bus A – ADC1
#define AD7190_CS2         3    // Bus A – ADC2
#define AD7190_CS3         1    // Bus B – ADC3
#define AD7190_CS4         5    // Bus B – ADC4

#define AD7190_NUM_ADCS          4
#define AD7190_CHANNELS_PER_ADC  2

// ─── SPI Settings ─────────────────────────────────────────────────────────────
#define AD7190_SPI_FREQ          100000UL
#define AD7190_SPI_MODE          SPI_MODE3
// Bit-bang half-period: 1 / (2 × 100 kHz) = 5 µs
#define AD7190_BB_HALF_PERIOD_US 5

// ─── Register Addresses ───────────────────────────────────────────────────────
#define AD7190_REG_COMM        0x00
#define AD7190_REG_STATUS      0x00
#define AD7190_REG_MODE        0x01
#define AD7190_REG_CONF        0x02
#define AD7190_REG_DATA        0x03
#define AD7190_REG_ID          0x04
#define AD7190_REG_GPOCON      0x05
#define AD7190_REG_OFFSET      0x06
#define AD7190_REG_FULLSCALE   0x07

// ─── Communications Register Bits ─────────────────────────────────────────────
#define AD7190_COMM_WEN        (0 << 7)
#define AD7190_COMM_READ       (1 << 6)
#define AD7190_COMM_WRITE      (0 << 6)
#define AD7190_COMM_ADDR(r)    ((r) << 3)
#define AD7190_COMM_CREAD      (1 << 2)

// ─── Mode Register Bit Masks (24-bit) ─────────────────────────────────────────
#define AD7190_MODE_CONT             (0x0 << 21)
#define AD7190_MODE_SINGLE           (0x1 << 21)
#define AD7190_MODE_IDLE             (0x2 << 21)
#define AD7190_MODE_PWRDN            (0x3 << 21)
#define AD7190_MODE_INTZCAL          (0x4 << 21)
#define AD7190_MODE_INTFSCAL         (0x6 << 21)
#define AD7190_MODE_DAT_STA          (1 << 20)
#define AD7190_MODE_CLK_INT_NOTAVAIL (0x2 << 18)
#define AD7190_MODE_CLK_INT_AVAIL    (0x3 << 18)
#define AD7190_MODE_CLK_EXT          (0x0 << 18)
#define AD7190_MODE_SINC4            (0 << 11)
#define AD7190_MODE_SINC3            (1 << 11)
#define AD7190_MODE_REJ60            (1 << 10)
#define AD7190_FS_50HZ_CHOP          0x030

// ─── Configuration Register Bit Masks (24-bit) ────────────────────────────────
#define AD7190_CONF_CHOP           (1 << 23)
#define AD7190_CONF_REFSEL_1       (0 << 20)
#define AD7190_CONF_REFSEL_2       (1 << 20)
#define AD7190_CONF_CH_AIN1_AIN2   (1 << 8)
#define AD7190_CONF_CH_AIN3_AIN4   (1 << 9)
#define AD7190_CONF_BUF            (1 << 4)
#define AD7190_CONF_BIPOLAR        (0 << 3)
#define AD7190_CONF_UNIPOLAR       (1 << 3)
#define AD7190_CONF_GAIN_1         0x0
#define AD7190_CONF_GAIN_8         0x3
#define AD7190_CONF_GAIN_16        0x4
#define AD7190_CONF_GAIN_32        0x5
#define AD7190_CONF_GAIN_64        0x6
#define AD7190_CONF_GAIN_128       0x7

// ─── GPOCON Register Bits (8-bit) ─────────────────────────────────────────────
#define AD7190_GPOCON_BPDSW    (1 << 6)
#define AD7190_GPOCON_GP32EN   (1 << 5)
#define AD7190_GPOCON_GP10EN   (1 << 4)

// ─── Status Register Bits (8-bit) ─────────────────────────────────────────────
#define AD7190_STATUS_RDY      (1 << 7)
#define AD7190_STATUS_ERR      (1 << 6)
#define AD7190_STATUS_NOREF    (1 << 5)
#define AD7190_STATUS_CHANNEL  0x0F

// ─── Expected ID Register Value ───────────────────────────────────────────────
#define AD7190_ID_VALUE        0x04

// ─── Channel Identifiers ──────────────────────────────────────────────────────
typedef enum {
    AD7190_CH_LOADCELL1 = 0,
    AD7190_CH_LOADCELL2 = 1
} AD7190Channel;

// ─── ADC Instance Identifier ──────────────────────────────────────────────────
typedef enum {
    AD7190_ADC1 = 0,   // Bus A (FSPI hardware), CS=IO2
    AD7190_ADC2 = 1,   // Bus A (FSPI hardware), CS=IO3
    AD7190_ADC3 = 2,   // Bus B (bit-bang),       CS=IO1
    AD7190_ADC4 = 3    // Bus B (bit-bang),       CS=IO5
} AD7190Index;

// ─── Measurement Result ───────────────────────────────────────────────────────
struct AD7190Result {
    uint32_t raw;       // Raw 24-bit ADC code
    double   voltage;   // Tare-corrected bridge output in mV
    double   kg;        // Converted weight in kg (uses per-channel sensitivity)
    uint8_t  status;    // Status byte appended to data read
    bool     valid;     // True if read succeeded with no error flags and zero
                        // reference is trustworthy. False during post-recovery
                        // retare deferral — partner channel data should be used.
};

// ─────────────────────────────────────────────────────────────────────────────
// BitBangSPI — software SPI Mode 3 (CPOL=1, CPHA=1), MSB first
//
// SCLK idles HIGH. MOSI driven before falling edge. MISO sampled on rising.
// ─────────────────────────────────────────────────────────────────────────────
class BitBangSPI {
public:
    BitBangSPI() : _sck(0), _miso(0), _mosi(0) {}

    void begin(uint8_t sck, uint8_t miso, uint8_t mosi) {
        _sck  = sck;
        _miso = miso;
        _mosi = mosi;
        pinMode(_sck,  OUTPUT); digitalWrite(_sck,  HIGH);
        pinMode(_mosi, OUTPUT); digitalWrite(_mosi, LOW);
        pinMode(_miso, INPUT);
    }

    uint8_t transfer(uint8_t txByte) {
        uint8_t rxByte = 0;
        for (int8_t bit = 7; bit >= 0; bit--) {
            digitalWrite(_mosi, (txByte >> bit) & 0x01);
            delayMicroseconds(AD7190_BB_HALF_PERIOD_US);
            digitalWrite(_sck, LOW);
            delayMicroseconds(AD7190_BB_HALF_PERIOD_US);
            digitalWrite(_sck, HIGH);
            if (digitalRead(_miso)) rxByte |= (1 << bit);
        }
        return rxByte;
    }

private:
    uint8_t _sck;
    uint8_t _miso;
    uint8_t _mosi;
};

// ─── Main Driver Class ────────────────────────────────────────────────────────
class AD7190Driver {
public:

    void captureTare();
    bool isTareDone() const { return _tareDone; }

    explicit AD7190Driver(float vrefMv = 5000.0f);

    bool begin();

    AD7190Result readChannel(AD7190Index adc, AD7190Channel channel);
    void         readBothChannels(AD7190Index adc, AD7190Result out[2]);
    void         readAll(AD7190Result out[8]);

    uint8_t readID(AD7190Index adc);
    void    powerDown(AD7190Index adc);
    void    wakeUp(AD7190Index adc);
    void    calibrate(AD7190Index adc);
    void    resetDevice(AD7190Index adc);

    /**
     * Convert a raw ADC code to kg using per-channel load cell sensitivity.
     * Channels 0–3 (Bus A): 1 mV/V sensitivity
     * Channels 4–7 (Bus B): 2 mV/V sensitivity
     * Rated capacity is set in AD7190_LC_RATED_KG[] in AD7190.cpp.
     */
    double rawToKg(uint32_t raw, int channel) const;

    /**
     * Returns true if this ADC is waiting to retare (scale was loaded at
     * recovery time). While pending, both channels of this ADC return
     * valid=false. Call checkDeferredRetare() each loop to poll.
     */
    bool isRetarePending(AD7190Index adc) const {
        return _pendingRetare[(int)adc];
    }

    /**
     * Call once per main loop. For each Bus A ADC with a deferred retare,
     * reads the partner ADC on the same physical scale. If both partner
     * channels read below RETARE_LOADED_THRESHOLD_KG, the scale is empty
     * and retare proceeds. Returns true if any retare was performed.
     *
     * Partner mapping (same physical scale platform split across two ADCs):
     *   ADC1 (ch0,1)  ↔  ADC2 (ch2,3)
     */
    bool checkDeferredRetare();

private:
    SPIClass    _spiA;
    SPISettings _spiSettings;
    BitBangSPI  _spiB;

    float   _vrefMv;
    int32_t _tare[8]  = {0};
    bool    _tareDone = false;

    // Per-ADC failure flag — set when recovery fails ID check.
    bool _adcFailed[AD7190_NUM_ADCS] = {false, false, false, false};

    // Per-channel consecutive midscale (0x800000) read counter.
    uint8_t _midscaleCount[AD7190_NUM_ADCS * AD7190_CHANNELS_PER_ADC] = {0};

    // ── Deferred retare ─────────────────────────────────────────────────────
    // Set for a Bus A ADC when recovery fires while the partner half shows
    // a loaded reading. Retare is deferred until the scale empties.
    // While pending, readChannel() returns valid=false for both channels.
    bool _pendingRetare[AD7190_NUM_ADCS] = {false, false, false, false};

    // A scale half is considered loaded if either channel reads above this.
    // Set to ~10× your noise floor. Adjust if your cells differ.
    static constexpr double RETARE_LOADED_THRESHOLD_KG = 0.05;

    static const uint8_t _csPins[AD7190_NUM_ADCS];

    inline bool isBusBB(AD7190Index adc) const { return (int)adc >= 2; }

    // ── Atomic bus access ───────────────────────────────────────────────────
    void     busBegin(AD7190Index adc);
    void     busEnd  (AD7190Index adc);
    uint8_t  busTransfer(AD7190Index adc, uint8_t out);

    // ── Register access ─────────────────────────────────────────────────────
    void     writeRegister(AD7190Index adc, uint8_t reg, uint32_t value, uint8_t numBytes);
    uint32_t readRegister (AD7190Index adc, uint8_t reg, uint8_t numBytes);

    void     configureForChannel(AD7190Index adc, AD7190Channel channel);
    bool     waitForReady(AD7190Index adc, uint32_t timeoutMs = 200);

    double   rawToMillivolts(uint32_t raw, int channel) const;
    uint32_t buildConfReg(AD7190Channel channel);

    // Full hardware reset + re-init. Returns true if ADC responds correctly.
    bool     fullResetADC(AD7190Index adc);
    // Soft recovery (exit CREAD / stuck state) then fullResetADC.
    void     recoverADC(AD7190Index adc);
    // Re-tare only the two channels belonging to one ADC.
    // For Bus A: only called when scale confirmed empty.
    // For Bus B: performs offset compensation (scale stays loaded).
    void     retareChannels(AD7190Index adc);
    // Returns the kg reading from the partner Bus A ADC (ADC1↔ADC2).
    // Used to decide whether to retare or defer.
    double   partnerMaxKg(AD7190Index adc);
    // Write a register and read it back to verify. Returns false if readback
    // doesn't match — useful for diagnosing SPI cable integrity issues.
    bool     writeRegisterVerified(AD7190Index adc, uint8_t reg, uint32_t value,
                                   uint8_t numBytes);
};