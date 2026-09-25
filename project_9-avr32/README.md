# Project_9 – AVR32 dev environment (macOS + Docker + VS Code)

Compiles inside a Linux container (the AVR32 toolchain has no macOS build),
flashes natively from macOS with `dfu-programmer`.

## One-time setup

1. **Docker Desktop** running. On Apple Silicon, make sure
   *Settings → General → "Use Rosetta for x86_64/amd64 emulation"* is on.
2. **VS Code extension:** "Dev Containers" (ms-vscode-remote.remote-containers).
3. **Flasher on the Mac:** `brew install dfu-programmer`
4. **ASF 3:** download the standalone .zip from
   https://www.microchip.com/en-us/development-tool/asf3 and unzip it into `vendor/`
   (see `vendor/README.md`).
5. Open this folder in VS Code → Command Palette → **"Dev Containers: Reopen in Container"**.
   The first build downloads the toolchain and takes a few minutes.

## Check it works (inside the container)

Run the task **"Check toolchain"** (Terminal → Run Task…). It should print
`avr32-gcc (AVR_32_bit_GNU_Toolchain_3.4.3...) 4.4.7` and find `uc3a3256.h`.

## First build: an ASF example

1. Run task **"List UC3-A3 Xplained examples in ASF"** and pick one
   (a GPIO/LED example is a good first test; later the ADC and PDCA ones).
2. Run the default build task (⇧⌘B) and paste that `gcc` folder path.
3. The output `.hex` appears in that same folder.

### Important: the DFU bootloader offset
The Xplained ships with a USB DFU bootloader in the first 8 KB of flash, so your
program must start at 0x80002000. ASF handles this with a *trampoline*. Open the
example's `gcc/config.mk` and check it contains:

    ASSRCS += avr32/utils/startup/trampoline_uc3.S
    LDFLAGS += -nostartfiles -Wl,-e,_trampoline

If those lines are missing, add them. Without them the program flashes but never runs.

## Flash (in a normal macOS Terminal, not the container)

1. Put the board in DFU mode (hold the bootloader button while plugging in USB).
2. `./scripts/flash.sh vendor/.../gcc/<name>.hex`

USB passthrough into Docker on macOS is unreliable, which is why flashing
happens on the host. Both sides see the same files, so no copying is needed.
