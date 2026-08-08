#pragma once

#include <stdint.h>

#include "esp_err.h"

// Minimal hand-rolled driver for a GC9A01 240x240 round SPI TFT, purpose-built
// for the web UI's scrolling marquee card — not a general graphics library.
// Wiring (fixed, not configurable): SCLK=GPIO18, MOSI=GPIO23, CS=GPIO21,
// DC=GPIO22, RST=GPIO17 ("TX2"), backlight tied directly to 3.3V.

#define GC9A01_WIDTH 240

// RGB888 -> RGB565, matching the web UI's own color palette.
#define GC9A01_COLOR565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define GC9A01_BG_COLOR GC9A01_COLOR565(0x0f, 0x17, 0x2a) // page background navy
#define GC9A01_FG_COLOR GC9A01_COLOR565(0x38, 0xbd, 0xf8) // button accent blue

// Resets and brings up the panel, and fills the screen with GC9A01_BG_COLOR.
// Must be called once before gc9a01_draw_marquee_frame().
esp_err_t gc9a01_init(void);

// Renders one marquee frame: `text` (only space/digits/uppercase A-Z/basic
// punctuation are drawn — lowercase is uppercased, anything else renders as a
// blank glyph) starting at horizontal pixel `x_offset`, and pushes it to the
// panel. Draws into a single reusable off-screen band, not the full screen.
void gc9a01_draw_marquee_frame(const char *text, int x_offset, uint16_t fg565, uint16_t bg565);

// Total on-screen pixel width `text` would occupy at the marquee's fixed font
// scale — used by the caller to know when the text has fully scrolled past
// and the scroll offset should wrap back around.
int gc9a01_marquee_text_width_px(const char *text);
