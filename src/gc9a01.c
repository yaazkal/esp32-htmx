#include <ctype.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"

#include "gc9a01.h"

#define PIN_SCLK GPIO_NUM_18
#define PIN_MOSI GPIO_NUM_23
#define PIN_CS GPIO_NUM_21
#define PIN_DC GPIO_NUM_22
#define PIN_RST GPIO_NUM_17

// SCLK=18/MOSI=23 are the ESP32's native VSPI (SPI3_HOST) pins -- IOMUX-direct
// routing instead of going through the GPIO matrix.
#define SPI_HOST_USED SPI3_HOST
#define SPI_CLOCK_HZ (20 * 1000 * 1000) // conservative given breadboard jumper wiring

// Marquee text is drawn into a single reusable off-screen band across the
// circle's widest point, then blitted in one SPI transfer per frame, rather
// than redrawing (or even touching) the full 240x240 screen every frame.
#define BAND_HEIGHT 40
#define BAND_Y ((GC9A01_WIDTH - BAND_HEIGHT) / 2)

// Font: 5x7, drawn at this many device pixels per font pixel. One column of
// spacing (also scaled) separates characters.
#define FONT_SCALE 4
#define GLYPH_W 5
#define GLYPH_H 7
#define CHAR_ADVANCE_PX ((GLYPH_W + 1) * FONT_SCALE)
#define GLYPH_Y_OFFSET ((BAND_HEIGHT - GLYPH_H * FONT_SCALE) / 2)

// Panel-batch-dependent; flip if colors/orientation look wrong on your unit.
// MADCTL bits: MY=0x80, MX=0x40, MV=0x20 (row/col exchange), BGR=0x08.
// MX was set (0x48) which mirrors the X axis -- cleared here to un-mirror.
#define MADCTL_VALUE 0x08

static spi_device_handle_t s_spi;
static uint8_t *s_band_buf; // BAND_HEIGHT rows x GC9A01_WIDTH cols x 2 bytes/px, DMA-capable

// --- 5x7 font -------------------------------------------------------------
// Covers ASCII 0x20 (' ') through 0x5F ('_') -- i.e. space, digits, uppercase
// letters and common punctuation. Anything outside that range, and any
// lowercase letter (uppercased before lookup), maps into this table;
// unsupported glyphs are left blank. Each row is 5 bits, MSB = leftmost
// pixel. Hand-authored block lettering -- deliberately uppercase-only/small
// glyph set since there's no way to visually proof a bitmap font before
// flashing to real hardware; if a specific glyph looks wrong on the panel,
// fix that one row here.
#define FONT_FIRST_CHAR 0x20
#define FONT_LAST_CHAR 0x5F
#define FONT_GLYPH_COUNT (FONT_LAST_CHAR - FONT_FIRST_CHAR + 1)

static const uint8_t FONT5X7[FONT_GLYPH_COUNT][GLYPH_H] = {
    /* 0x20 ' ' */ {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000},
    /* 0x21 '!' */ {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100},
    /* 0x22 '"' */ {0},
    /* 0x23 '#' */ {0},
    /* 0x24 '$' */ {0},
    /* 0x25 '%' */ {0},
    /* 0x26 '&' */ {0},
    /* 0x27 '\''*/ {0b00100, 0b00100, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000},
    /* 0x28 '(' */ {0},
    /* 0x29 ')' */ {0},
    /* 0x2A '*' */ {0},
    /* 0x2B '+' */ {0},
    /* 0x2C ',' */ {0b00000, 0b00000, 0b00000, 0b00000, 0b00110, 0b00100, 0b01000},
    /* 0x2D '-' */ {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000},
    /* 0x2E '.' */ {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100},
    /* 0x2F '/' */ {0},
    /* 0x30 '0' */ {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110},
    /* 0x31 '1' */ {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110},
    /* 0x32 '2' */ {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111},
    /* 0x33 '3' */ {0b01110, 0b10001, 0b00001, 0b00110, 0b00001, 0b10001, 0b01110},
    /* 0x34 '4' */ {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010},
    /* 0x35 '5' */ {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110},
    /* 0x36 '6' */ {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110},
    /* 0x37 '7' */ {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000},
    /* 0x38 '8' */ {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110},
    /* 0x39 '9' */ {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100},
    /* 0x3A ':' */ {0},
    /* 0x3B ';' */ {0},
    /* 0x3C '<' */ {0},
    /* 0x3D '=' */ {0},
    /* 0x3E '>' */ {0},
    /* 0x3F '?' */ {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b00000, 0b00100},
    /* 0x40 '@' */ {0},
    /* 0x41 'A' */ {0b00100, 0b01010, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001},
    /* 0x42 'B' */ {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110},
    /* 0x43 'C' */ {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110},
    /* 0x44 'D' */ {0b11100, 0b10010, 0b10001, 0b10001, 0b10001, 0b10010, 0b11100},
    /* 0x45 'E' */ {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111},
    /* 0x46 'F' */ {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000},
    /* 0x47 'G' */ {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111},
    /* 0x48 'H' */ {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001},
    /* 0x49 'I' */ {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110},
    /* 0x4A 'J' */ {0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100},
    /* 0x4B 'K' */ {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001},
    /* 0x4C 'L' */ {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111},
    /* 0x4D 'M' */ {0b10001, 0b11011, 0b10101, 0b10001, 0b10001, 0b10001, 0b10001},
    /* 0x4E 'N' */ {0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001},
    /* 0x4F 'O' */ {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110},
    /* 0x50 'P' */ {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000},
    /* 0x51 'Q' */ {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101},
    /* 0x52 'R' */ {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001},
    /* 0x53 'S' */ {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110},
    /* 0x54 'T' */ {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100},
    /* 0x55 'U' */ {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110},
    /* 0x56 'V' */ {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100},
    /* 0x57 'W' */ {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010},
    /* 0x58 'X' */ {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001},
    /* 0x59 'Y' */ {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100},
    /* 0x5A 'Z' */ {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111},
    /* 0x5B '[' */ {0},
    /* 0x5C '\\'*/ {0},
    /* 0x5D ']' */ {0},
    /* 0x5E '^' */ {0},
    /* 0x5F '_' */ {0},
};

// --- low-level command/data helpers ---------------------------------------

static void send_cmd(uint8_t cmd) {
  gpio_set_level(PIN_DC, 0);
  spi_transaction_t t = {.length = 8, .tx_buffer = &cmd};
  spi_device_polling_transmit(s_spi, &t);
}

static void send_data(const uint8_t *data, size_t len) {
  if (len == 0) {
    return;
  }
  gpio_set_level(PIN_DC, 1);
  spi_transaction_t t = {.length = len * 8, .tx_buffer = data};
  spi_device_polling_transmit(s_spi, &t);
}

// --- bring-up sequence ------------------------------------------------------
// Standard GC9A01 register init table, as reproduced (with the same values)
// across most open GC9A01 drivers -- it's chip-specific bring-up boilerplate,
// not really product code. Not independently verified against real hardware
// by whoever wrote this: if the panel stays blank after flashing, this table
// (particularly MADCTL_VALUE's orientation/BGR bits, or a missing delay) is
// the prime suspect.
typedef struct {
  uint8_t cmd;
  uint8_t data[12];
  uint8_t len;
  uint16_t delay_ms;
} init_cmd_t;

static const init_cmd_t INIT_SEQUENCE[] = {
    {0xEF, {0}, 0, 0},
    {0xEB, {0x14}, 1, 0},
    {0xFE, {0}, 0, 0},
    {0xEF, {0}, 0, 0},
    {0xEB, {0x14}, 1, 0},
    {0x84, {0x40}, 1, 0},
    {0x85, {0xFF}, 1, 0},
    {0x86, {0xFF}, 1, 0},
    {0x87, {0xFF}, 1, 0},
    {0x88, {0x0A}, 1, 0},
    {0x89, {0x21}, 1, 0},
    {0x8A, {0x00}, 1, 0},
    {0x8B, {0x80}, 1, 0},
    {0x8C, {0x01}, 1, 0},
    {0x8D, {0x01}, 1, 0},
    {0x8E, {0xFF}, 1, 0},
    {0x8F, {0xFF}, 1, 0},
    {0xB6, {0x00, 0x20}, 2, 0},
    {0x36, {MADCTL_VALUE}, 1, 0},
    {0x3A, {0x05}, 1, 0}, // COLMOD: 16-bit RGB565
    {0x90, {0x08, 0x08, 0x08, 0x08}, 4, 0},
    {0xBD, {0x06}, 1, 0},
    {0xBC, {0x00}, 1, 0},
    {0xFF, {0x60, 0x01, 0x04}, 3, 0},
    {0xC3, {0x13}, 1, 0},
    {0xC4, {0x13}, 1, 0},
    {0xC9, {0x22}, 1, 0},
    {0xBE, {0x11}, 1, 0},
    {0xE1, {0x10, 0x0E}, 2, 0},
    {0xDF, {0x21, 0x0C, 0x02}, 3, 0},
    {0xF0, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6, 0},
    {0xF1, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6, 0},
    {0xF2, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6, 0},
    {0xF3, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6, 0},
    {0xED, {0x1B, 0x0B}, 2, 0},
    {0xAE, {0x77}, 1, 0},
    {0xCD, {0x63}, 1, 0},
    {0x70, {0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03}, 9, 0},
    {0xE8, {0x34}, 1, 0},
    {0x62, {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70}, 12, 0},
    {0x63, {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70}, 12, 0},
    {0x64, {0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07}, 7, 0},
    {0x66, {0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00}, 10, 0},
    {0x67, {0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98}, 10, 0},
    {0x74, {0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00}, 7, 0},
    {0x98, {0x3E}, 1, 0},
    {0x99, {0x3E}, 1, 0},
    {0x21, {0}, 0, 0}, // display inversion on -- most GC9A01 panels need this for correct colors
    {0x11, {0}, 0, 120}, // sleep out
    {0x29, {0}, 0, 20},  // display on
};

static esp_err_t spi_setup(void) {
  spi_bus_config_t buscfg = {
      .mosi_io_num = PIN_MOSI,
      .miso_io_num = -1,
      .sclk_io_num = PIN_SCLK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = GC9A01_WIDTH * BAND_HEIGHT * 2,
  };
  esp_err_t err = spi_bus_initialize(SPI_HOST_USED, &buscfg, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    return err;
  }

  spi_device_interface_config_t devcfg = {
      .clock_speed_hz = SPI_CLOCK_HZ,
      .mode = 0,
      .spics_io_num = PIN_CS,
      .queue_size = 1,
  };
  return spi_bus_add_device(SPI_HOST_USED, &devcfg, &s_spi);
}

static void set_window(int x0, int y0, int x1, int y1) {
  uint8_t caset[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
  uint8_t raset[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
  send_cmd(0x2A);
  send_data(caset, sizeof(caset));
  send_cmd(0x2B);
  send_data(raset, sizeof(raset));
  send_cmd(0x2C); // RAMWR -- pixel data follows via send_data
}

static inline void band_set_pixel(int x, int y, uint16_t color565) {
  if (x < 0 || x >= GC9A01_WIDTH || y < 0 || y >= BAND_HEIGHT) {
    return;
  }
  size_t idx = ((size_t)y * GC9A01_WIDTH + x) * 2;
  s_band_buf[idx] = (uint8_t)(color565 >> 8); // panel expects big-endian pixel words
  s_band_buf[idx + 1] = (uint8_t)color565;
}

static void band_fill(uint16_t color565) {
  for (int y = 0; y < BAND_HEIGHT; y++) {
    for (int x = 0; x < GC9A01_WIDTH; x++) {
      band_set_pixel(x, y, color565);
    }
  }
}

static void band_draw_char(int pen_x, char c, uint16_t fg565) {
  if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR) {
    return; // unsupported glyph -- leave blank, same as a space
  }
  const uint8_t *glyph = FONT5X7[c - FONT_FIRST_CHAR];
  for (int row = 0; row < GLYPH_H; row++) {
    for (int col = 0; col < GLYPH_W; col++) {
      if (!((glyph[row] >> (GLYPH_W - 1 - col)) & 1)) {
        continue;
      }
      for (int sy = 0; sy < FONT_SCALE; sy++) {
        for (int sx = 0; sx < FONT_SCALE; sx++) {
          band_set_pixel(pen_x + col * FONT_SCALE + sx, GLYPH_Y_OFFSET + row * FONT_SCALE + sy, fg565);
        }
      }
    }
  }
}

esp_err_t gc9a01_init(void) {
  gpio_reset_pin(PIN_DC);
  gpio_set_direction(PIN_DC, GPIO_MODE_OUTPUT);
  gpio_reset_pin(PIN_RST);
  gpio_set_direction(PIN_RST, GPIO_MODE_OUTPUT);

  esp_err_t err = spi_setup();
  if (err != ESP_OK) {
    return err;
  }

  gpio_set_level(PIN_RST, 0);
  esp_rom_delay_us(20);
  gpio_set_level(PIN_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(120));

  for (size_t i = 0; i < sizeof(INIT_SEQUENCE) / sizeof(INIT_SEQUENCE[0]); i++) {
    const init_cmd_t *ic = &INIT_SEQUENCE[i];
    send_cmd(ic->cmd);
    send_data(ic->data, ic->len);
    if (ic->delay_ms > 0) {
      vTaskDelay(pdMS_TO_TICKS(ic->delay_ms));
    }
  }

  s_band_buf = heap_caps_malloc(GC9A01_WIDTH * BAND_HEIGHT * 2, MALLOC_CAP_DMA);
  if (s_band_buf == NULL) {
    return ESP_ERR_NO_MEM;
  }

  // Fill the whole screen with the background color, one BAND_HEIGHT-tall
  // strip at a time, reusing the same buffer the marquee will draw into.
  band_fill(GC9A01_BG_COLOR);
  for (int y = 0; y < GC9A01_WIDTH; y += BAND_HEIGHT) {
    set_window(0, y, GC9A01_WIDTH - 1, y + BAND_HEIGHT - 1);
    send_data(s_band_buf, GC9A01_WIDTH * BAND_HEIGHT * 2);
  }

  return ESP_OK;
}

void gc9a01_draw_marquee_frame(const char *text, int x_offset, uint16_t fg565, uint16_t bg565) {
  band_fill(bg565);

  int pen_x = x_offset;
  for (const char *p = text; *p != '\0'; p++) {
    band_draw_char(pen_x, (char)toupper((unsigned char)*p), fg565);
    pen_x += CHAR_ADVANCE_PX;
  }

  set_window(0, BAND_Y, GC9A01_WIDTH - 1, BAND_Y + BAND_HEIGHT - 1);
  send_data(s_band_buf, GC9A01_WIDTH * BAND_HEIGHT * 2);
}

int gc9a01_marquee_text_width_px(const char *text) {
  return (int)strlen(text) * CHAR_ADVANCE_PX;
}
