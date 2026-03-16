# QNAP IT8528 Embedded Controller Emulation for QEMU

## Overview

`qemu-qnap-it8528` is a QEMU device emulation for the ITE8528 Embedded Controller (EC) found in many QNAP NAS devices. 

This project serves as a companion, test target for the [qnap8528](https://github.com/0xGiddi/qnap8528) Linux kernel module, which exposes the physical IT8528 EC functionalities (like fan control, thermal sensors, LEDs, and VPD) to the standard Linux subsystems.

> **NOTE 1:** This emulation is specific to IT8528 EC running QNAPs specific firmware and does not emulate a generic IT8528 device.

> **NOTE 2:** This emulation does not run the original QNAP EC firmware internally, it's custom designed around registers and protocols that I have reverse engendered for the qnap8528 kernel module. Newly discovered EC features may require adding implementation code.

## Why Emulate QNAPs ITE8528?

* **Safe Kernel Module Testing:** Develop and test the `qnap8528` kernel driver without the risk of causing a kernel panic on live NAS.
* **Safe Script Dev Environment:** Writing and testing custom scripts that interact with the LEDs, fans or buttons is made easier and safer, making testing automation and testing edge cases possible.
* **Future Reverse Engineering:** Monitor and trace how QNAP's proprietary OS interacts with the Embedded Controller.

## Planned/Supported Features
Currently, all features that are supported by the qnap8528 kernel module are supported. Fans and temperatures are static, but I have plans on expanding this project to simulate such fluctuations.

## Repository Structure

* `Makefile` - Build scripts
* `sources/*` - Device C source files defining the QEMU device.
* `patches/*` - Patch files to apply against the upstream QEMU source tree to wire in the device
* `scripts/regsutil.py` - Utility to generate/modify/inspect registers state backing file.
* `scripts/vpdutil.py` - Utility to generate/modify/inspect VPD backing file.
* `scripts/panel.html` - TBD

## Compiling

### Build Environment
See [QEMUs Setup build environment instructions](https://www.qemu.org/docs/master/devel/build-environment.html) for instructions on setting up an environment capable of compiling the original QEMU project.

### The Build Script
The `Makefile` provided in the root directory of this project contains everything that is needed to fetch, patch and build QEMU with the qnap-it8528 device wired in. By default, the makefile uses QEMU version *10.0.0*, QEMUs [QOM](https://qemu-project.gitlab.io/qemu/devel/qom.html) API changes with versions and version 10.0.0 is the version this device is written for, however, the QEMU version can be overridden with the `QEMU_VERSION` argument.

The following makefile targets exist:
* `fetch` - Fetches QEMU sources into `./qemu`
* `copy_sources` - Copies device `.c` and `.h` files into QEMUs source tree.
* `patch` - Applies a series of patches to wire QEMU to compile the device.
* `configure` - Runs QEMU build systems configure script.
* `build` - Runs QEMU build scripts (multithreaded).
* `install` - Will install the built project into `./install`
* `update_sources` - Updates device files only and builds without re-patching.
* `clean` - Clean QEMU build `clean` target
* `distclean` - Removes all build related files and QEMU source tree.

Running `make` with no target will all stages of the build except for the `install` target.

### Quick Build
```bash
# Clone this repo
git clone https://github.com/youruser/qemu-qnap-it8528.git
cd qemu-qnap-it8528
# Build QEMU with qnap-it8528 device
make
```

## Preparing Registers and VPD Backing Files
The QEMU qnap-it8528 device default registers and VPD tables are zeroed. There is not default values built in to the device. The device accepts two backing files that can be used to pre-populate the EC register space and the VPD tables with sensible information (FW version, fan speeds, BP/MB codes etc.). 

### Register backing file script `scripts/regsutil.py`
```bash
# Create a regs.bin file with default values
python3 scripts/regsutil.py create regs.bin
# Or, create a regs.bin file while overriding default values
python3 scripts/regsutil.py create regs.bin fw_ver=ABCDEFGH
```
```bash
# Dump the content of a register space baking file
python3 scripts/regsutil.py dump regs.bin
```
```bash
# Change a register value
python3 scripts/regsutil.py amend regs.bin <key>=<value>
```

To view the default values when running `create` without parameters, `create` a file and the use `dump`. The following register keys are available to override on `create` or `amend`:

| Key | Description |
|---|---|
| `fw_ver=<str>` | Firmware version string, 8 chars |
| `eup_mode=<0\|1>` | EuP mode off/on |
| `panel_bright=<0-100>` | Panel LED brightness |
| `pwr_recovery=<0\|1\|2>` | AC power recovery off/on/last |
| `led_status=<0-5>` | Status LED (0=off 1=green 2=red 3=g-blink 4=r-blink 5=bicolor) |
| `led_usb=<0-2>` | USB LED (0=off 1=blink 2=on) |
| `led_ident=<1\|2>` | Ident LED (1=on 2=off) |
| `led_jbod=<0\|1>` | JBOD LED (1=on 2=off)|
| `led_10g=<0\|1>` | 10GbE LED (1=on 2=off)|
| `fan<id>_rpm=<rpm>` | Set fan RPM (e.g. `fan0_rpm=1200`) |
| `fan_pwm_<0-3>=<0-100>` | Set fan bank PWM value|
| `temp<id>=<celsius>` | Set temperature sensor (e.g. `temp0=45`) |

**Note:**
Setting fan PWM values will update all the fans RPM values in that PWM bank that have already been set, to avoid this (but this is not true to real hardware) first set fan PWM value and then fan RPM value.

### VPD backing file script `scripts/vpdutil.py`
The QEMU device loads a binary VPD (Vital Product Data) file containing
manufacturing information about the mainboard, backplane and enclosure. This
is how the qnap8528 kernel module identifies the device model and selects the correct
configuration from its built-in config table.

The VPD space consists of four 512-byte tables. Tables 0 and 1 hold mainboard
and backplane data respectively, tables 3 and 4 are not used by the kernel module and are kept zeroed. The total file size is 2048 bytes.

#### Creating a VPD file
Similar to the `regsutil.py` file interface.
```bash
python3 scripts/vpdutil.py create vpd.bin
```

Creates a VPD file with built-in defaults. The default information matches a TS-473A device with arbitrary serial and manufacturing dates. The following key filed keys are available for `create` or `amend` commands:

| Key | Description | Max len | Example |
|---|---|---|---|
| `mb_manuf` | Mainboard manufacturer | 16 chars | `BTC Systems` |
| `mb_vendor` | Mainboard vendor | 16 chars | `QNAP Systems` |
| `mb_name` | Mainboard name | 16 chars | `SATA-6G-MB` |
| `mb_model` | Mainboard model code (used for config matching) | 32 chars | `70-0Q07D0250` |
| `mb_serial` | Mainboard serial number | 16 chars | `M1234567890` |
| `mb_date` | Mainboard manufacture date | YYYYMMDD | `20200101` |
| `enc_ser_mb` | Enclosure serial (mainboard source) | 16 chars | `Q000000000M` |
| `enc_nick` | Enclosure nickname | 16 chars | `MyNAS` |
| `bp_manuf` | Backplane manufacturer | 16 chars | `BTC Systems` |
| `bp_vendor` | Backplane vendor | 16 chars | `QNAP Systems` |
| `bp_name` | Backplane name | 16 chars | `LF-SATA-BP` |
| `bp_model` | Backplane model code (used for config matching) | 32 chars | `70-1Q07N0200` |
| `bp_serial` | Backplane serial number | 16 chars | `B1234567890` |
| `bp_date` | Backplane manufacture date | YYYYMMDD | `20200102` |
| `enc_ser_bp` | Enclosure serial (backplane source) | 16 chars | `Q000000000B` |

Dates are specified as `YYYYMMDD` on input and decoded to a UTC timestamp on
output. Internally they are stored as minutes elapsed since 2013-01-01 00:00 UTC,
matching the format used by the real EC firmware.

## Using The Device
Use QEMU as you would normally and just add `-device qnap-it8528` to add an empty (no register state, no VPD) QNAP IT8528 EC to the VM. To emulate an EC with proper data and register information, use the following properties to pass the register space and VPD backing files created with `vpdutil.py` and `regsutil.py`, for example `-device qnap-it8528,regs-file=regs.bin,vpd-file=vpd.bin`.

| Property | Description | Default |
|---|---|---|
| `regs-file` | Path to binary register state file | (empty, all zero) |
| `vpd-file` | Path to binary VPD tables file | (empty, all zero) |
| `chip-id` | SuperIO chip ID reported at 0x2e/0x2f | `0x8528` |

### Monitor commands
The following commands are available from the QEMUs monitor console:

#### `qnap_it8528_info` - Get EC information

Prints firmware version, CPLD version, EC phase, LEDs, buttons, temperatures,
fan RPMs, fan bank PWMs and active disk LED states.

#### `qnap_it8528_press` - Simulate a physical button press

Simulates a button press such as the from USB *COPY* button. The button register bit is set for the given duration in milliseconds, then cleared automatically by a timer. The command format is `qnap_it8528_press <Button Name> <Press Suration in MS>`, currently `CHASSIS`, `COPY` and `RESET` buttons are available. Pressing a button while another button is active will reset the press duration to the new value. 

> NOTE TO FUTURE ME: Fix bug/implement multiple button press.

Examples:
```bash
(qemu) qnap_it8528_press CHASSIS 100 # Press the CHASSIS button for 100ms
(qemu) qnap_it8528_press COPY 3000   # Press the COPY button for 3 seconds
(qemu) qnap_it8528_press RESET 10000 # Press the RESET button for 10 seconds
```

## Other Info and Troubleshooting

### Fan register indexing
Fan RPM registers are split into high and low bytes at non-contiguous addresses.
The kernel module uses 1-based fan IDs in the device config (for hwmon channel
matching) but 0-based indexes for the actual register address calculation.
The emulator pre-populates fan ID 0 (not fan ID 1) for a config listing fan 1,
to match what the kernel reads.

**`sensors` shows 0 RPM** : Ensure `regs.bin` was created with `regsutil.py create` and that the fan IDs populated match what the kernel actually reads (0-based, not 1-based).

## Planned Features / Known bugs
- Feature: Fan RPM and temperature fluctuations.
- Bug: Pressing a different button while another is active will soft lock the first button pressed.
- Feature: Visual LED/fan monitor via QMP for script development ease.
- Feature: HMP commands for direct settings registers, temperature and fan RPM setting (simulate fan failure).
- Feature: Real world EC read/write delays and busy/timeout conditions.
- Feature: Implement device interface for QEMU VM state for saving/loading running VMs.


## Disclaimer
This project is not affiliated with, endorsed by, or connected to QNAP Systems Inc. or ITE Tech. Inc. Use at your own risk.