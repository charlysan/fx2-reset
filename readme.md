# FX2 logic analyzer reset tool

A tool to reset cheap 8-channel logic analyzer clones (Cypress FX2, CY7C68013A) after another tool (like sigrok/PulseView) loads its own firmware and the vendor app can't connect anymore. No unplug needed.


## Problem: vendor app won't connect after using sigrok

After running sigrok-cli, the vendor app shows:

```
Device connection failure
An unknown error occurred during 
device connection. Please disconnect 
and reconnect your device...
```

Only unplug/replug fixed it.

### Why it happens

- The FX2 has no flash. At power-up it reads only VID/PID from the EEPROM
  ("C0" boot) and sits there with no firmware.
- The vendor app sees 0925:3881, uploads its own firmware to RAM, and runs it.
- sigrok does the same with fx2lafw. The build it picks for this device
  **keeps the same VID/PID 0925:3881**.
- fx2lafw stays in RAM until power is lost.
- So the vendor app still finds "its" device, tries to connect, and fails.

fx2lafw doesn't show up with its own ID (1d50:608c). Same VID/PID, only the
strings change:

|                       | fresh plug (EEPROM boot) | after sigrok (fx2lafw in RAM) |
|-----------------------|--------------------------|-------------------------------|
| VID:PID               | 0925:3881                | 0925:3881                     |
| Manufacturer / Product| none                     | `sigrok` / `fx2lafw`          |
| Serial                | none                     | original device name          |
| bcdDevice             | 0x0001                   | 0x0001                        |

So the state has to be detected from the strings, not the VID/PID.

A plain USB reset doesn't help: macOS re-enumerates and fx2lafw is still
running.

### Second part: leftover registers

First try only handed enumeration back to the FX2 core (clear `USBCS.RENUM`
+ disconnect/reconnect). The device came back as bare 0925:3881, no
strings, but the vendor app **still failed**.

Reason: an 8051 reset and a re-enumeration don't touch the FX2 config
registers (0xE600...). fx2lafw left them configured. The vendor firmware
expects power-on defaults.

Register dumps (0xE600-0xE61F):

```
fresh plug     12 80 00 00 00 00 00 00 00 00 01 00 00 00 00 00 a0 a0 a2 a0 e2 e0 00 00 05 05 05 05 00 00 00 00
first reset    12 ce 00 00 00 00 00 00 00 00 01 03 00 00 00 00 a0 20 e0 20 a0 60 00 00 08 05 10 05 00 00 00 00
final reset    12 80 00 00 00 00 00 00 00 00 01 00 00 00 00 00 a0 a0 a2 a0 e2 e0 00 00 05 05 05 05 00 00 00 00
```

Main differences after the first reset:

| reg        | addr   | fresh | first reset |
|------------|--------|-------|-------------|
| IFCONFIG   | E601   | 80    | ce          |
| REVCTL     | E60B   | 00    | 03          |
| EP1INCFG   | E611   | a0    | 20          |
| EP2CFG     | E612   | a2    | e0          |
| EP4CFG     | E613   | a0    | 20          |
| EP6CFG     | E614   | e2    | a0          |
| EP8CFG     | E615   | e0    | 60          |
| EP2FIFOCFG | E618   | 05    | 08          |
| EP6FIFOCFG | E61A   | 05    | 10          |

`REVCTL = 03` (NOAUTOARM + SKIPCOMMIT) is the most likely culprit: it changes
how endpoints get armed. The vendor app had also touched the device before
the "first reset" dump, so not every value there is from fx2lafw.

After restoring defaults, the only differences left vs a fresh plug:

- GPIF waveform RAM (E400-E47F): random at power-up, zeros after. Can't matter.
- Counters/status: USBFRAME, ERRCNTLIM, I2CS/I2DAT, GPIFIRQ.
- Small GPIF/flow leftovers (GPIFIDLECTL, FLOWSTB*, EP2GPIFFLGSEL, IBNIE,
  SUDPTR). Now also restored.

`CPUCS = 12` in all dumps comes from the dump tool's own bootloader, not
the device state.

### Fix

Load a tiny firmware (`reenum.c`) on top of fx2lafw. The FX2 core always
accepts RAM uploads (0xA0 request), even with fx2lafw running.

It does:

1. CPU clock to 12 MHz (fx2lafw leaves 48 MHz), so the timer delay is exact.
2. `USBCS`: set DISCON, clear RENUM (FX2 core does enumeration again).
3. Put USB/FIFO/GPIF/port registers back to power-on defaults.
4. Wait ~790 ms with Timer0 (long enough for macOS to see the detach).
5. Clear DISCON (reconnect).

The FX2 core re-enumerates with the VID/PID it latched from the EEPROM at
boot (0925:3881, no strings). It does *not* fall back to 04b4:8613. The
vendor app then connects normally.

### Requirements

Only tested on macOS.

- Python 3
- Python package `fx2` (gives `fx2tool`, pulls in `libusb1`), installed in
  `.venv` next to the script. The script switches to `.venv` by itself.
- libusb (`brew install libusb`, already there if sigrok is installed)
- sdcc + make, to build the firmware (`brew install sdcc`, make comes with
  Xcode command line tools)

Setup (macOS):

```
brew install sdcc libusb
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
make    # builds reenum.hex (the reset firmware)
```

Other OS:

- Linux: should work, untested. Needs permission to open the device: a udev
  rule for 0925:3881 (sigrok users usually have one) or run with sudo.
- Windows: unknown, untested. libusb needs a WinUSB driver on the device
  (Zadig). The vendor app may need its own driver, so they may not get along.

fx2tool somewhere else? Point the script to it:

```
FX2TOOL=/path/to/fx2tool ./fx2-reset
```

### Usage

```
./fx2-reset     # run after using sigrok
```

Output:

```
current: 0925:3881 sigrok fx2lafw
loading reenum firmware...
final:   0925:3881 (no strings)
OK: device is back in EEPROM-boot state; ...
```

If the device is clean, or the vendor app's firmware is running, it says
"nothing to do". `--force` loads anyway.

The script reads the state through libusb and loads with `fx2tool`. Load
command:

```
fx2tool -d 0925:3881 load reenum.hex
```

States it knows:

| what the device shows          | meaning                          |
|--------------------------------|----------------------------------|
| 0925:3881, no strings          | clean, vendor app works          |
| 0925:3881, `sigrok`/`fx2lafw`  | fx2lafw loaded, reset it         |
| 0925:3881, other strings       | vendor app firmware, leave it    |
| 1d50:608c                      | generic fx2lafw loaded, reset it |
| 04b4:8613                      | FX2 defaults, replug             |

### Checking state by hand (macOS)

```
ioreg -p IOUSB -l -w0 | grep -iE '"idVendor"|"idProduct"|"USB Product Name"'
# 2341 / 14465 = 0925:3881
```

Register dump (loads fx2tool's bootloader, so replug or reset after):

```
fx2tool -d 0925:3881 -B -F bin read_xram -f dump.bin 0xE400 0x300
```

The file is 0xE400-0xE6FF. Control registers start at offset 0x200:

```
xxd -s 0x200 -l 0x100 dump.bin
```


## Files

- `reenum.c`: reset firmware
- `Makefile`: sdcc -> reenum.ihx -> reenum.hex
- `fx2-reset`: check state, load firmware, verify
- `requirements.txt`: Python dependencies
