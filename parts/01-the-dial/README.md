[All parts](../../README.md#the-parts) · [Part 2 →](../02-wall-mount/README.md)

# Part 1 · The dial

[![The Waveshare ESP32-S3 knob with its round touchscreen](../../images/13-waveshare-knob-small.jpg)](../../images/13-waveshare-knob.jpg)

*The Waveshare ESP32-S3 knob (product photo, shown for reference).*

The hardware is stock: a round touchscreen inside a ring that turns, with an ESP32-S3 behind it. Nothing is soldered or opened. Everything on the screen is the firmware in [part 4](../04-firmware/README.md).

## At a glance

- **Board:** [Waveshare ESP32-S3-Knob-Touch-LCD-1.8](https://www.waveshare.com/esp32-s3-knob-touch-lcd-1.8.htm)
- **Chip:** ESP32-S3 at 240 MHz, 16 MB flash, 8 MB PSRAM
- **Screen:** 360 × 360 round panel, ST77916 driver over QSPI
- **Touch:** CST816 over I²C
- **The ring:** a rotary encoder that pulses one line per direction
- **Haptics:** a DRV2605 driving a small vibration motor, for a tick on every step
- **Microphone:** PDM, recorded at 16 kHz for voice search
- **Power:** USB-C, which is also how it's flashed the first time
- **Files for this part:** `firmware/components/board/` (the drivers), `firmware/sdkconfig.defaults` (the chip settings)

## The pins

All of these are fixed by the board. They're listed so you can check them against your own unit, and they live in `firmware/components/board/`.

- **Screen (QSPI):** CS 14, clock 13, data 15 to 18, reset 21, backlight 47
- **Touch and haptics (I²C):** SDA 11, SCL 12, touch reset 10. Touch at address 0x15, haptics at 0x5A
- **The ring:** GPIO 8 pulses for clockwise, GPIO 7 for anticlockwise
- **Microphone:** clock 45, data 46
- **Battery sense:** GPIO 1, through a 10 k / 10 k divider

## Settings I chose

- **Two 6 MB app slots** (`firmware/partitions.csv`), so an update over Wi-Fi can be written beside the running firmware. Rollback is on: if a new build can't get back online, the bootloader puts the old one back.
- **Octal PSRAM at 80 MHz.** Album covers and lists live there. The internal RAM is kept for Wi-Fi and anything the flash can't share.
- **The ring is read as two pulse lines, every 3 ms.** Each direction has its own line that goes low for one detent, so it isn't a quadrature encoder. A pulse counts once it has been low for two samples.

## Add just this part

1. Buy the board. It comes ready to use, with a USB-C socket on the back.
2. Go on to [part 4](../04-firmware/README.md) to put the firmware on it.

---

[All parts](../../README.md#the-parts) · [Part 2 →](../02-wall-mount/README.md)
