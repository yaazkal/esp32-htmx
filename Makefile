ENV  ?= esp32doit-devkit-v1
PORT ?= /dev/cu.usbserial-0001

.PHONY: all build flash upload clean monitor

all: build

build:
	pio run -e $(ENV)

flash upload: build
	pio run -e $(ENV) -t upload --upload-port $(PORT)

clean:
	pio run -e $(ENV) -t clean

monitor:
	pio device monitor --port $(PORT) --baud 115200
