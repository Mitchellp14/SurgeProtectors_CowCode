/**
 * AD7190.cpp
 * Implementation of the AD7190 driver for ESP32-C6-Mini
 *
 * Key design decisions:
 *  - SPI Mode 3 (CPOL=1, CPHA=1): AD7190 clocks data on the rising edge,
 *    SCLK idles high – this matches SPI_MODE3.
 *  - DOUT/RDY is the shared MISO line. Between SPI transactions DOUT/RDY
 *    acts as a ready signal (goes LOW when conversion is complete). We poll
 *    it by reading the status register rather than using a dedicated GPIO,
 *    since all 4 CS lines are separate and the MISO bus is shared.
 *  - Each channel measurement is a single-conversion triggered by writing
 *    the mode register, then we poll status until RDY=0, then read data.
 *  - DAT_STA is set in the mode register so the 8-bit status is appended
 *    to every 24-bit data read (32-bit total), letting us confirm which
 *    channel the result belongs to.
 *  - BPDSW is set in GPOCON during init so the bridge excitation ground
 *    return is connected through the ADC's internal 30 mA switch to AGND.
 *  - CHOP is enabled for offset elimination. With SINC4+CHOP the effective
 *    ODR halves, so FS=48 gives ~50 Hz.
 */

#include "AD7190.h"

// ─── CS Pin Table ─────────────────────────────────────────────────────────────
const uint8_t AD7190Driver::_csPins[AD7190_NUM_ADCS] = {
    AD7190_CS1,   // ADC1 – IO5
    AD7190_CS2,   // ADC2 – IO1
    AD7190_CS3,   // ADC3 – IO3
    AD7190_CS4    // ADC4 – IO2
};

// ─── Constructor ──────────────────────────────────────────────────────────────
AD7190Driver::AD7190Driver(float vrefMv)
    : _spi(FSPI),   // ESP32-C6 uses FSPI for custom pin SPI
      _vrefMv(vrefMv)
{
    _spiSettings = SPISettings(AD7190_SPI_FREQ, MSBFIRST, AD7190_SPI_MODE);
}

// ─── begin() ──────────────────────────────────────────────────────────────────
bool AD7190Driver::begin()
{
    // Force all CS high BEFORE SPI starts – critical for strapping pins
    for (int i = 0; i < AD7190_NUM_ADCS; i++) {
        pinMode(_csPins[i], OUTPUT);
        digitalWrite(_csPins[i], HIGH);
    }
    delay(100); // Extra delay for strapping pins to settle after boot

    _spi.begin(AD7190_PIN_SCK, AD7190_PIN_MISO, AD7190_PIN_MOSI, -1);
    delay(10);

    // Init each ADC with extra reset attempts on failure
    bool allOk = true;
    for (int i = 0; i < AD7190_NUM_ADCS; i++) {
        AD7190Index idx = (AD7190Index)i;
        bool adcOk = false;

        for (int attempt = 0; attempt < 3 && !adcOk; attempt++) {
            resetDevice(idx);
            delay(10);  // Longer post-reset delay
            
            uint8_t id = readID(idx);
            Serial.printf("[AD7190] ADC%d attempt %d: ID=0x%02X\n", i+1, attempt+1, id);
            
            // Accept lower nibble 4 or 5 (observed in some lots)
            if ((id & 0x0F) == 0x04 || (id & 0x0F) == 0x05) {
                adcOk = true;
            } else {
                delay(20);
            }
        }

        if (!adcOk) {
            Serial.printf("[AD7190] ADC%d: FAILED after 3 attempts\n", i+1);
            allOk = false;
            continue;
        }

        writeRegister(idx, AD7190_REG_GPOCON, AD7190_GPOCON_BPDSW, 1);
        calibrate(idx);
        Serial.printf("[AD7190] ADC%d: Init OK\n", i+1);
    }
    return allOk;
}

// ─── readChannel() ────────────────────────────────────────────────────────────
AD7190Result AD7190Driver::readChannel(AD7190Index adc, AD7190Channel channel)
{
    AD7190Result result = {0, 0.0, 0, false};

    // Attempt up to 3 times with reset on failure
    for (int attempt = 0; attempt < 3; attempt++) {
        configureForChannel(adc, channel);

        if (!waitForReady(adc, 200)) {
            Serial.printf("[AD7190] ADC%d CH%d: Timeout (attempt %d), resetting\n",
                          (int)adc+1, (int)channel+1, attempt+1);
            recoverADC(adc);
            continue;
        }

        uint32_t raw32 = readRegister(adc, AD7190_REG_DATA, 4);
        result.status = (uint8_t)(raw32 & 0xFF);
        result.raw    = (raw32 >> 8) & 0x00FFFFFF;

        // Sanity check: 0x000000 or 0xFFFFFF are pathological values
        // indicating MISO stuck low/high — ADC needs recovery
        if (result.raw == 0x000000 || result.raw == 0xFFFFFF) {
            Serial.printf("[AD7190] ADC%d CH%d: Pathological raw=0x%06lX, recovering\n",
                          (int)adc+1, (int)channel+1, result.raw);
            recoverADC(adc);
            continue;
        }

        result.valid  = !(result.status & AD7190_STATUS_ERR) &&
                        !(result.status & AD7190_STATUS_NOREF);
        result.voltage = rawToMillivolts(result.raw);
        return result;  // Success
    }

    // All attempts failed
    Serial.printf("[AD7190] ADC%d CH%d: Failed after 3 attempts\n",
                  (int)adc+1, (int)channel+1);
    return result;  // valid=false
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
    // Transition to idle; a subsequent configureForChannel call will start conversion
    uint32_t modeReg = AD7190_MODE_IDLE
                     | AD7190_MODE_DAT_STA
                     | AD7190_MODE_CLK_INT_NOTAVAIL
                     | AD7190_MODE_SINC4
                     | AD7190_FS_50HZ_CHOP;
    writeRegister(adc, AD7190_REG_MODE, modeReg, 3);
    delay(2); // Allow internal clock to stabilise
}

// ─── calibrate() ──────────────────────────────────────────────────────────────
void AD7190Driver::calibrate(AD7190Index adc)
{
    // Run calibration on both channels sequentially.
    // The ADC must be in idle/power-down before writing mode for calibration.

    for (int ch = 0; ch < 2; ch++) {
        AD7190Channel channel = (AD7190Channel)ch;

        // Write config for this channel first (sets gain, channel, ref, chop)
        uint32_t confReg = buildConfReg(channel);
        writeRegister(adc, AD7190_REG_CONF, confReg, 3);

        // Internal zero-scale calibration
        uint32_t calZero = AD7190_MODE_INTZCAL
                         | AD7190_MODE_CLK_INT_NOTAVAIL
                         | AD7190_MODE_SINC4
                         | AD7190_FS_50HZ_CHOP;
        writeRegister(adc, AD7190_REG_MODE, calZero, 3);
        // Wait for calibration to complete (RDY goes low then high)
        waitForReady(adc, 1000);

        // Internal full-scale calibration
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
void AD7190Driver::resetDevice(AD7190Index adc)
{
    // AD7190 resets when ≥40 consecutive 1-bits are clocked on DIN
    csSelect(adc);
    _spi.beginTransaction(_spiSettings);
    for (int i = 0; i < 6; i++) {   // 6 bytes × 8 bits = 48 bits of 0xFF
        _spi.transfer(0xFF);
    }
    _spi.endTransaction();
    csDeselect(adc);
    delay(1); // Wait for reset to complete
}

// ─── configureForChannel() ────────────────────────────────────────────────────
void AD7190Driver::configureForChannel(AD7190Index adc, AD7190Channel channel)
{
    // Build and write Configuration Register
    uint32_t confReg = buildConfReg(channel);
    writeRegister(adc, AD7190_REG_CONF, confReg, 3);

    // Build and write Mode Register – single conversion
    // DAT_STA appends 8-bit status after 24-bit data on every data read
    uint32_t modeReg = AD7190_MODE_SINGLE
                     | AD7190_MODE_DAT_STA
                     | AD7190_MODE_CLK_INT_NOTAVAIL
                     | AD7190_MODE_SINC4
                     | AD7190_MODE_REJ60          // Extra 60 Hz notch at 50 Hz ODR
                     | AD7190_FS_50HZ_CHOP;
    writeRegister(adc, AD7190_REG_MODE, modeReg, 3);
    // ADC now starts converting; RDY will assert low when done
}

// ─── buildConfReg() – internal helper ─────────────────────────────────────────
uint32_t AD7190Driver::buildConfReg(AD7190Channel channel)
{
    uint32_t confReg = AD7190_CONF_CHOP        // Enable chopping
                     | AD7190_CONF_BUF         // Buffered inputs
                     | AD7190_CONF_BIPOLAR      // Bipolar mode (load cells go ±)
                     | AD7190_CONF_GAIN_128;    // PGA gain 128

    if (channel == AD7190_CH_LOADCELL1) {
        // Load cell 1: AIN1+/AIN2- with REFIN2+/REFIN2-
        confReg |= AD7190_CONF_CH_AIN1_AIN2;
        confReg |= AD7190_CONF_REFSEL_2;       // Use REFIN2
    } else {
        // Load cell 2: AIN3+/AIN4- with REFIN1+/REFIN1-
        confReg |= AD7190_CONF_CH_AIN3_AIN4;
        confReg |= AD7190_CONF_REFSEL_1;       // Use REFIN1
    }

    return confReg;
}

// ─── waitForReady() ───────────────────────────────────────────────────────────
bool AD7190Driver::waitForReady(AD7190Index adc, uint32_t timeoutMs)
{
    uint32_t start = millis();
    while ((millis() - start) < timeoutMs) {
        // Read status register: RDY bit (bit 7) = 0 means data is ready
        uint8_t status = (uint8_t)(readRegister(adc, AD7190_REG_STATUS, 1) & 0xFF);
        if (!(status & AD7190_STATUS_RDY)) {
            return true;   // Conversion complete
        }
        delayMicroseconds(500); // Avoid hammering SPI; 50 Hz period = 20 ms
    }
    return false;  // Timeout
}

// ─── writeRegister() ──────────────────────────────────────────────────────────
void AD7190Driver::writeRegister(AD7190Index adc, uint8_t reg,
                                  uint32_t value, uint8_t numBytes)
{
    // Communications byte: WEN=0, RD=0 (write), address, CREAD=0
    uint8_t commByte = AD7190_COMM_WEN | AD7190_COMM_WRITE | AD7190_COMM_ADDR(reg);

    csSelect(adc);
    _spi.beginTransaction(_spiSettings);

    _spi.transfer(commByte);

    // Send register bytes MSB first
    for (int8_t b = numBytes - 1; b >= 0; b--) {
        _spi.transfer((value >> (b * 8)) & 0xFF);
    }

    _spi.endTransaction();
    csDeselect(adc);
}

// ─── readRegister() ───────────────────────────────────────────────────────────
uint32_t AD7190Driver::readRegister(AD7190Index adc, uint8_t reg, uint8_t numBytes)
{
    // Communications byte: WEN=0, RD=1 (read), address, CREAD=0
    uint8_t commByte = AD7190_COMM_WEN | AD7190_COMM_READ | AD7190_COMM_ADDR(reg);

    csSelect(adc);
    _spi.beginTransaction(_spiSettings);

    _spi.transfer(commByte);

    uint32_t result = 0;
    for (uint8_t b = 0; b < numBytes; b++) {
        result = (result << 8) | _spi.transfer(0x00);
    }

    _spi.endTransaction();
    csDeselect(adc);

    return result;
}

// ─── csSelect / csDeselect ────────────────────────────────────────────────────
void AD7190Driver::csSelect(AD7190Index adc)
{
    digitalWrite(_csPins[(int)adc], LOW);
    delayMicroseconds(1); // t_CSS setup time
}

void AD7190Driver::csDeselect(AD7190Index adc)
{
    delayMicroseconds(1); // t_CSH hold time
    digitalWrite(_csPins[(int)adc], HIGH);
}

// ─── rawToMillivolts() ────────────────────────────────────────────────────────
double AD7190Driver::rawToMillivolts(uint32_t raw) const
{
    // Bipolar coding: midscale = 0x800000 = 8388608
    // Voltage = ((raw - midscale) / midscale) * (Vref / gain)
    const double midscale = 8388608.0;  // 2^23
    const double gain     = 128.0;
    double       norm     = ((double)raw - midscale) / midscale;
    return norm * (_vrefMv / gain);
}

// ─── recoverADC() ────────────────────────────────────────────────────────
void AD7190Driver::recoverADC(AD7190Index adc)
{
    // Send 64 high bits to exit CREAD/any stuck state
    csSelect(adc);
    _spi.beginTransaction(_spiSettings);
    for (int i = 0; i < 8; i++) _spi.transfer(0xFF);
    _spi.endTransaction();
    csDeselect(adc);
    delay(2);

    // Restore GPOCON (BPDSW) — this is lost on reset
    writeRegister(adc, AD7190_REG_GPOCON, AD7190_GPOCON_BPDSW, 1);

    // Restore mode and config to a known idle state
    // (no conversion running, just idle with correct settings)
    uint32_t modeIdle = AD7190_MODE_IDLE
                      | AD7190_MODE_DAT_STA
                      | AD7190_MODE_CLK_INT_NOTAVAIL
                      | AD7190_MODE_SINC4
                      | AD7190_MODE_REJ60
                      | AD7190_FS_50HZ_CHOP;
    writeRegister(adc, AD7190_REG_MODE, modeIdle, 3);
}
