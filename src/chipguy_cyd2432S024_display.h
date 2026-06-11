/*
 * ILI9341 SPI display driver for the CYD ESP32-2432S024 ("Cheap Yellow Display")
 *
 * Drives the 240x320 ILI9341 panel through the ESP-IDF SPI master driver
 * (every transaction goes through spi_device_polling_transmit, so the bus is
 * shared cleanly with any other ESP-IDF SPI peripheral).  Designed to feed an
 * LVGL display in PARTIAL render mode: LVGL hands us a rendered region and we
 * push it with flushRegion().
 *
 * Board wiring (fixed on the ESP32-2432S024):
 *   TFT_SDI(MOSI) 13   TFT_SCK 14   TFT_CS 15   TFT_RS(DC) 2   TFT_SDO(MISO) 12
 *   TFT_BL   27 (backlight, active HIGH)
 *
 * Unlike the 2432S028, this board has NO dedicated TFT reset line — the panel
 * is brought up with the ILI9341 software reset (0x01).  It also shares its SPI
 * bus (SCK 14 / SDI 13 / SDO 12) with the XPT2046 touch controller, which is
 * registered as a second device on the same bus (its own CS) by the companion
 * chipguy_cyd2432S024_touch driver.
 */

#pragma once

#include <Arduino.h>
#include <driver/spi_master.h>

// Native panel geometry (portrait).  width()/height() reflect the active
// rotation; these are the unrotated physical dimensions.
#define CYD_TFT_NATIVE_WIDTH   240
#define CYD_TFT_NATIVE_HEIGHT  320

// Normalize a rotation argument to an index 0..3.  Accepts either the index
// directly (0..3) or degrees (0/90/180/270) as aliases for 0/1/2/3.
static inline uint8_t cyd_normalize_rotation(uint16_t rotation) {
    switch (rotation) {
        case 90:  return 1;
        case 180: return 2;
        case 270: return 3;
        default:  return (uint8_t)(rotation & 0x03);
    }
}

class chipguy_cyd2432S024_display {
public:
    chipguy_cyd2432S024_display();

    // Initialize the SPI bus, the ILI9341 panel, and the backlight.
    // rotation: 0 = portrait (240x320, native), 1 = landscape (320x240),
    //           2 = portrait flipped, 3 = landscape flipped.  Degrees
    //           (0/90/180/270) are accepted as aliases for 0/1/2/3.
    // Returns false if the SPI bus/device could not be set up.
    bool begin(uint16_t rotation = 0);

    // Change rotation after begin().  Updates width()/height() and the
    // panel's MADCTL.  Accepts an index 0..3 or degrees 0/90/180/270.
    void setRotation(uint16_t rotation);

    // Logical dimensions for the current rotation.
    int16_t width()  const { return _w; }
    int16_t height() const { return _h; }

    // Current rotation (0..3).
    uint8_t rotation() const { return _rotation; }

    // Backlight brightness, 0..100 (active HIGH on this board).
    void setBacklight(int percentage);

    // Push a rectangular region of RGB565 pixels to the panel.  Coordinates
    // are inclusive.  `pixels` is in LVGL's native little-endian byte order;
    // this call byte-swaps it in place to the big-endian order the ILI9341
    // expects, so the buffer is modified.
    //
    // Asynchronous: it byte-swaps, waits for the PREVIOUS flush's DMA to finish,
    // sends the address window, then QUEUES this region's pixel DMA and returns
    // before it completes.  With double buffering, the caller can render the
    // next region into the other buffer while this one transmits.  `pixels`
    // must stay valid and untouched until the next flushRegion()/waitFlush().
    void flushRegion(int x1, int y1, int x2, int y2, uint16_t *pixels);

    // Block until the most recently queued flush has finished transmitting.
    // Call before tearing down or doing direct drawing after a flush.
    void waitFlush();

    // Fill the whole screen with one RGB565 color (handy for bring-up tests).
    void fillScreen(uint16_t color);

private:
    spi_device_handle_t _spi = nullptr;
    int16_t _w = CYD_TFT_NATIVE_WIDTH;
    int16_t _h = CYD_TFT_NATIVE_HEIGHT;
    uint8_t _rotation = 0;

    spi_transaction_t _pixelTrans = {};   // persistent: queued DMA reads it async
    bool _pixelBusy = false;              // true while a queued flush is in flight

    void _cmd(uint8_t cmd);
    void _data(const uint8_t *data, size_t len);
    void _data8(uint8_t val);
    void _initPanel();
    void _setAddrWindow(int x0, int y0, int x1, int y1);
    void _pushPixels(const uint8_t *data, size_t bytes);
    void _waitPixelDone();
};
