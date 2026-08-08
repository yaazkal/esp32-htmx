# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

A PlatformIO project targeting the ESP32 (`esp32doit-devkit-v1` board, 2MB flash) using the native **ESP-IDF** framework (C, not Arduino). `src/main.c` connects to WiFi in station mode and runs an `esp_http_server` serving a small styled web page (HTML/CSS + [htmx](https://htmx.org) for AJAX-style interactivity without a JS build step) with a grid of live device-status cards, using the onboard LED (GPIO2) as a connect-status indicator: blinking while retrying, solid on if the connection fails, off once connected, and a brief flash on each user-triggered `/api/*` request (not the auto-polling WiFi one — see below).

WiFi credentials are hardcoded as `WIFI_SSID`/`WIFI_PASSWORD` `#define`s at the top of `main.c` — edit them before flashing. Must be a 2.4GHz network (the ESP32 radio doesn't support 5GHz). Note: the original ESP32 die (unlike the S2/S3/C-series) has **no on-die temperature sensor** — don't add one without external hardware.

### Routes
- `GET /` — the HTML page shell
- `GET /style.css`, `GET /htmx.min.js` — static assets (htmx is served gzip-compressed)
- `GET /api/uptime`, `GET /api/heap`, `GET /api/tasks` — each backs one status card's "Refresh" button (`hx-get` + `hx-trigger="load"` so they also populate on page load); returns just the value as a plain HTML fragment swapped into the card.
- `GET /api/wifi` — same pattern, but `hx-trigger="load, every 2s"` with no button, so it polls continuously. Deliberately skips the LED flash the others do (flashing every 2s forever would be noise, not signal).

Copy this per-card, per-endpoint pattern for future interactive features instead of building out a JS frontend or one do-everything endpoint.

## Build system

This is a PlatformIO project (not CMake/Make despite the CLion project files present, and despite ESP-IDF normally being a CMake project on its own — PlatformIO wraps it). All builds, uploads, and dependency management go through the `pio` CLI, or the `Makefile` wrapper in this repo.

Common commands (run from the repo root, where `platformio.ini` lives):

- Build: `make build` / `pio run`
- Flash to a connected board: `make flash` / `pio run -t upload --upload-port <port>`
- Open serial monitor: `make monitor` / `pio device monitor --port <port> --baud 115200` (needs a real interactive terminal — doesn't work through a non-tty/scripted shell)
- Clean build artifacts: `make clean` / `pio run -t clean`
- List connected serial devices: `pio device list`

The board enumerates as a CP2102 USB-UART bridge. `Makefile` defaults `PORT` to `/dev/cu.usbserial-0001` and `ENV` to `esp32doit-devkit-v1`; override per-invocation, e.g. `make flash PORT=/dev/cu.usbserial-0002`.

No unit tests exist yet (`test/` is an empty PlatformIO scaffold).

## Project structure

- `platformio.ini` — single environment `esp32doit-devkit-v1`, platform `espressif32`, framework `espidf`.
- `src/main.c` — entry point (`app_main()`, not `setup()`/`loop()`). No manual component `REQUIRES`/`PRIV_REQUIRES` list is needed in `src/CMakeLists.txt`: ESP-IDF's own build system auto-resolves which components (`esp_wifi`, `esp_http_server`, `nvs_flash`, `driver`, etc.) to link based on the headers `main.c` includes.
- `src/CMakeLists.txt` — hand-written (not the PlatformIO-auto-generated stub, which globs `src/*.*` as sources — that would try to compile the HTML/CSS/gz assets below as C). Keep it minimal: just `SRCS "main.c"`.
- `src/index.html`, `src/style.css`, `src/htmx.min.js.gz` — static web assets, vendored/authored directly, **not** referenced at build time. They're compiled into `src/web_assets.h` (see below) and only kept around as the source of truth for regenerating it.
- `src/web_assets.h` — generated, checked-in header with the three assets above as `const unsigned char foo[]` byte arrays (plus `foo_len`), produced via `xxd -i`. **Regenerated automatically** by `scripts/gen-web-assets.sh`, which `make build`/`make flash` run as a prerequisite — so editing any of the three source assets and rebuilding is enough, no manual step needed. The script is `/bin/sh` (POSIX), portable across macOS/FreeBSD/Linux; it writes to a temp file and renames it into place rather than using `sed -i`, since GNU sed and BSD sed take incompatible flags for in-place editing. It also adds `const` to `xxd -i`'s output (which doesn't emit it by default) so the arrays land in flash (`.rodata`) instead of being copied into RAM at boot. To regenerate without a full build: `scripts/gen-web-assets.sh`.

  **Why not ESP-IDF's native `EMBED_FILES`/`EMBED_TXTFILES`?** Tried first, hit two real bugs in this PlatformIO release's (`espressif32` platform, `framework-espidf` 6.0.1) CMake↔SCons integration: (1) `idf_component_register(... EMBED_FILES ...)` in `src/CMakeLists.txt` makes PlatformIO's build script double-prefix the build dir for CMake-generated sources, producing an unfindable path like `.pio/build/<env>/.pio/build/<env>/foo.S`; (2) the PlatformIO-native workaround, `board_build.embed_files`/`embed_txtfiles` in `platformio.ini`, generates the `.S`/`.o` but never actually adds it to the link step for the `espidf` framework (only for `arduino`), producing `undefined reference to _binary_..._start` at link time. The `xxd -i` byte-array approach sidesteps both — it's just plain C, no linker-symbol/build-graph magic — at the cost of manual regeneration when assets change.
- `include/` — project header files.
- `lib/` — private, project-specific libraries; each gets its own subdirectory under `lib/` and is auto-discovered by PlatformIO's Library Dependency Finder.
- `test/` — PlatformIO unit tests (empty scaffold currently).
- `.pio/` — PlatformIO build output/cache, including the generated `sdkconfig`; gitignored, do not edit or commit.
