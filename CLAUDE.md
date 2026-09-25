# CLAUDE.md — Voxel: Sound Source Localization with a Microphone Array

## What this project is
University course project (Project 9). We build a system that detects a sound and
estimates the direction it comes from using **Time Difference of Arrival (TDoA)**
between microphones, on an **AVR32 UC3 microcontroller**.

- **Phase 1 (current, 5 hp):** get hardware working, read all 4 mic channels,
  simple bidirectional / 2D localization.
- **Later (15 hp):** 2D localization with UI (grade 4), then 3D with a tetrahedron of
  4 mics = 6 mic pairs (grade 5), with UI.

**Learning is a goal.** The team is using this project to learn embedded and DSP
fundamentals. When you write or change code, briefly explain *why* (peripheral
behaviour, timing, memory), not just *what*. Prefer clear, commented code over clever code.

## Hardware (what we actually have)

| Item | Details |
|---|---|
| MCU board | **UC3-A3 Xplained** (AT32UC3A3256, 32-bit AVR, up to 66 MHz, 64 KB CPU SRAM + 2×32 KB bus SRAM, Hi-Speed USB) |
| Programming | USB **DFU bootloader** (no on-board debugger). No JTAG debugger yet. |
| Microphones | 4 × **Adafruit MAX9814** electret mic amp with AGC (pins: GND, V+, Gain, Out, AR) |
| Layout | 4 mics at the corners of a rectangle on a tray, roughly 35 × 45 cm (**exact capsule positions not yet measured**) |

### Wiring (per microphone)
| MAX9814 pin | Connect to |
|---|---|
| V+ | 3.3 V on the Xplained (NOT 5 V — keeps output within ADC range) |
| GND | GND (star ground from central breadboard) |
| Out | One ADC input each: mics 1–4 → ADC channels 0–3 (**actual header pins TBD — check UC3-A3 Xplained schematic**) |
| Gain | Unconnected for now = 60 dB. To V+ = 40 dB, to GND = 50 dB |
| AR | Unconnected (attack/release ratio 1:4000, slowest release) |

- MAX9814 output: ~1.25 V DC bias, up to ~2 Vpp → about 0.25–2.25 V. Connected directly
  to ADC (no coupling cap); optional 1 kΩ series resistor.
- ADC reference (ADVREF) is presumably 3.3 V — **verify on schematic**.
- AGC cannot be disabled. It changes gain independently per channel and distorts
  transient onsets. Prefer amplitude-independent methods (e.g. GCC-PHAT) over
  threshold-crossing onset detection.

## Key numbers and constraints
- Speed of sound ≈ 343 m/s. Max mic spacing ≈ 0.55 m diagonal → max delay ≈ 1.6 ms.
- Target sample rate ≈ 96 kHz per channel (4 channels ≈ 384 kSPS total). Check this
  against the UC3A3 ADC datasheet limits (10-bit, ADC clock max ~5 MHz, ~10 clocks +
  S/H per conversion). Lower (e.g. 48 kHz) is acceptable if needed.
- One sample at 96 kHz = 10.4 µs ≈ 3.6 mm of sound travel.
- **The ADC is a single multiplexed converter** — channels are sampled sequentially,
  a few µs apart, NOT simultaneously. This inter-channel skew (~2–3 µs per channel step)
  must be measured (same signal from a function generator into all inputs) and
  corrected in software.
- **Sampling must be hardware-timed**: Timer/Counter triggers the ADC, Peripheral DMA
  (PDCA) moves results to RAM. Never sample from a software loop (jitter).
- Data rate 4 ch × 96 kHz × 2 B ≈ 768 kB/s → UART is far too slow. Use either
  burst capture into SRAM (~160 ms fits) then dump, or USB (CDC / vendor class).

## Toolchain and workflow (macOS host)
The AVR32 toolchain has no macOS build, so we compile in a Docker dev container and
flash from macOS.

- `.devcontainer/` — Ubuntu 22.04 **linux/amd64** image (Rosetta on Apple Silicon) with
  Atmel AVR32 GNU Toolchain 3.4.3 (`avr32-gcc`, GCC 4.4.7, gnu99) + headers 6.2.0.742
  in `/opt/avr32`.
- `vendor/xdk-asf-3.52.0/` — **ASF 3** standalone (drivers: adc, pdca, tc, gpio, intc,
  pm, usbb/udc; board support for UC3-A3 Xplained). Git-ignored.
- Build: `make -C <project>/gcc` inside the container (ASF makefile system:
  `config.mk` + `make/Makefile.avr32.in`). Compiler flag `-mpart=uc3a3256`,
  `BOARD=UC3_A3_XPLAINED`.
- If only `.elf` is produced: `avr32-objcopy -O ihex x.elf x.hex`.
- Flash (macOS Terminal, NOT container): board into DFU mode, then
  `./scripts/flash.sh path/to/file.hex` (uses `dfu-programmer at32uc3a3256`,
  `--suppress-bootloader-mem`).
- **Bootloader offset:** DFU bootloader occupies first 8 KB; application must start at
  0x80002000. Every project's `config.mk` must include:
  ```
  ASSRCS  += avr32/utils/startup/trampoline_uc3.S
  LDFLAGS += -nostartfiles -Wl,-e,_trampoline
  ```
  Otherwise it flashes but never runs.
- Debug output: USB CDC → appears on Mac as `/dev/cu.usbmodem*`.

## Current status
- [x] Hardware assembled on tray, mics on mini breadboards
- [x] Dev container + flash script written — **not yet verified end-to-end**
- [ ] Container builds and `avr32-gcc --version` works
- [ ] ASF example (LED/GPIO) built, flashed, runs
- [ ] Single mic read via ADC and printed over USB
- [ ] All 4 channels read

## Next tasks (in order)
1. Get the toolchain verified: build and flash an ASF UC3-A3 Xplained LED example.
2. Create our own project (copy an ASF example's structure into `firmware/`, not inside
   `vendor/`). Keep ASF paths relative via `PRJ_PATH` in `config.mk`.
3. Read one ADC channel, print over USB CDC. Confirm ~1.25 V bias ≈ code ~388 at
   3.3 V ref, 10-bit.
4. Timer-triggered 4-channel ADC sequence + PDCA into double (ping-pong) buffers.
5. Burst capture on trigger (e.g. clap above threshold) → dump raw samples over USB.
6. Host side (Python, in repo `host/`): read serial, save to `.npy`/CSV, plot 4 channels.
7. Measure and store ADC inter-channel skew; apply correction.
8. TDoA estimation on the PC first (cross-correlation / GCC-PHAT with sub-sample
   interpolation), validate against oscilloscope measurements, then port to the MCU
   (UC3 DSP library has fixed-point FFT).
9. Direction estimate from pairwise delays + measured mic geometry; simple UI.

## Conventions
- C (gnu99) for firmware, Python 3 for host tools.
- Keep hardware constants (pin map, mic positions in mm, sample rate, channel order)
  in one header, e.g. `firmware/src/config_voxel.h`, and mirror mic positions in
  `host/geometry.py`.
- Document channel→mic mapping explicitly; swapped channels are a classic failure mode.
- Don't modify files under `vendor/`; copy what you need.
- When unsure about a register, clock or pin detail, say so and point to the relevant
  datasheet section rather than guessing.

## Useful references
- AT32UC3A3 datasheet (ADC, PDCA, TC, PM chapters)
- UC3-A3 Xplained user guide + schematic (header pinout, ADVREF, DFU button)
- ASF 3 docs: https://asf.microchip.com/docs/latest/
- Adafruit MAX9814 guide (Gain/AR pin behaviour)
