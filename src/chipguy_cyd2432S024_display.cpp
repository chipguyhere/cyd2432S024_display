/*
 * ILI9341 SPI display driver for the CYD ESP32-2432S024.
 * See chipguy_cyd2432S024_display.h for the wiring and usage overview.
 */

#include "chipguy_cyd2432S024_display.h"
#include <driver/gpio.h>
#include <esp_heap_caps.h>

// ── Board pin assignments (fixed on the ESP32-2432S024) ────────────────────
// MISO (SDO, GPIO12) is on the bus only so the XPT2046 touch controller can
// share it; the display itself is write-only and never reads it.  This board
// has NO dedicated TFT reset line — _initPanel uses the software reset (0x01).
#define CYD_TFT_MOSI  13   // SDI
#define CYD_TFT_MISO  12   // SDO — present for the shared XPT2046 touch device
#define CYD_TFT_SCLK  14   // SCK
#define CYD_TFT_CS    15
#define CYD_TFT_DC     2   // RS
#define CYD_TFT_BL    27   // backlight, active HIGH

// The display shares this SPI bus with the XPT2046 touch controller (a separate
// ESP-IDF device with its own CS, clocked far slower).  The ILI9341 still
// tolerates 40 MHz comfortably over the short CYD traces.
#define CYD_TFT_SPI_HZ  40000000
#define CYD_TFT_SPI_HOST  SPI2_HOST   // HSPI

// Color order base for MADCTL.  Unlike the 2.8" board, the 2.4" panel reads as
// RGB (0x00) — with the BGR bit set, red/blue come out swapped.  If your unit
// shows swapped colors, flip this between 0x00 (RGB) and 0x08 (BGR).
// Rotation bits (MY/MX/MV) are OR'd in per rotation.
#define CYD_MADCTL_COLOR   0x00   // 0x00 = RGB, 0x08 = BGR
#define ILI9341_MADCTL_MY  0x80
#define ILI9341_MADCTL_MX  0x40
#define ILI9341_MADCTL_MV  0x20

// Largest single SPI transfer we size DMA descriptors for: a full screen, so
// any partial flush region fits in one queued transaction.
static const size_t CYD_MAX_TRANSFER =
    (size_t)CYD_TFT_NATIVE_WIDTH * CYD_TFT_NATIVE_HEIGHT * 2;

// CS is driven by the SPI peripheral (hardware CS), but DC is a plain GPIO.
// The driver calls this just before each transaction goes on the bus — for
// queued transactions that's in ISR context — and we set DC from the
// transaction's `user` field: 0 = command, 1 = data.
static void _cyd_spi_pre_cb(spi_transaction_t *t) {
    gpio_set_level((gpio_num_t)CYD_TFT_DC, (int)(intptr_t)t->user);
}

chipguy_cyd2432S024_display::chipguy_cyd2432S024_display() {}

// ── Low-level SPI helpers ──────────────────────────────────────────────────
// DC and CS are handled by the pre-callback and hardware CS respectively, so
// these just submit the bytes.  All command/parameter traffic is synchronous
// (polling); only the pixel blit is queued asynchronously (see flushRegion).

void chipguy_cyd2432S024_display::_cmd(uint8_t cmd) {
    if (!_spi) return;
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_data[0] = cmd;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.user = (void *)0;          // DC = command
    spi_device_polling_transmit(_spi, &t);
}

void chipguy_cyd2432S024_display::_data(const uint8_t *data, size_t len) {
    if (!_spi || len == 0) return;
    spi_transaction_t t = {};
    t.length = len * 8;
    t.tx_buffer = data;
    t.user = (void *)1;          // DC = data
    spi_device_polling_transmit(_spi, &t);
}

void chipguy_cyd2432S024_display::_data8(uint8_t val) {
    _data(&val, 1);
}

// ── ILI9341 power-on init sequence ─────────────────────────────────────────
// Standard ILI9341 startup (gamma, power, frame rate).  MADCTL is written
// here as a placeholder and then re-issued by setRotation().
//
// This board has no hardware reset line, so we begin with a software reset
// (0x01) and let the controller settle before the rest of the sequence.

void chipguy_cyd2432S024_display::_initPanel() {
    _cmd(0x01);                            // Software reset
    delay(150);
    _cmd(0xEF); { const uint8_t d[] = {0x03, 0x80, 0x02}; _data(d, 3); }
    _cmd(0xCF); { const uint8_t d[] = {0x00, 0xC1, 0x30}; _data(d, 3); }
    _cmd(0xED); { const uint8_t d[] = {0x64, 0x03, 0x12, 0x81}; _data(d, 4); }
    _cmd(0xE8); { const uint8_t d[] = {0x85, 0x00, 0x78}; _data(d, 3); }
    _cmd(0xCB); { const uint8_t d[] = {0x39, 0x2C, 0x00, 0x34, 0x02}; _data(d, 5); }
    _cmd(0xF7); _data8(0x20);
    _cmd(0xEA); { const uint8_t d[] = {0x00, 0x00}; _data(d, 2); }
    _cmd(0xC0); _data8(0x23);              // Power control 1
    _cmd(0xC1); _data8(0x10);              // Power control 2
    _cmd(0xC5); { const uint8_t d[] = {0x3E, 0x28}; _data(d, 2); }  // VCOM 1
    _cmd(0xC7); _data8(0x86);              // VCOM 2
    _cmd(0x36); _data8(CYD_MADCTL_COLOR);  // MADCTL (overwritten by setRotation)
    _cmd(0x3A); _data8(0x55);              // Pixel format: 16-bit RGB565
    _cmd(0xB1); { const uint8_t d[] = {0x00, 0x18}; _data(d, 2); }  // Frame rate
    _cmd(0xB6); { const uint8_t d[] = {0x08, 0x82, 0x27}; _data(d, 3); } // Display func
    _cmd(0xF2); _data8(0x00);              // 3Gamma disable
    _cmd(0x26); _data8(0x01);              // Gamma curve selected
    _cmd(0xE0); { const uint8_t d[] = {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E,
                  0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}; _data(d, 15); }
    _cmd(0xE1); { const uint8_t d[] = {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31,
                  0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}; _data(d, 15); }
    _cmd(0x11);                            // Sleep out
    delay(120);
    _cmd(0x29);                            // Display on
    delay(20);
}

void chipguy_cyd2432S024_display::_setAddrWindow(int x0, int y0, int x1, int y1) {
    uint8_t col[4] = {
        (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF)
    };
    _cmd(0x2A);              // CASET — Column Address Set
    _data(col, 4);

    uint8_t row[4] = {
        (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
        (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF)
    };
    _cmd(0x2B);              // RASET — Row Address Set
    _data(row, 4);

    _cmd(0x2C);              // RAMWR — Memory Write
}

// Synchronous (blocking) pixel push, used by fillScreen().  The interactive
// path uses the queued/async flushRegion() instead.
void chipguy_cyd2432S024_display::_pushPixels(const uint8_t *data, size_t bytes) {
    if (!_spi || bytes == 0) return;
    spi_transaction_t t = {};
    t.length = bytes * 8;
    t.tx_buffer = data;
    t.user = (void *)1;          // DC = data
    spi_device_polling_transmit(_spi, &t);
}

// Block until the in-flight async pixel DMA (if any) has completed, so the bus
// is idle before we touch it directly.
void chipguy_cyd2432S024_display::_waitPixelDone() {
    if (_pixelBusy) {
        spi_transaction_t *result = nullptr;
        spi_device_get_trans_result(_spi, &result, portMAX_DELAY);
        _pixelBusy = false;
    }
}

// ── Public API ─────────────────────────────────────────────────────────────

bool chipguy_cyd2432S024_display::begin(uint16_t rotation) {
    // DC is driven by the pre-transaction callback; configure it as output.
    // CS is owned by the SPI peripheral (hardware CS), so we don't touch it.
    // This board has no TFT reset line — the panel is reset in software by
    // _initPanel (0x01), so there is no reset pulse here.
    gpio_set_direction((gpio_num_t)CYD_TFT_DC, GPIO_MODE_OUTPUT);

    // Register the SPI bus with the ESP-IDF master driver.  MISO is included so
    // the XPT2046 touch driver can attach to this same bus as a second device.
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = CYD_TFT_MOSI;
    buscfg.miso_io_num = CYD_TFT_MISO;  // shared with the XPT2046 touch device
    buscfg.sclk_io_num = CYD_TFT_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = CYD_MAX_TRANSFER;

    esp_err_t err = spi_bus_initialize(CYD_TFT_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("cyd display: spi_bus_initialize failed: 0x%x\n", err);
        return false;
    }

    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = CYD_TFT_SPI_HZ;
    devcfg.mode = 0;
    devcfg.spics_io_num = CYD_TFT_CS;   // hardware-managed CS
    devcfg.queue_size = 1;              // one async pixel transfer in flight
    devcfg.pre_cb = _cyd_spi_pre_cb;    // sets DC from each transaction's user flag
    devcfg.flags = SPI_DEVICE_NO_DUMMY;

    err = spi_bus_add_device(CYD_TFT_SPI_HOST, &devcfg, &_spi);
    if (err != ESP_OK) {
        Serial.printf("cyd display: spi_bus_add_device failed: 0x%x\n", err);
        _spi = nullptr;
        return false;
    }

    _initPanel();
    setRotation(rotation);

    // Backlight on at full brightness.
    setBacklight(100);

    return true;
}

void chipguy_cyd2432S024_display::setRotation(uint16_t rotation) {
    _waitPixelDone();           // bus must be idle before we send a command
    _rotation = cyd_normalize_rotation(rotation);
    uint8_t madctl = CYD_MADCTL_COLOR;

    // The 2.4" glass is mounted 90° rotated from the 2.8" board, so the MV
    // (row/column exchange) bit is INVERTED relative to the standard ILI9341
    // table: portrait sets MV (with a 240-wide address window), landscape
    // clears it (320-wide).  See the diagnostic notes in the README.
    switch (_rotation) {
        case 0:  // portrait, native (upright)
            madctl |= ILI9341_MADCTL_MV | ILI9341_MADCTL_MX | ILI9341_MADCTL_MY;
            _w = CYD_TFT_NATIVE_WIDTH;
            _h = CYD_TFT_NATIVE_HEIGHT;
            break;
        case 1:  // landscape
            madctl |= ILI9341_MADCTL_MY;
            _w = CYD_TFT_NATIVE_HEIGHT;
            _h = CYD_TFT_NATIVE_WIDTH;
            break;
        case 2:  // portrait flipped (180° of rotation 0)
            madctl |= ILI9341_MADCTL_MV;
            _w = CYD_TFT_NATIVE_WIDTH;
            _h = CYD_TFT_NATIVE_HEIGHT;
            break;
        case 3:  // landscape flipped (180° of rotation 1)
            madctl |= ILI9341_MADCTL_MX;
            _w = CYD_TFT_NATIVE_HEIGHT;
            _h = CYD_TFT_NATIVE_WIDTH;
            break;
    }

    _cmd(0x36);          // MADCTL
    _data8(madctl);
}

void chipguy_cyd2432S024_display::setBacklight(int percentage) {
    if (percentage < 0)   percentage = 0;
    if (percentage > 100) percentage = 100;
    // Active HIGH: higher duty = brighter.
    analogWrite(CYD_TFT_BL, percentage * 255 / 100);
}

// Asynchronous flush.  Returns once this region's pixel DMA has been *queued*,
// not when it finishes — the caller (LVGL via lv_setup) is then free to render
// the next region into the other buffer while this one transmits.  The bus is
// serialized by blocking on the *previous* flush's DMA at the top of this one.
void chipguy_cyd2432S024_display::flushRegion(int x1, int y1, int x2, int y2,
                                              uint16_t *pixels) {
    if (!_spi) return;

    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    size_t count = (size_t)w * h;

    // The swap below mutates THIS region's buffer.  With double buffering LVGL
    // hands us a different buffer than the one the previous flush is still
    // DMA'ing, so the swap safely overlaps that transfer.  Guard the (not
    // expected) case where we're handed the in-flight buffer: wait first.
    if (_pixelBusy && (const void *)pixels == _pixelTrans.tx_buffer) {
        _waitPixelDone();
    }

    // LVGL renders RGB565 little-endian (native ESP32); the ILI9341 expects
    // big-endian (MSB first).  Swap in place — overlaps the previous region's
    // in-flight DMA (different buffer) for free.
    for (size_t i = 0; i < count; i++) {
        pixels[i] = __builtin_bswap16(pixels[i]);
    }

    // Now wait for the previous flush's DMA to finish, so the SPI bus is free
    // for this region's address window and pixel transfer.
    _waitPixelDone();

    // Address window (synchronous commands), then queue the pixel blit.
    _setAddrWindow(x1, y1, x2, y2);

    _pixelTrans = {};
    _pixelTrans.length = count * 16;        // bits (2 bytes per pixel)
    _pixelTrans.tx_buffer = pixels;
    _pixelTrans.user = (void *)1;           // DC = data
    if (spi_device_queue_trans(_spi, &_pixelTrans, portMAX_DELAY) == ESP_OK) {
        _pixelBusy = true;
    }
}

void chipguy_cyd2432S024_display::waitFlush() {
    _waitPixelDone();
}

void chipguy_cyd2432S024_display::fillScreen(uint16_t color) {
    if (!_spi) return;
    _waitPixelDone();           // drain any queued async flush first
    uint16_t be = __builtin_bswap16(color);

    // One-row buffer pushed for every row.  Must be DMA-capable: the ESP-IDF
    // SPI driver requires DMA memory for transfers larger than the FIFO.
    uint16_t *line = (uint16_t *)heap_caps_malloc((size_t)_w * 2, MALLOC_CAP_DMA);
    if (!line) return;
    for (int i = 0; i < _w; i++) line[i] = be;

    _setAddrWindow(0, 0, _w - 1, _h - 1);
    for (int y = 0; y < _h; y++) {
        _pushPixels((const uint8_t *)line, (size_t)_w * 2);
    }
    heap_caps_free(line);
}
