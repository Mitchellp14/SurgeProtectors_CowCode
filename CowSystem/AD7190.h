/**
 * AD7190.h
 * Driver for 4x AD7190 24-bit Sigma-Delta ADCs on ESP32-C6-Mini
 *
 * Hardware layout:
 *   SPI:  DIN=IO22 (MOSI), DOUT=IO23 (MISO), SCLK=IO19
 *   CS1=IO5 (Weigh System 1, ADC1), CS2=IO1 (Weigh System 1, ADC2)
 *   CS3=IO3 (Weigh System 2, ADC3), CS4=IO2 (Weigh System 2, ADC4)
 *
 * Each ADC has 2 load cells:
 *   Load cell 1: AIN1+/AIN2-  with REFIN2+/REFIN2-
 *   Load cell 2: AIN3+/AIN4-  with REFIN1+/REFIN1-
 *   BPDSW (GPOCON) closes the bridge power-down switch to AGND for excitation ground return
 *
 * Settings: SINC4, CHOP enabled, Gain=128, ~50 Hz output data rate
 */

#pragma once
#include <Arduino.h>
#include <SPI.h>

// ─── SPI Pin Definitions ──────────────────────────────────────────────────────
#define AD7190_PIN_MOSI   22
#define AD7190_PIN_MISO   23
#define AD7190_PIN_SCK    19

// ─── Chip Select Pins ─────────────────────────────────────────────────────────
#define AD7190_CS1        5    // Weigh System 1 – ADC 1
#define AD7190_CS2        1    // Weigh System 1 – ADC 2
#define AD7190_CS3        3    // Weigh System 2 – ADC 3
#define AD7190_CS4        2    // Weigh System 2 – ADC 4

#define AD7190_NUM_ADCS   4
#define AD7190_CHANNELS_PER_ADC 2

// ─── SPI Settings ─────────────────────────────────────────────────────────────
// AD7190: SPI Mode 3 (CPOL=1, CPHA=1), MSB first, max ~5 MHz
#define AD7190_SPI_FREQ   2000000UL  // 2 MHz – conservative for wiring
#define AD7190_SPI_MODE   SPI_MODE3

// ─── Register Addresses ───────────────────────────────────────────────────────
#define AD7190_REG_COMM        0x00   // Communications  (WO, 8-bit)
#define AD7190_REG_STATUS      0x00   // Status          (RO, 8-bit) – same addr, read via COMM
#define AD7190_REG_MODE        0x01   // Mode            (RW, 24-bit)
#define AD7190_REG_CONF        0x02   // Configuration   (RW, 24-bit)
#define AD7190_REG_DATA        0x03   // Data            (RO, 24-bit, or 32-bit with status)
#define AD7190_REG_ID          0x04   // ID              (RO, 8-bit)
#define AD7190_REG_GPOCON      0x05   // GPOCON          (RW, 8-bit)
#define AD7190_REG_OFFSET      0x06   // Offset          (RW, 24-bit)
#define AD7190_REG_FULLSCALE   0x07   // Full-Scale      (RW, 24-bit)

// ─── Communications Register Bits ─────────────────────────────────────────────
// Byte sent before every transaction to select register + RD/WR
#define AD7190_COMM_WEN        (0 << 7)   // Write enable (must be 0)
#define AD7190_COMM_READ       (1 << 6)   // 1 = Read
#define AD7190_COMM_WRITE      (0 << 6)   // 0 = Write
#define AD7190_COMM_ADDR(r)    ((r) << 3) // Register address bits [5:3]
#define AD7190_COMM_CREAD      (1 << 2)   // Continuous read enable

// ─── Mode Register Bit Masks (24-bit) ─────────────────────────────────────────
// Bits [23:21] – operating mode
#define AD7190_MODE_CONT       (0x0 << 21)  // Continuous conversion (default)
#define AD7190_MODE_SINGLE     (0x1 << 21)  // Single conversion
#define AD7190_MODE_IDLE       (0x2 << 21)  // Idle mode
#define AD7190_MODE_PWRDN      (0x3 << 21)  // Power-down mode
#define AD7190_MODE_INTZCAL    (0x4 << 21)  // Internal zero-scale calibration
#define AD7190_MODE_INTFSCAL   (0x6 << 21)  // Internal full-scale calibration

// Bit 20 – DAT_STA: append status register to data reads
#define AD7190_MODE_DAT_STA    (1 << 20)

// Bits [19:18] – clock source
#define AD7190_MODE_CLK_INT_NOTAVAIL (0x2 << 18)  // Internal 4.92 MHz, MCLK2 tristated
#define AD7190_MODE_CLK_INT_AVAIL    (0x3 << 18)  // Internal 4.92 MHz, available on MCLK2
#define AD7190_MODE_CLK_EXT          (0x0 << 18)  // External from MCLK1

// Bit 11 – SINC3: 0=SINC4 (default, better 50/60 Hz rejection), 1=SINC3
#define AD7190_MODE_SINC4      (0 << 11)
#define AD7190_MODE_SINC3      (1 << 11)

// Bit 10 – REJ60: place additional notch at 60 Hz when ODR=50 Hz
#define AD7190_MODE_REJ60      (1 << 10)

// Bits [9:0] – FS[9:0]: filter word
// With CHOP enabled: fADC = fCLK / (1024 × FS × 2)
// For 50 Hz:  FS = 4915200 / (1024 × 50 × 2) = 48 = 0x030
#define AD7190_FS_50HZ_CHOP    0x030  // 48 decimal → ~50 Hz with chop enabled

// ─── Configuration Register Bit Masks (24-bit) ────────────────────────────────
// Bit 23 – CHOP: enable chopping (eliminates offset, halves ODR)
#define AD7190_CONF_CHOP       (1 << 23)

// Bit 20 – REFSEL: 0=REFIN1, 1=REFIN2
#define AD7190_CONF_REFSEL_1   (0 << 20)   // Use REFIN1+/REFIN1-
#define AD7190_CONF_REFSEL_2   (1 << 20)   // Use REFIN2+/REFIN2-

// Bits [9:8] – channel selection (can OR multiple for sequencer)
// For manual per-channel reads we set one at a time
#define AD7190_CONF_CH_AIN1_AIN2   (1 << 8)   // Differential AIN1+/AIN2-
#define AD7190_CONF_CH_AIN3_AIN4   (1 << 9)   // Differential AIN3+/AIN4-

// Bit 4 – BUF: enable input buffer (required for high-impedance sources)
#define AD7190_CONF_BUF        (1 << 4)

// Bit 3 – UNIPOLAR: 0=bipolar (default), 1=unipolar
#define AD7190_CONF_BIPOLAR    (0 << 3)
#define AD7190_CONF_UNIPOLAR   (1 << 3)

// Bits [2:0] – PGA gain
#define AD7190_CONF_GAIN_1     0x0
#define AD7190_CONF_GAIN_8     0x3
#define AD7190_CONF_GAIN_16    0x4
#define AD7190_CONF_GAIN_32    0x5
#define AD7190_CONF_GAIN_64    0x6
#define AD7190_CONF_GAIN_128   0x7   // Used here

// ─── GPOCON Register Bits (8-bit) ─────────────────────────────────────────────
// Bit 6 – BPDSW: 1=close bridge power-down switch (BPDSW pin to AGND)
// This connects load cell excitation ground through the ADC's internal switch
#define AD7190_GPOCON_BPDSW    (1 << 6)
#define AD7190_GPOCON_GP32EN   (1 << 5)
#define AD7190_GPOCON_GP10EN   (1 << 4)

// ─── Status Register Bits (8-bit) ─────────────────────────────────────────────
#define AD7190_STATUS_RDY      (1 << 7)   // 0=data ready, 1=not ready
#define AD7190_STATUS_ERR      (1 << 6)
#define AD7190_STATUS_NOREF    (1 << 5)
#define AD7190_STATUS_CHANNEL  0x0F       // Bits [3:0] active channel

// ─── Expected ID Register Value ───────────────────────────────────────────────
#define AD7190_ID_VALUE        0x04   // Lower nibble = 0x4 for AD7190

// ─── Channel Identifiers (used in API) ────────────────────────────────────────
typedef enum {
    AD7190_CH_LOADCELL1 = 0,   // AIN1+/AIN2-, REFIN2
    AD7190_CH_LOADCELL2 = 1    // AIN3+/AIN4-, REFIN1
} AD7190Channel;

// ─── ADC Instance Identifier ──────────────────────────────────────────────────
typedef enum {
    AD7190_ADC1 = 0,   // Weigh System 1, CS=IO5
    AD7190_ADC2 = 1,   // Weigh System 1, CS=IO1
    AD7190_ADC3 = 2,   // Weigh System 2, CS=IO3
    AD7190_ADC4 = 3    // Weigh System 2, CS=IO2
} AD7190Index;

// ─── Measurement Result ───────────────────────────────────────────────────────
struct AD7190Result {
    uint32_t raw;          // Raw 24-bit ADC code
    double   voltage;      // Converted voltage in mV (requires Vref to be set)
    uint8_t  status;       // Status byte appended to data read
    bool     valid;        // True if read succeeded and no error flags
};

// ─── Main Driver Class ────────────────────────────────────────────────────────
class AD7190Driver {
public:
    /**
     * Constructor.
     * @param vrefMv  Reference voltage in millivolts (e.g. 5000 for 5 V).
     *                Used only for voltage conversion; raw counts always available.
     */
    explicit AD7190Driver(float vrefMv = 5000.0f);

    /**
     * Initialise SPI bus and all 4 ADC chips.
     * Must be called once in setup().
     * @return true if all 4 ADCs respond with correct ID.
     */
    bool begin();

    /**
     * Perform a single conversion on the selected channel of one ADC.
     * Blocks until RDY asserts or timeout expires (~200 ms).
     *
     * @param adc      Which ADC to read (AD7190_ADC1 … AD7190_ADC4)
     * @param channel  Which load cell channel (AD7190_CH_LOADCELL1 or 2)
     * @return         AD7190Result with raw code, millivolt value, and validity flag
     */
    AD7190Result readChannel(AD7190Index adc, AD7190Channel channel);

    /**
     * Read both channels of one ADC sequentially.
     * @param adc     Which ADC
     * @param out     Array of 2 results [0]=loadcell1, [1]=loadcell2
     */
    void readBothChannels(AD7190Index adc, AD7190Result out[2]);

    /**
     * Read all 8 channels (2 per ADC × 4 ADCs).
     * @param out     Array of 8 results, indexed [adc*2 + channel]
     */
    void readAll(AD7190Result out[8]);

    /**
     * Read the ID register of one ADC for diagnostics.
     * @return ID byte (expect lower nibble == 0x4 for AD7190)
     */
    uint8_t readID(AD7190Index adc);

    /**
     * Put one ADC into power-down mode (reduces current consumption).
     */
    void powerDown(AD7190Index adc);

    /**
     * Wake one ADC from power-down back into continuous conversion idle.
     */
    void wakeUp(AD7190Index adc);

    /**
     * Trigger internal zero-scale then full-scale self-calibration on one ADC.
     * Takes ~8× the normal conversion time per calibration step.
     * Call this once after begin() for best accuracy.
     */
    void calibrate(AD7190Index adc);

    /**
     * Reset one ADC by sending 40+ high bits on DIN.
     */
    void resetDevice(AD7190Index adc);

private:
    // SPI bus object (FSPI on ESP32-C6)
    SPIClass  _spi;
    SPISettings _spiSettings;
    float     _vrefMv;

    // CS pin lookup table
    static const uint8_t _csPins[AD7190_NUM_ADCS];

    // ── Low-level register access ───────────────────────────────────────────
    void     writeRegister(AD7190Index adc, uint8_t reg, uint32_t value, uint8_t numBytes);
    uint32_t readRegister (AD7190Index adc, uint8_t reg, uint8_t numBytes);

    void     csSelect  (AD7190Index adc);
    void     csDeselect(AD7190Index adc);

    // ── Per-channel configuration helpers ──────────────────────────────────
    /**
     * Write Mode + Config registers for a single channel measurement.
     * Starts a single conversion; caller must then poll for RDY.
     */
    void configureForChannel(AD7190Index adc, AD7190Channel channel);

    /**
     * Poll DOUT/RDY (via status register read) until ready or timeout.
     * @param timeoutMs  Max wait in milliseconds
     * @return true if data is ready
     */
    bool waitForReady(AD7190Index adc, uint32_t timeoutMs = 200);

    // ── Voltage conversion ──────────────────────────────────────────────────
    double rawToMillivolts(uint32_t raw) const;

    // ── Register value builders ─────────────────────────────────────────────
    uint32_t buildConfReg(AD7190Channel channel);

    // ── Recover ADC ─────────────────────────────────────────────────────────
    void recoverADC(AD7190Index adc);

};
