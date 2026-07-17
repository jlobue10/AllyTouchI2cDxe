# AllyTouchI2cDxe

**Status: experimental / work in progress — not yet functional.**

A UEFI driver that aims to make the **ASUS ROG Xbox Ally X** built-in touchscreen
(**Novatek NVTK0603**, an I2C-HID device) usable in the [rEFInd](https://www.rodsbooks.com/refind/)
boot menu, by producing `EFI_ABSOLUTE_POINTER_PROTOCOL`. rEFInd consumes that
protocol natively, so no rEFInd changes are needed — the driver is meant to load
from rEFInd's `drivers_x64/` folder alongside
[`UsbXbox360Dxe.efi`](https://github.com/jlobue10/UsbXbox360Dxe).

This is a sibling to the Xbox 360 controller driver: that one binds USB gamepads;
this one binds the I2C-HID touch panel that a USB driver structurally cannot see.

## Why a whole new driver

The Ally X touchscreen is **not** a USB device — it is a Novatek NVTK0603 on the
SoC's **I2C** bus (ACPI `AMDI0010` DesignWare controller), spoken to with
HID-over-I2C. A USB driver never sees it. Full rationale, architecture, and the
feasibility spike are in **[DESIGN.md](DESIGN.md)**.

## Current state

The driver is implemented and builds; first on-hardware test answered the
go/no-go question: on a **normal** boot the firmware leaves the I2C controller
tile power-gated, so the original build failed its probe — and returning that
error from the entry point kept rEFInd from launching. Both are fixed:

- the entry point **never returns an error** (worst case the driver sits idle
  and retries in the background for ~30 s), so rEFInd always launches;
- the driver **powers the I2C controller tile on itself** via the FCH AOAC
  registers (the same thing ACPI `_PS0` does) and programs 400 kHz timing.

### Tip: guaranteed touch bring-up

If touch still does not respond, press **volume up during the boot-animation
splash**: the firmware then powers *and* initializes the touchscreen before the
boot loader starts, and the driver finds a live panel immediately. This is the
supported fallback while we learn whether the Novatek panel needs firmware-side
init that the driver cannot replicate.

### Start here

1. **Collect hardware facts** — run [`tools/collect-hardware-info.sh`](tools/collect-hardware-info.sh)
   on the Ally X booted into Linux. It dumps the ACPI/I2C/GPIO parameters the
   driver must hardcode (controller MMIO base, slave address, HID descriptor
   register, reset/IRQ GPIOs) and prints a checklist to fill in.
2. **Run the go/no-go probe** — see [`tools/uefi-probe.md`](tools/uefi-probe.md).
   Phase 0 is a zero-code `mm` check in the UEFI Shell; Phase 1 builds
   [`tools/probe/AllyTouchProbe.efi`](tools/probe/AllyTouchProbe.c) (same EDK2
   toolchain as `UsbXbox360Dxe`) and reports whether the Goodix panel ACKs
   without any bring-up.

The probe result decides whether the driver is "just an I2C-HID reader"
(scenario a) or needs a small reset-GPIO/`_PS0` power-on step (scenario b).

## Layout

```
DESIGN.md                     feasibility spike, architecture, sources
src/
  DwI2c.h                     DesignWare I2C register defs (standard)
  FchAoac.h                   AMD FCH AOAC power-gating regs (un-gate I2C tile)
  I2cHid.h                    HID-over-I2C structs + opcodes (standard)
  AllyTouchI2cDxe.inf/.c      the driver (Layers 0-3)
tools/
  collect-hardware-info.sh    Linux-side ACPI/I2C/GPIO dumper -> start here
  uefi-probe.md               the go/no-go procedure
  probe/AllyTouchProbe.c/.inf standalone UEFI Shell liveness probe
```

## Building

Uses the standard EDK2 build (same workspace/toolchain as `UsbXbox360Dxe`). The
probe app and the driver are both EDK2 modules; point `build` at their `.inf` or
add them to a platform DSC `[Components]`. See `tools/uefi-probe.md` for the
probe build line.

## License

BSD-2-Clause-Patent (matches EDK2 and the sibling driver repos). See
[LICENSE](LICENSE).

## Credits / references

Builds on prior art from the Linux kernel (`i2c-designware`, `i2c-hid`,
`goodix`), coreboot and u-boot (DesignWare master), Microsoft Project Mu
`HidPkg` (HID-over-I2C → AbsolutePointer pattern), and the
`ty2/goodix-gt7868q` driver. Full citations in DESIGN.md.
