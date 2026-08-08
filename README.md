# esp32-htmx

Small concept using https://htmx.org and plain CSS for building a UI for ESP32. Has a small API so htmx can query some values

## Running this project

This is a [PlatformIO](https://platformio.org) project using the **ESP-IDF** framework (not Arduino) targeting an `esp32doit-devkit-v1` board. Before building, you'll need PlatformIO installed, and you'll need to make two edits: your WiFi credentials, and your board's serial port.

### 1. Install PlatformIO and the ESP-IDF framework

PlatformIO bundles and manages the ESP-IDF toolchain for you — there's no need to install ESP-IDF separately. Installing PlatformIO Core (CLI) is enough; the ESP-IDF framework itself is pulled in automatically on first build, based on `framework = espidf` in `platformio.ini`.

#### macOS

Easiest via [Homebrew](https://brew.sh):

```
brew install platformio
```

or via [MacPorts](https://www.macports.org):

```
sudo port install platformio
```

Alternatively, install PlatformIO Core directly with its installer script:

```
python3 -c "$(curl -fsSL https://raw.githubusercontent.com/platformio/platformio/master/scripts/get-platformio.py)"
```

If the script was used, this installs PlatformIO Core into `~/.platformio/penv` and adds a `pio` shim. Add it to your
`PATH` if the installer doesn't do so automatically:

```
echo 'export PATH="$HOME/.platformio/penv/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
```

Verify with `pio --version`. The CP2102 USB-UART driver used by this board's dev kit is typically already supported by macOS; if the board doesn't enumerate as a `/dev/cu.usbserial-*` device, install the [Silicon Labs CP210x VCP driver](https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers).

#### FreeBSD

FreeBSD isn't an officially supported PlatformIO host, but the CLI (being pure Python) generally works. Install Python and pip from ports/pkg first:

```
pkg install python3 py312-pip
python3.12 -m pip install --user platformio
```

Add the pip user-install bin directory to your `PATH` (e.g. `~/.local/bin`), then verify with `pio --version`. The board should enumerate through the `cuaU*`/`ttyU*` USB serial devices (via `uftdi`/`umodem`, or `cp210x` on many FreeBSD versions) — check with `usbconfig` or `dmesg` after plugging it in, and adjust the `PORT` value described below accordingly (e.g. `/dev/cuaU0`). You may need to add your user to the `dialer` group to access the serial device without root:

```
pw groupmod dialer -m <your-username>
```

#### Linux

```
python3 -c "$(curl -fsSL https://raw.githubusercontent.com/platformio/platformio/master/scripts/get-platformio.py)"
```

Add `~/.platformio/penv/bin` to your `PATH` as shown in the macOS section above (adjust for your shell/rc file), then verify with `pio --version`. On most distros the CP2102 USB-UART bridge is supported by the in-kernel `cp210x` driver out of the box, so the board should show up as `/dev/ttyUSB0` (or similar). You'll likely need to add your user to the `dialout` group to access the serial port without root:

```
sudo usermod -aG dialout $USER
```

Log out and back in for the group change to take effect. PlatformIO also documents udev rules for USB device permissions — see their [Linux setup guide](https://docs.platformio.org/en/latest/core/installation/udev-rules.html) if you still get permission errors.

### 2. Configure WiFi credentials

Edit the `WIFI_SSID` and `WIFI_PASSWORD` `#define`s near the top of `src/main.c`:

```c
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
```

Fill in your network's SSID and password. It must be a **2.4GHz** network — the ESP32 radio doesn't support 5GHz.

### 3. Configure your device/port in the Makefile

Find your board's serial port (it enumerates as a CP2102 USB-UART bridge):

```
pio device list
```

Then either edit the defaults at the top of `Makefile`:

```make
ENV  ?= esp32doit-devkit-v1
PORT ?= /dev/cu.usbserial-0001
```

or override `PORT` (and `ENV`, if you're targeting a different board) per-invocation, e.g. `make flash PORT=/dev/cu.usbserial-0002`. On Linux this will typically look like `/dev/ttyUSB0`, and on FreeBSD `/dev/cuaU0`.

### 4. Build, flash, and monitor

```
make build     # compile
make flash     # flash to the connected board (add PORT=... to override)
make monitor   # open the serial monitor (needs a real interactive terminal)
make clean     # clean build artifacts
```
