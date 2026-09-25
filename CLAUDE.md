# CLAUDE.md — Sound Source Localization with a Microphone Array

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
| Out | One ADC input each: mic 1 → AD4, mics 2–4 → AD1–AD3 (see pin table below) |
| Gain | Unconnected for now = 60 dB. To V+ = 40 dB, to GND = 50 dB |
| AR | Unconnected (attack/release ratio 1:4000, slowest release) |

### ADC pins (from the part header `uc3a3256.h`; AD0–AD3 use GPIO function 0, AD4–AD7 function 2)
| Mic | ADC channel | MCU pin | Xplained header | Verified |
|---|---|---|---|---|
| 1 | AD4 | PA20 (function 2) | J2 (pin not yet recorded) | tap test ✓ |
| 2 | AD1 | PA22 | J2 pin 2 | tap test ✓ |
| 3 | AD2 | PA23 | J2 pin 3 (presumed from pattern) | tap test ✓ |
| 4 | AD3 | PA24 | J2 pin 4 (presumed from pattern) | tap test ✓ |

Wiring is provisional; re-verify this table after the rewire.

**AD0 / PA21 is not used**: it showed a steady ~360-count p2p noise signal whichever mic
was on it (mic swap confirmed it follows the channel), so mic 1 was moved to AD4. Cause not
found yet (wiring vs. something on the board — check the schematic for PA21).
Spare channels: AD5–AD7 on PA19–PA17 (function 2). PA20 doubles as the MXT143E display
backlight pin in ASF, which only matters if `CONF_BOARD_ENABLE_MXT143E_XPLAINED` is set.

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
- **Current clock is 12 MHz**: `conf_clock.h` runs CPU and PBA straight from OSC0
  (12 MHz crystal); PLL0 is configured but unused. Reaching 4 × 96 kHz needs the PLL
  (48–66 MHz) — check flash wait states when going above ~33 MHz.
- **ADC timing** (verify against datasheet ADC chapter): ADC clock = PBA / ((PRESCAL+1)·2),
  max ~5 MHz. ASF's `adc_configure()` sets SHTIM to max (15), so one channel takes about
  (SHTIM+1) + 10 ADC clocks. With today's 3 MHz ADC clock that is ~8.7 µs/channel,
  ~35 µs for 4 channels → ~29 kHz per channel max. Faster needs a higher PBA clock plus
  our own PRESCAL/SHTIM values.
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
- Repo layout: everything lives in `project_9-avr32/`. Our firmware apps are in
  `project_9-avr32/apps/<name>/` (sources + `conf_*.h` + `asf.h`), each with a `gcc/`
  folder holding `Makefile` and `config.mk`. `config.mk` sets
  `PRJ_PATH = ../../../vendor/xdk-asf-3.52.0`, lists our own files as `../main.c`, and
  adds `-I..` to `CPPFLAGS`.
- Build: `make -C apps/<name>/gcc` inside the container (ASF makefile system:
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
**Milestone reached (2026-09-25): hardware bring-up done.** The board boots our own
firmware, all four microphones deliver signal to the ADC, and we talk to the board over
USB. Wiring is provisional and will be redone.

- [x] Hardware assembled on tray, mics on mini breadboards
- [x] Dev container builds, toolchain works (image `project_9-avr32:latest`)
- [x] Build → DFU flash → boot verified end-to-end (trampoline OK)
- [x] USB CDC serial output to the Mac works (`/dev/cu.usbmodem*`)
- [x] All 4 mics read by `apps/mic_test` (software-paced, 8 kHz, mean + p2p per 50 ms):
      idle mean ~385–388 counts ≈ 1.24 V on every channel, matching the MAX9814 bias
      (so ADVREF ≈ 3.3 V is consistent); quiet p2p ≈ 60–110 at 60 dB gain
- [x] Channel→mic mapping verified by tapping each capsule in turn (mic 1 = AD4,
      mic 2 = AD1, mic 3 = AD2, mic 4 = AD3). Claps don't work for this: at 60 dB they
      saturate every mic.

### Findings to carry into the rewire
- **AD0 / PA21 is noisy — avoid it.** It showed a steady p2p ≈ 360 with no sound and a
  mean wandering ±40, whichever mic was connected (a board swap proved it follows the
  channel, not the mic). Likely 50 Hz hum or a ground problem on that path; cause not
  found. Moving mic 1 to AD4 fixed it. If AD0 is ever needed: ground PA21 with nothing
  connected to tell wiring apart from the board, and check the schematic for PA21 (the
  Xplained does have on-board parts on some ADC pins — ASF reads a temperature sensor
  on AD1).
- **Suspected loose shared GND/V+.** Once, all four channels jumped together to a flat
  ~810 counts (≈2.6 V, p2p < 10) for ~150 ms and took ~0.5 s to settle. All mics moving
  at once means a shared supply/ground connection, not sound. Make the star ground and
  V+ rail solid, then do a wiggle test while watching `mic_test`.
- **Clipping and AGC.** Claps and taps hit a ceiling of ~770 counts p2p on every mic.
  After a loud sound the AGC cuts gain, then takes ~1–1.5 s to recover (AR pin
  unconnected = slowest release), so levels depend on recent history. Consider Gain → V+
  (40 dB) when rewiring.
- Record J2 pin numbers for every mic wire, and which tray corner each mic sits in.

## Gotchas learned so far
- `conf_clock.h` sets all `CONFIG_SYSCLK_INIT_*MASK` to 0, so peripheral clocks are off
  after `sysclk_init()`. Enable each one explicitly (e.g.
  `sysclk_enable_pba_module(SYSCLK_ADC)`), or the peripheral silently never runs.
- `dfu-programmer launch` can hang after the chip resets; `flash.sh` kills it after 5 s.
- `mic_test` only prints while a terminal holds DTR high; LED0 toggles as a heartbeat
  regardless.

## Next tasks (in order)
1. Rewire: solid star ground and V+ rail, mics on AD1–AD4 (not AD0), decide on gain.
   Rerun `apps/mic_test`: quiet p2p similar on all four, tap test maps correctly, no
   jumps when wires are wiggled.
2. Measure capsule positions (mm) and record them with the channel map.
3. Put hardware constants in one shared header (see Conventions).
4. Raw-sample capture over USB + host script (Python, in repo `host/`) to plot the
   4 channels — lets every later step be checked by eye.
5. Switch system clock to the PLL (48–66 MHz) and set ADC PRESCAL/SHTIM for the target
   rate.
6. Timer-triggered 4-channel ADC sequence + PDCA into double (ping-pong) buffers.
7. Burst capture on trigger (e.g. clap above threshold) → dump raw samples over USB;
   host saves to `.npy`/CSV.
8. Measure and store ADC inter-channel skew; apply correction. Note the ADC converts
   enabled channels lowest first, so channel order (not mic number) sets the skew.
9. TDoA estimation on the PC first (cross-correlation / GCC-PHAT with sub-sample
   interpolation), validate against oscilloscope measurements, then port to the MCU
   (UC3 DSP library has fixed-point FFT).
10. Direction estimate from pairwise delays + measured mic geometry; simple UI.

## Conventions
- C (gnu99) for firmware, Python 3 for host tools.
- Keep hardware constants (pin map, mic positions in mm, sample rate, channel order)
  in one header, e.g. `project_9-avr32/include/hw_config.h` (not created yet), and mirror mic positions in
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
