# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an Arduino sketch that turns an M5Stack Cardputer (v1.1) into a CardKB keyboard emulator. It exposes the Cardputer's keyboard over I2C as a slave device at address `0x5F`, making it compatible with devices that expect a CardKB.

Tested with Heltec WiFi LoRa 32 V3 running Meshtastic 2.7.15.

## Build Commands

This is an Arduino project. Build and upload using:
- **Arduino IDE**: Open `Cardkb_emulator.ino`, select M5Stack Cardputer board, and upload
- **Arduino CLI**: `arduino-cli compile --fqbn m5stack:esp32:m5stack_cardputer && arduino-cli upload -p /dev/ttyACM0 --fqbn m5stack:esp32:m5stack_cardputer`

## Dependencies

- M5Cardputer library (includes M5Unified)
- Wire library (built-in)

## Architecture

Single-file Arduino sketch with these components:

- **Ring buffer** (`q[]`, `qh`, `qt`): 128-byte circular buffer storing keypresses until the I2C master reads them
- **I2C slave handler** (`onRequest()`): Responds to I2C read requests by popping from the ring buffer
- **Key mapping**: Fn+IJKL or Fn+WAS produce CardKB arrow codes (`0xB4`-`0xB7`), Fn+D toggles display mode
- **User display** (`drawUserDisplay()`): Clean interface with connection status and text preview
- **Debug display** (`drawDebugDisplay()`): Technical view with hex codes, timestamps, queue status
- **Text preview** (`previewBuf[]`): 40-char buffer showing recently typed characters
- **Audio feedback**: Buzzer beep (1800Hz, 10ms) on each keypress

## Key Constants

- `CARDKB_ADDR = 0x5F`: I2C slave address (standard CardKB address)
- Arrow codes: `0xB4` (left), `0xB5` (up), `0xB6` (down), `0xB7` (right)
- Enter: `0x0D`, Backspace: `0x08`
- `BEEP_FREQ = 1800`: Buzzer frequency in Hz
- `BEEP_DURATION = 10`: Buzzer duration in ms
- `CONNECTION_TIMEOUT_MS = 2000`: Time before showing "Waiting..." status
- `PREVIEW_SIZE = 40`: Text preview buffer size
