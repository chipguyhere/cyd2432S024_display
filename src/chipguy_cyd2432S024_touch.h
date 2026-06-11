/*
 * XPT2046 resistive touch driver for the CYD ESP32-2432S024.
 *
 * On this board the XPT2046 SHARES the ILI9341 display's SPI bus (SCK/SDI/SDO),
 * unlike the 2432S028 where it has its own pins.  So instead of bit-banging,
 * this driver attaches the XPT2046 as a second ESP-IDF SPI device on the same
 * bus (SPI2_HOST) with its own chip-select, clocked slowly (well under the
 * XPT2046's ~2.5 MHz limit).  The display driver owns bus initialization; this
 * driver tolerates the bus already being up.
 *
 * Board wiring (fixed on the ESP32-2432S024):
 *   TP_CLK 14   TP_DIN(MOSI) 13   TP_OUT(MISO) 12   (shared with the display)
 *   TP_CS  33   TP_IRQ 36 (active LOW while touched)
 *
 * Resistive panels vary unit-to-unit, so raw ADC readings are mapped to screen
 * pixels through a calibration rectangle.  The defaults work on typical CYDs;
 * call setCalibration() to tune, or readRaw() to gather values for your panel.
 */

#pragma once

#include <Arduino.h>
#include <driver/spi_master.h>

class chipguy_cyd2432S024_touch {
public:
    chipguy_cyd2432S024_touch();

    // Configure the touch GPIOs.  Safe to call before the display is up.
    void begin();

    // Match the touch coordinate mapping to the display rotation, using the
    // same convention as chipguy_cyd2432S024_display: an index 0..3 or degrees
    // 0/90/180/270.
    void setRotation(uint16_t rotation);

    // Override the raw-ADC calibration rectangle.  On this board the raw axes
    // are swapped vs. the screen: xMin/xMax are the raw-X endpoints along the
    // long (320) axis, yMin/yMax the raw-Y endpoints along the short (240) axis
    // (in the unrotated portrait frame).  Usually set via loadCalibration() or
    // produced by the TouchCalibrate example.
    void setCalibration(uint16_t xMin, uint16_t xMax, uint16_t yMin, uint16_t yMax);

    // Read the current calibration rectangle (e.g. to display or re-save it).
    void getCalibration(uint16_t &xMin, uint16_t &xMax,
                        uint16_t &yMin, uint16_t &yMax) const;

    // Load the calibration rectangle from NVS (Preferences).  Returns true and
    // applies it if a saved calibration exists; returns false and leaves the
    // current (default) calibration in place otherwise.  Safe to call in setup.
    bool loadCalibration();

    // Persist the current calibration rectangle to NVS (Preferences) so future
    // boots can loadCalibration() it.
    void saveCalibration();

    // Noise filters applied by read() (not readRaw()):
    //   minPressure - reject touches lighter than this (0 = off).  Compare
    //                 against pressure() values gathered on your panel.
    //   maxJump     - reject a reading that teleports more than this many pixels
    //                 from the previous accepted point (0 = off); brief outliers
    //                 are dropped, a sustained move is still let through.
    void setFilters(uint16_t minPressure, uint16_t maxJump);
    void getFilters(uint16_t &minPressure, uint16_t &maxJump) const;

    // Load/save the filter settings in NVS (Preferences), same store as the
    // calibration.  loadFilters() returns true if saved settings were found.
    bool loadFilters();
    void saveFilters();

    // Pressure of the most recent sample: a position-normalized touch-resistance
    // metric (XPT2046 Z and X), higher = firmer press, and roughly uniform
    // across the panel.  Watch it live (e.g. the readout in TouchCalibrate) to
    // choose a minPressure threshold.
    uint16_t pressure() const { return _lastPressure; }

    // Read the touch point.  Returns true if the screen is currently pressed,
    // with x,y mapped to screen pixels for the active rotation.
    bool read(uint16_t &x, uint16_t &y);

    // Read averaged raw 12-bit ADC values (no calibration/rotation applied).
    // Returns true if pressed.  Useful for gathering calibration numbers.
    bool readRaw(uint16_t &rawX, uint16_t &rawY);

private:
    uint8_t  _rotation = 0;
    uint16_t _xMin, _xMax, _yMin, _yMax;

    // Noise-filter settings and state (see setFilters()).
    uint16_t _minPressure = 0;
    uint16_t _maxJump = 0;
    uint16_t _lastPressure = 0;
    uint16_t _lastX = 0, _lastY = 0;
    bool     _hasLast = false;
    uint8_t  _jumpCount = 0;

    spi_device_handle_t _spi = nullptr;   // XPT2046 on the shared display bus

    uint16_t _readChannel12(uint8_t cmd);
    bool     _readRawSample(uint16_t &rawX, uint16_t &rawY);
};
