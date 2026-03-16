
# NOT ALL FILES UPLOADED YET

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

* `<root>` - Build scripts, documentation
* `sources/` - C source files defining the QEMU device model logic.
* `patches/` - Patch files to apply against the upstream QEMU source tree to wire in the device.
* `scripts/` - Emulation utility scripts

## Getting Started
TBD

## Disclaimer
This project is not affiliated with, endorsed by, or connected to QNAP Systems Inc. or ITE Tech. Inc. Use at your own risk.