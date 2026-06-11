# chipguy_cyd2432S024_display

A self-contained **display + touch** driver for the **CYD ESP32-2432S024[R]**
("Cheap Yellow Display") — a low-cost ESP32 dev board with a 2.4" 240×320 SPI
LCD and a resistive touch panel. It drives the display and the
touch controller so you don't have to think about either: the
hardware is fixed and fully handled, leaving you to write an **LVGL 9** app and
nothing else.

The two bundled examples are the heart of the library. Each is a complete,
working **starting point** — the display, touch, and LVGL are already wired up
and running before your code gets control. You pick the one that matches how you
want to build your UI, copy it, and start replacing its placeholder UI with your
own.

> This is for the 2.4 inch **2432S024** / **ESP32-024** version of the board.  The more common
> 2.8 inch **2432S028** CYD is wired differently and needs a different library. There are
> also CYD variants with different controllers (e.g. ST7789); those need a different library.
> 

---

## Installation

This library is distributed by downloading the code as a ZIP from GitHub —
there is no Arduino Library Manager entry. To install:

1. On the GitHub project page, click **Code ▸ Download ZIP**.
2. Unzip it, and move the resulting folder into your Arduino **libraries**
   directory:
   - macOS / Linux: `~/Documents/Arduino/libraries/`
   - Windows: `Documents\Arduino\libraries\`
3. Restart Arduino IDE so the examples will appear in the File menu.

(Alternatively, in the IDE: **Sketch ▸ Include Library ▸ Add .ZIP Library…**
and select the downloaded ZIP.)

### Dependency: LVGL 9

The examples need **LVGL 9**. Install it via the Arduino
**Library Manager** (search "lvgl"). Each example sketch folder ships its own
`lv_conf.h`; LVGL reads its configuration from that file, so keep it next to the
`.ino`.

This library was developed against LVGL 9.3 (the current version of LVGL that
is compatible with Squareline) but is very likely compatible with later versions.

### Arduino IDE board settings

The current ESP32 Arduino core ships a board definition for this exact board.
In **Tools ▸ Board ▸ esp32**, select **"ESP32-2432S024R CYD"** — it configures
the board specifics for you, so there's nothing else to set up. The examples
fit comfortably in the default partition.

---

## The two examples — pick your starting point

Open them from **File ▸ Examples ▸ chipguy_cyd2432S024_display**. Both come up
running on the hardware immediately, so you can confirm your board works before
you change a line. They differ only in **how you author the UI**.

To turn either one into your own project, **save it under a new name**
(File ▸ Save As) so your work lives outside the library, then start editing as
described below. Each example folder is self-contained — it carries its own
`lv_setup.hpp` and `lv_conf.h` — so a copied sketch is fully standalone.

### LvglClaudeCodeStub — write the UI yourself (by hand or with an AI)

Use this when you want to build the interface **in code**: directly, or by
handing the sketch to an AI coding assistant such as Claude Code.

The UI lives in a tiny `ui.h` / `ui.cpp` pair in the sketch folder. All the
hardware/LVGL setup happens in the sketch before `ui_init()` is called, so
`ui_init()` is a blank canvas: it receives a live LVGL screen and you create
widgets on it. To start your app:

1. Open `ui.cpp` and look at `ui_init()`. The demo it ships with (a label that
   follows your finger, plus a color-cycling rectangle) is just there to prove
   the display and touch work — **delete it.**
2. In its place, build your own UI on `lv_screen_active()` using normal LVGL
   calls (`lv_label_create`, `lv_button_create`, event callbacks, …).
3. As your app grows, add more screens and helper functions, and declare the
   ones the sketch needs in `ui.h`.

Because everything except the UI is already wired up, this scaffold is ideal to
hand to **Claude Code**: point it at the sketch folder and describe the app you
want — it can focus entirely on `ui.cpp` and the screens you add, without
touching display/touch/LVGL plumbing.

**Reach for this one** when you want full control, a tiny footprint, or
AI-assisted/hand-written UI code.

### Lvgl93SquarelineLauncher — design the UI visually in SquareLine Studio

Use this when you'd rather **design the interface visually** in
[SquareLine Studio](https://squareline.io/) (a drag-and-drop LVGL UI editor) and
just run the result on the CYD.

The sketch's `src/` folder holds a small **placeholder** UI in the exact file
shape a SquareLine export produces (`ui.c`, `ui_Screen1.c`, …). The sketch calls
the `ui_init()` that SquareLine generates. To start your app:

1. In SquareLine Studio, create a project sized to your orientation
   (**240×320** for portrait, 320×240 for landscape), **16-bit** color, with **no
   rotation or offset** — this library handles rotation, so keep the project
   unrotated.
2. Lay out your screens visually.
3. **Export ▸ UI files** (the "Arduino TFT_eSPI profile" works, among others).
4. **Replace the whole `src/` folder** with your export. Build and upload —
   `ui_init()` now runs your design.

Treat `src/` as disposable: you overwrite it wholesale each time you re-export
from SquareLine, so don't keep anything there. Your own application logic lives
in the **sketch** — after calling `ui_init()`, attach event handlers from the
`.ino` to the UI objects SquareLine exposes (e.g. `ui_Button1`). That way
re-exporting the visuals never touches your code.

**Reach for this one** when you want to iterate on layout visually and write
little or no C for the UI itself.

### TouchCalibrate — a one-time setup tool, not a UI starting point

The third bundled example, **`TouchCalibrate`**, isn't a template for your app —
it's a tool you flash once to calibrate the touch panel and tune its noise
filters, saving the result to flash so your real sketch loads it automatically.
See [Touch calibration & filtering](#touch-calibration--filtering) below.

---

## What the examples give you: the `lv_setup` API

Both examples wire the hardware to LVGL through one small header,
**`lv_setup.hpp`** (plus **`lv_conf.h`** for LVGL's build config). That header,
and the global objects it defines, is the entire surface you interact with — and
once `begin()` returns, you are just using plain LVGL.

```cpp
#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"

void setup() {
    Serial.begin(115200);

    lv_setup.begin();           // initialize display + touch + LVGL (default: portrait)

    // ...build your UI on lv_screen_active() here (or call ui_init())...
}

void loop() {
    lv_timer_handler();         // let LVGL run
    delay(5);
}
```

What the header provides:

- **`lv_setup.begin(uint16_t rotation = 0)`** — call once in `setup()`. Brings up
  the ILI9341, the XPT2046 touch, and LVGL; creates the LVGL display and a
  pointer (touch) input device and connects them. After this returns, the active
  LVGL screen exists and you build your UI with ordinary LVGL calls. The
  examples call it as `lv_setup.begin()` and take the default (portrait).
- **`display`** — a global display object. Handy methods:
  `display.width()`, `display.height()` (for the current rotation), and
  `display.setBacklight(0..100)` to dim/brighten the backlight.
- **`lv_setup.touch()`** — the touch driver, for calibration (below). You won't
  normally call it otherwise; `lv_setup` already feeds touch into LVGL.

You don't render or flush anything yourself — LVGL does, through the callbacks
`lv_setup` installed. Your job is to create LVGL widgets and call
`lv_timer_handler()` in `loop()`.

### Portable across chipguy board libraries

Because every hardware detail lives in just two files — **`lv_setup.hpp`** and
**`lv_conf.h`** — your application code (`ui.cpp`, or the SquareLine `src/`
files) is hardware-independent. It only ever talks to LVGL.

So to move an app to a **different display board**, drop in the `lv_setup.hpp`
and `lv_conf.h` from that board's chipguy library, and the rest of your sketch
is unchanged. The same UI runs on different panels with no edits to your widget
code. (Different boards have different resolutions, so alignment-based layouts
travel best; fixed pixel coordinates may need adjusting.)

### Rotation

Pass `0`–`3` (or the equivalent degrees) to `lv_setup.begin()`. Display and
touch rotate together.

| rotation | degrees | orientation | resolution |
|---|---|---|---|
| 0 | 0 | portrait (native) | 240×320 |
| 1 | 90 | landscape | 320×240 |
| 2 | 180 | portrait flipped | 240×320 |
| 3 | 270 | landscape flipped | 320×240 |

So `0`/`2` are portrait and `1`/`3` are landscape, and `90`/`180`/`270` are
accepted as aliases for `1`/`2`/`3` (e.g. `lv_setup.begin(90)` is landscape).
For the SquareLine example, size your SquareLine project to match the
orientation you choose.

### Touch calibration & filtering

You don't calibrate touch in code. The library ships a dedicated tool sketch,
**`TouchCalibrate`** (File ▸ Examples ▸ chipguy_cyd2432S024_display), that you
flash once, run, and then flash your real application over the top. It walks
through three steps on the screen:

1. **Calibration** — tap a sequence of crosshairs with a stylus.
2. **Filter settings** — two sliders set the touch noise filters: a **minimum
   pressure** (ignore feather-light/phantom touches) and a **jump filter** (drop
   glitchy readings that teleport across the screen), with a live readout to help
   you pick values. A **Save** button stores them.
3. **Drawing test** — scribble on the screen to confirm it tracks cleanly.

Everything it produces — the calibration rectangle *and* the filter settings — is
saved to the ESP32's **non-volatile storage preferences partition (NVS)**: the
small flash area the Arduino `Preferences` library reads and writes, which
survives reboots *and* reflashing your sketch.

Your application then picks it up **automatically** — `lv_setup.begin()` calls
`loadCalibration()` and `loadFilters()` at startup — so you never call
`setCalibration()` yourself. Run `TouchCalibrate` once per board.

---

## License

MIT — see [LICENSE](LICENSE).

The ILI9341 initialization/gamma values are standard panel-datasheet register
settings (as also used by Adafruit_ILI9341, BSD-3). `lv_conf.h` is based on
LVGL's template (LVGL is MIT-licensed).
