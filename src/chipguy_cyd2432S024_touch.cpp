/*
 * XPT2046 resistive touch driver for the CYD ESP32-2432S024.
 * See chipguy_cyd2432S024_touch.h for wiring and usage.
 */

#include "chipguy_cyd2432S024_touch.h"
#include "chipguy_cyd2432S024_display.h"   // for CYD_TFT_NATIVE_WIDTH/HEIGHT

#include <driver/spi_master.h>
#include <Preferences.h>

// NVS namespace + keys for the persisted calibration rectangle.
#define CYD_TOUCH_NVS_NS   "cydtouch"
#define CYD_TOUCH_NVS_XMIN "xmin"
#define CYD_TOUCH_NVS_XMAX "xmax"
#define CYD_TOUCH_NVS_YMIN "ymin"
#define CYD_TOUCH_NVS_YMAX "ymax"
#define CYD_TOUCH_NVS_MINP "minp"   // minimum pressure filter
#define CYD_TOUCH_NVS_JUMP "jump"   // max jump (px) filter

// ── Board pin assignments (fixed on the ESP32-2432S024) ────────────────────
// SCK/SDI/SDO (14/13/12) are shared with the display; the display driver owns
// bus initialization on this host, so here we only add the XPT2046 as a device.
#define CYD_TOUCH_SCLK  14   // shared with the display SCK
#define CYD_TOUCH_MOSI  13   // TP_DIN, shared with the display SDI
#define CYD_TOUCH_MISO  12   // TP_OUT, shared with the display SDO
#define CYD_TOUCH_CS    33   // dedicated touch chip-select
#define CYD_TOUCH_IRQ   36   // active LOW while the panel is being touched

// Same SPI host the display driver brings up.  spi_bus_initialize here is
// idempotent — whichever driver runs first wins and the other tolerates
// ESP_ERR_INVALID_STATE.
#define CYD_TOUCH_SPI_HOST  SPI2_HOST   // HSPI, shared with the display
#define CYD_TOUCH_SPI_HZ    2000000     // 2 MHz, well under the XPT2046's limit

// XPT2046 control bytes: start bit + channel select, 12-bit, differential.
#define XPT2046_CMD_X   0xD0   // read X position
#define XPT2046_CMD_Y   0x90   // read Y position
#define XPT2046_CMD_Z1  0xB0   // read Z1 (pressure)
#define XPT2046_CMD_Z2  0xC0   // read Z2 (pressure)

// Scale for the position-normalized pressure metric (see _readRawSample).  It
// just sets the numeric range of pressure() / the minPressure slider; raise it
// if firm presses read too low, lower it if they saturate at 4095.
#define CYD_TOUCH_P_SCALE  100.0f

// Default raw-ADC calibration rectangle (typical CYD; tune with readRaw()).
#define CYD_TOUCH_X_MIN  200
#define CYD_TOUCH_X_MAX  3900
#define CYD_TOUCH_Y_MIN  200
#define CYD_TOUCH_Y_MAX  3900

// Number of samples averaged per read to smooth resistive jitter.
static const int CYD_TOUCH_SAMPLES = 4;

chipguy_cyd2432S024_touch::chipguy_cyd2432S024_touch()
    : _xMin(CYD_TOUCH_X_MIN), _xMax(CYD_TOUCH_X_MAX),
      _yMin(CYD_TOUCH_Y_MIN), _yMax(CYD_TOUCH_Y_MAX) {}

void chipguy_cyd2432S024_touch::begin() {
    // IRQ is GPIO36, an input-only pin with NO internal pull resistor; the
    // XPT2046's PENIRQ drives it high when idle, low when touched.
    pinMode(CYD_TOUCH_IRQ, INPUT);

    // Make sure the shared SPI bus exists.  The display driver normally brings
    // it up first; if touch.begin() runs first we initialize it here, and if it
    // was already initialized spi_bus_initialize returns ESP_ERR_INVALID_STATE,
    // which we ignore.  The pin config must match the display's bus config.
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = CYD_TOUCH_MOSI;
    buscfg.miso_io_num = CYD_TOUCH_MISO;
    buscfg.sclk_io_num = CYD_TOUCH_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    esp_err_t err = spi_bus_initialize(CYD_TOUCH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("cyd touch: spi_bus_initialize failed: 0x%x\n", err);
        return;
    }

    // Add the XPT2046 as its own slow device on the bus, with hardware-managed
    // CS.  Mode 0, MSB first; the ESP-IDF driver arbitrates the bus with the
    // display device for us.
    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = CYD_TOUCH_SPI_HZ;
    devcfg.mode = 0;
    devcfg.spics_io_num = CYD_TOUCH_CS;   // hardware-managed CS
    devcfg.queue_size = 1;
    err = spi_bus_add_device(CYD_TOUCH_SPI_HOST, &devcfg, &_spi);
    if (err != ESP_OK) {
        Serial.printf("cyd touch: spi_bus_add_device failed: 0x%x\n", err);
        _spi = nullptr;
    }
}

void chipguy_cyd2432S024_touch::setRotation(uint16_t rotation) {
    _rotation = cyd_normalize_rotation(rotation);
}

void chipguy_cyd2432S024_touch::setCalibration(uint16_t xMin, uint16_t xMax,
                                               uint16_t yMin, uint16_t yMax) {
    _xMin = xMin; _xMax = xMax;
    _yMin = yMin; _yMax = yMax;
}

void chipguy_cyd2432S024_touch::getCalibration(uint16_t &xMin, uint16_t &xMax,
                                               uint16_t &yMin, uint16_t &yMax) const {
    xMin = _xMin; xMax = _xMax;
    yMin = _yMin; yMax = _yMax;
}

bool chipguy_cyd2432S024_touch::loadCalibration() {
    Preferences prefs;
    if (!prefs.begin(CYD_TOUCH_NVS_NS, /*readOnly=*/true)) return false;
    // Treat the calibration as present only if all four keys exist.
    bool ok = prefs.isKey(CYD_TOUCH_NVS_XMIN) && prefs.isKey(CYD_TOUCH_NVS_XMAX) &&
              prefs.isKey(CYD_TOUCH_NVS_YMIN) && prefs.isKey(CYD_TOUCH_NVS_YMAX);
    if (ok) {
        _xMin = prefs.getUShort(CYD_TOUCH_NVS_XMIN, _xMin);
        _xMax = prefs.getUShort(CYD_TOUCH_NVS_XMAX, _xMax);
        _yMin = prefs.getUShort(CYD_TOUCH_NVS_YMIN, _yMin);
        _yMax = prefs.getUShort(CYD_TOUCH_NVS_YMAX, _yMax);
    }
    prefs.end();
    return ok;
}

void chipguy_cyd2432S024_touch::saveCalibration() {
    Preferences prefs;
    if (!prefs.begin(CYD_TOUCH_NVS_NS, /*readOnly=*/false)) return;
    prefs.putUShort(CYD_TOUCH_NVS_XMIN, _xMin);
    prefs.putUShort(CYD_TOUCH_NVS_XMAX, _xMax);
    prefs.putUShort(CYD_TOUCH_NVS_YMIN, _yMin);
    prefs.putUShort(CYD_TOUCH_NVS_YMAX, _yMax);
    prefs.end();
}

void chipguy_cyd2432S024_touch::setFilters(uint16_t minPressure, uint16_t maxJump) {
    _minPressure = minPressure;
    _maxJump = maxJump;
}

void chipguy_cyd2432S024_touch::getFilters(uint16_t &minPressure, uint16_t &maxJump) const {
    minPressure = _minPressure;
    maxJump = _maxJump;
}

bool chipguy_cyd2432S024_touch::loadFilters() {
    Preferences prefs;
    if (!prefs.begin(CYD_TOUCH_NVS_NS, /*readOnly=*/true)) return false;
    bool ok = prefs.isKey(CYD_TOUCH_NVS_MINP) && prefs.isKey(CYD_TOUCH_NVS_JUMP);
    if (ok) {
        _minPressure = prefs.getUShort(CYD_TOUCH_NVS_MINP, _minPressure);
        _maxJump     = prefs.getUShort(CYD_TOUCH_NVS_JUMP, _maxJump);
    }
    prefs.end();
    return ok;
}

void chipguy_cyd2432S024_touch::saveFilters() {
    Preferences prefs;
    if (!prefs.begin(CYD_TOUCH_NVS_NS, /*readOnly=*/false)) return;
    prefs.putUShort(CYD_TOUCH_NVS_MINP, _minPressure);
    prefs.putUShort(CYD_TOUCH_NVS_JUMP, _maxJump);
    prefs.end();
}

// One XPT2046 conversion over the shared SPI bus: clock out the 8-bit command
// and read back the 12-bit result.  We send a 3-byte full-duplex transaction
// (command byte + two dummy bytes); the 12-bit result lands in the upper bits
// of rx[1..2] (the first busy bit follows the command).  Mode 0, MSB first.
uint16_t chipguy_cyd2432S024_touch::_readChannel12(uint8_t cmd) {
    if (!_spi) return 0;
    spi_transaction_t t = {};
    uint8_t tx[3] = { cmd, 0x00, 0x00 };
    uint8_t rx[3] = { 0, 0, 0 };
    t.length = 3 * 8;            // bits, clocked in both directions
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    if (spi_device_polling_transmit(_spi, &t) != ESP_OK) return 0;

    // Result is the 12 bits after the command byte's busy bit: bits 14..3 of
    // the 16-bit big-endian word formed by rx[1],rx[2].
    return (uint16_t)(((rx[1] << 8) | rx[2]) >> 3) & 0x0FFF;
}

bool chipguy_cyd2432S024_touch::_readRawSample(uint16_t &rawX, uint16_t &rawY) {
    if (!_spi) return false;
    // IRQ is pulled low by the controller only while the panel is pressed.
    if (digitalRead(CYD_TOUCH_IRQ) != LOW) return false;

    // CS is hardware-managed per transaction by the ESP-IDF driver, which also
    // arbitrates the bus against the display device.
    uint32_t sumX = 0, sumY = 0;
    for (int i = 0; i < CYD_TOUCH_SAMPLES; i++) {
        sumX += _readChannel12(XPT2046_CMD_X);
        sumY += _readChannel12(XPT2046_CMD_Y);
    }

    // Pressure via the XPT2046 datasheet touch-resistance formula:
    //   R_touch ∝ (xpos / 4096) * (z2/z1 - 1)
    // The (xpos/4096) term divides out the touch position, so the result is
    // roughly uniform across the panel — unlike the raw z1+4096-z2 proxy, which
    // reads much lighter near the low-rawX edge.  We report its inverse
    // (conductance), scaled, so higher = firmer press (matching minPressure).
    uint16_t z1 = _readChannel12(XPT2046_CMD_Z1);
    uint16_t z2 = _readChannel12(XPT2046_CMD_Z2);
    uint16_t xpos = (uint16_t)(sumX / CYD_TOUCH_SAMPLES);   // X position for the formula
    float p = 0.0f;
    if (z1 > 0 && xpos > 0) {
        if (z2 <= z1) {
            p = 4095.0f;                       // resistance ~0 (or noise): treat as firmest
        } else {
            float r = ((float)xpos / 4096.0f) * ((float)z2 / (float)z1 - 1.0f);
            p = (r > 0.0f) ? CYD_TOUCH_P_SCALE / r : 4095.0f;
        }
    }
    _lastPressure = (uint16_t)(p > 4095.0f ? 4095.0f : p);

    // Still pressed after sampling?  Rejects spurious single-edge IRQs.
    if (digitalRead(CYD_TOUCH_IRQ) != LOW) return false;

    rawX = sumX / CYD_TOUCH_SAMPLES;
    rawY = sumY / CYD_TOUCH_SAMPLES;
    return true;
}

bool chipguy_cyd2432S024_touch::readRaw(uint16_t &rawX, uint16_t &rawY) {
    return _readRawSample(rawX, rawY);
}

bool chipguy_cyd2432S024_touch::read(uint16_t &x, uint16_t &y) {
    uint16_t rawX, rawY;
    if (!_readRawSample(rawX, rawY)) {
        _hasLast = false;       // released: forget the last point for jump filter
        _jumpCount = 0;
        return false;
    }

    // Pressure filter: reject touches lighter than the configured threshold.
    if (_minPressure > 0 && _lastPressure < _minPressure) return false;

    // The touch panel is mounted the same 90°-rotated way as the glass, so the
    // XPT2046 axes are SWAPPED relative to the display: raw X spans the long
    // (320) screen axis, raw Y the short (240) axis.  Map into the upright
    // portrait (rotation 0) frame: sx in [0,239], sy in [0,319].
    long sx = map((long)constrain(rawY, _yMin, _yMax), _yMin, _yMax,
                  0, CYD_TFT_NATIVE_WIDTH - 1);
    long sy = map((long)constrain(rawX, _xMin, _xMax), _xMin, _xMax,
                  0, CYD_TFT_NATIVE_HEIGHT - 1);

    // Per-axis direction correction for this board.  The short (240) axis runs
    // right->left so it's flipped; the long (320) axis is already correct.  If a
    // corner test shows an axis running backwards, toggle the matching line.
    sx = (CYD_TFT_NATIVE_WIDTH - 1) - sx;    // 240 axis: raw Y increases right->left
    // sy is NOT flipped — the 320 axis already increases top->bottom.

    const long W = CYD_TFT_NATIVE_WIDTH - 1;    // 239
    const long H = CYD_TFT_NATIVE_HEIGHT - 1;   // 319

    // Build each rotation from the upright portrait (sx,sy) base, matching the
    // display's rotation convention.
    switch (_rotation) {
        case 0:  x = sx;          y = sy;          break;  // portrait (upright)
        case 1:  x = sy;          y = W - sx;      break;  // landscape
        case 2:  x = W - sx;      y = H - sy;      break;  // portrait flipped
        case 3:  x = H - sy;      y = sx;          break;  // landscape flipped
    }

    // Jump filter: drop a reading that teleports more than _maxJump pixels from
    // the last accepted point.  A real fast drag survives because we let the
    // reading through after a couple of consecutive "jumps".
    if (_maxJump > 0 && _hasLast) {
        long d = labs((long)x - _lastX) + labs((long)y - _lastY);
        if (d > _maxJump && ++_jumpCount < 3) return false;
    }
    _jumpCount = 0;
    _lastX = x; _lastY = y; _hasLast = true;
    return true;
}
