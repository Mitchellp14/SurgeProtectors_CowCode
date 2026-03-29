/**
 * weigh_system.h
 * ============================================================
 * Weighing system driver for 4x AD7190 ADCs on an ESP32-C6-Mini.
 *
 * Hardware topology
 * -----------------
 *  Shared SCLK   : IO19
 *
 *  "Bus A" (FSPI / SPI2 — the only true hardware SPI on C6):
 *    MOSI (DIN1)  : IO22
 *    MISO (DOUT1) : IO23
 *    CS1          : IO03   → ADC_A1 (AIN0/1 ref REFIN2, AIN2/3 ref REFIN1)
 *    CS2          : IO02   → ADC_A2 (AIN0/1 ref REFIN2, AIN2/3 ref REFIN1)
 *
 *  "Bus B" (HSPI — bit-banged software SPI, see IMPORTANT NOTE below):
 *    MOSI (DIN2)  : IO20
 *    MISO (DOUT2) : IO21
 *    CS3          : IO05   → ADC_B1 (AIN0/1 ref REFIN2, AIN2/3 ref REFIN1)
 *    CS4          : IO01   → ADC_B2 (AIN0/1 ref REFIN2, AIN2/3 ref REFIN1)
 *
 * IMPORTANT — ESP32-C6 SPI hardware limitation
 * ---------------------------------------------
 * The ESP32-C6 exposes only ONE user-accessible hardware SPI peripheral
 * (SPI2 / FSPI). SPI0 and SPI1 are reserved for internal flash.  There is
 * no SPI3/VSPI on this chip variant.
 *
 * "Bus B" is therefore implemented as **software (bit-bang) SPI** using the
 * same SCLK line (IO19) so the physical timing is still driven by software.
 * For a 24-bit sigma-delta ADC at typical output data rates (< 4.8 kHz) this
 * is entirely sufficient — the bottleneck is always the ADC conversion time,
 * not SPI throughput.
 *
 * If you need both buses to run simultaneously (true parallel reads), you
 * would need to use FreeRTOS tasks — but note that the shared SCLK means
 * the two buses cannot clock simultaneously anyway. Sequential reads on
 * alternating buses is the correct strategy.
 *
 * Dependencies
 * ------------
 *  • arduino-esp32  ≥ 3.x (ESP32-C6 support)
 *  • AD7190forESP32 library by gism  (install via Arduino Library Manager)
 *    https://github.com/gism/ESP32_AD7190
 *
 * ============================================================
 */

#pragma once

#include <Arduino.h>
#include <SPI.h>

// ---------------------------------------------------------------------------
// AD7190 register and bit-field definitions
// Sourced from the AD7190 datasheet (Rev. E) and cross-checked against the
// gism/ESP32_AD7190 library (src/esp32_ad7190/ad7190_spi.h).
// We define only what we use here so the driver compiles without depending
// on any particular library install path or version.
// ---------------------------------------------------------------------------

/* Register addresses */
#define AD7190_REG_COMM         0   // Communications Register (WO, 8-bit)
#define AD7190_REG_STAT         0   // Status Register         (RO, 8-bit)
#define AD7190_REG_MODE         1   // Mode Register           (RW, 24-bit)
#define AD7190_REG_CONF         2   // Configuration Register  (RW, 24-bit)
#define AD7190_REG_DATA         3   // Data Register           (RO, 24-bit)
#define AD7190_REG_ID           4   // ID Register             (RO, 8-bit)
#define AD7190_REG_GPOCON       5   // GPOCON Register         (RW, 8-bit)
#define AD7190_REG_OFFSET       6   // Offset Register         (RW, 24-bit)
#define AD7190_REG_FULLSCALE    7   // Full-Scale Register     (RW, 24-bit)

/* ID Register */
#define ID_AD7190               0x4         // Lower nibble of ID reg for AD7190
#define AD7190_ID_MASK          0x0F

/* Mode Register bit fields */
#define AD7190_MODE_SEL(x)      (((uint32_t)(x) & 0x7) << 21)
#define AD7190_MODE_CLKSRC(x)   (((uint32_t)(x) & 0x3) << 18)
#define AD7190_MODE_RATE(x)     ((uint32_t)(x) & 0x3FF)

/* Mode Register: AD7190_MODE_SEL options */
#define AD7190_MODE_CONT            0   // Continuous conversion
#define AD7190_MODE_SINGLE          1   // Single conversion
#define AD7190_MODE_IDLE            2   // Idle
#define AD7190_MODE_PWRDN           3   // Power-down
#define AD7190_MODE_CAL_INT_ZERO    4   // Internal zero-scale calibration
#define AD7190_MODE_CAL_INT_FULL    5   // Internal full-scale calibration

/* Mode Register: AD7190_MODE_CLKSRC options */
#define AD7190_CLK_INT              2   // Internal 4.92 MHz clock (MCLK2 tristated)

/* Configuration Register bit fields */
#define AD7190_CONF_CHOP            (1UL << 23) // Chop enable
#define AD7190_CONF_REFSEL          (1UL << 20) // 0=REFIN1, 1=REFIN2
#define AD7190_CONF_CHAN(x)         (((uint32_t)(x) & 0xFF) << 8)
#define AD7190_CONF_BUF             (1UL << 4)  // Buffer enable
#define AD7190_CONF_UNIPOLAR        (1UL << 3)  // 1=unipolar, 0=bipolar
#define AD7190_CONF_GAIN(x)         ((uint32_t)(x) & 0x7)

/* Configuration Register: channel index constants for AD7190_CONF_CHAN(1<<x) */
#define AD7190_CH_AIN1P_AIN2M       0   // AIN1(+) – AIN2(-)  → your AIN0/1 pair
#define AD7190_CH_AIN3P_AIN4M       1   // AIN3(+) – AIN4(-)  → your AIN2/3 pair

/* Configuration Register: gain constants for AD7190_CONF_GAIN(x) */
#define AD7190_CONF_GAIN_1          0
#define AD7190_CONF_GAIN_8          3
#define AD7190_CONF_GAIN_16         4
#define AD7190_CONF_GAIN_32         5
#define AD7190_CONF_GAIN_64         6
#define AD7190_CONF_GAIN_128        7

// ---------------------------------------------------------------------------
// Pin definitions
// ---------------------------------------------------------------------------

// Shared clock
#define WS_SCLK_PIN     19

// Hardware SPI bus (Bus A — FSPI/SPI2)
#define WS_BUSA_MOSI    22    // DIN1
#define WS_BUSA_MISO    23    // DOUT1
#define WS_CS1_PIN       3    // ADC_A1
#define WS_CS2_PIN       2    // ADC_A2

// Software SPI bus (Bus B — bit-bang on same SCLK)
#define WS_BUSB_MOSI    20    // DIN2
#define WS_BUSB_MISO    21    // DOUT2
#define WS_CS3_PIN       5    // ADC_B1
#define WS_BUSB_CS4_PIN  1    // ADC_B2

// ---------------------------------------------------------------------------
// SPI settings
// ---------------------------------------------------------------------------
// AD7190 supports SPI Mode 3 (CPOL=1, CPHA=1), up to ~5 MHz.
// Using 2 MHz gives comfortable margin when routed through GPIO matrix.
#define WS_SPI_FREQ     2000000UL
#define WS_SPI_MODE     SPI_MODE3
#define WS_SPI_ORDER    MSBFIRST

// ---------------------------------------------------------------------------
// AD7190 configuration defaults
// You can change these to suit your load-cell bridge and reference topology.
// ---------------------------------------------------------------------------
// Gain: AD7190_CONF_GAIN_1 / _8 / _16 / _32 / _64 / _128
// Values are 0,3,4,5,6,7 respectively (not sequential — see datasheet Table 16)
#define WS_ADC_GAIN         AD7190_CONF_GAIN_128   // 128× for small bridge signals (±39 mV FS)

// Polarity: 0 = bipolar (recommended for bridge), 1 = unipolar
#define WS_ADC_POLARITY     0

// Output data rate filter word (lower = slower, less noise)
// 0x060 ≈ 50 Hz (internal 4.92 MHz clock)
#define WS_ADC_FILTER_RATE  0x060

// ---------------------------------------------------------------------------
// Data structure returned by each read call
// ---------------------------------------------------------------------------

struct WeighReading {
    uint32_t raw_ch01;   // AIN0/1 differential (ref REFIN2)
    uint32_t raw_ch23;   // AIN2/3 differential (ref REFIN1)
    bool     valid_ch01;
    bool     valid_ch23;
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * @brief  Initialise both SPI buses.
 *         Call once from setup() before anything else.
 * @return true on success.
 */
bool ws_spi_init(void);

/**
 * @brief  Initialise all four AD7190 devices.
 *         Resets, verifies ID, configures gain, polarity, channel.
 *         Must be called after ws_spi_init().
 * @return true if all four devices responded correctly.
 */
bool ws_adc_init_all(void);

/**
 * @brief  Read one conversion from each channel on Bus A ADCs (CS1 and CS2).
 *         Reads AIN0/1 (ref REFIN2) then AIN2/3 (ref REFIN1) on each device.
 *
 * @param  adc1_out  Pointer to WeighReading for CS1 (ADC_A1). May be NULL.
 * @param  adc2_out  Pointer to WeighReading for CS2 (ADC_A2). May be NULL.
 * @return true if at least one valid reading was obtained.
 */
bool ws_read_bus_a(WeighReading *adc1_out, WeighReading *adc2_out);

/**
 * @brief  Read one conversion from each channel on Bus B ADCs (CS3 and CS4).
 *         Reads AIN0/1 (ref REFIN2) then AIN2/3 (ref REFIN1) on each device.
 *
 * @param  adc3_out  Pointer to WeighReading for CS3 (ADC_B1). May be NULL.
 * @param  adc4_out  Pointer to WeighReading for CS4 (ADC_B2). May be NULL.
 * @return true if at least one valid reading was obtained.
 */
bool ws_read_bus_b(WeighReading *adc3_out, WeighReading *adc4_out);

/**
 * @brief  Convenience wrapper: read all four ADCs in one call.
 *         Internally calls ws_read_bus_a() then ws_read_bus_b().
 *
 * @param  readings  Array of 4 WeighReading structs.
 *                   [0]=ADC_A1, [1]=ADC_A2, [2]=ADC_B1, [3]=ADC_B2
 * @return true if all four readings were valid.
 */
bool ws_read_all(WeighReading readings[4]);

/**
 * @brief  Print a human-readable summary of a WeighReading to Serial.
 *         Useful during bring-up.
 */
void ws_print_reading(const char *label, const WeighReading &r);
