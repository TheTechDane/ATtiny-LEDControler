# Changelog

All notable changes to the ATtiny412 WS2812B Multi-Function LED Controller will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [1.2.0] - 2026-09-06

### 🚀 Added
- **10-Pixel Spaced Chase (Effect 2):** Added a denser dual-chase pattern running two active pixels spaced 10 LEDs apart across the 20-pixel virtual block.
- **Rainbow Spectrum Shift (Effect 6):** Integrated a compact 256-hue spectrum shift algorithm moving smoothly across virtual tiles.
- **Smooth White Sparkle Fade (Effect 7):** Re-engineered the Sparkle effect to smoothly ramp random individual LEDs to pure White at master strip brightness and fade them back down across a 1-second interval.
- **Fast PRNG (`fastRandom`):** Added a lightweight 16-bit Linear Feedback Shift Register pseudo-random number generator to replace bulky standard library `rand()` calls.

### ⚡ Optimizations & Memory Engineering
- **180-LED Bare-Metal Virtual Tiling Streamer (`sendRawBytesTiled`):** Bypassed standard library reset delays with custom PORTA bit-bang streaming. Transmits a 60-byte SRAM buffer (20 virtual pixels) 9 times continuously down the data wire without triggering mid-stream WS2812 reset pulses.
- **Power Rail Safety:** Adjusted default initial startup brightness to **50%** (`brightnessStep = 10`) to prevent current spikes and programmer brownouts during USB/UPDI testing.

### 🛠️ Fixed
- **Button Double-Click State Machine:** Overhauled `handleButton()` state tracking with decoupled click-window logic (`DBL_CLICK_MS = 280ms`), eliminating click drops during heavy animation rendering passes.
- **Strict SRAM Allocation:** Guaranteed total SRAM usage stays under 60 bytes (for LED buffers), leaving plenty of headspace in the ATtiny412's 256-byte SRAM pool.

---

## [1.1.0] - 2026-09-06

### 🚀 Added
- Expanded effect library to 5 modes (Solid, Moving Chase, Breathing Pulse, Alternating Blink, Full Blink).
- Multi-click button interaction framework (Single Click: Color, Double Click: Effect, Long Press: Brightness).

### ⚡ Optimizations
- **Smart EEPROM Save:** Bit-packed color, effect, and brightness settings into a single 8-bit byte with a 10-second deferred write delay to reduce EEPROM wear.

---

## [1.0.0] - 2026-08-20

### 🚀 Initial Release
- Basic WS2812B controller implementation for ATtiny412 microcontroller.
- Single button input with debouncing.
- Optional Serial debugging output at 115200 baud on Pin 2 (`PA6`).