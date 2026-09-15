# CSI bring-up log

## 2026-09-13 — discovery and safe preparation

- Preserved `/boot` and decompiled the running Device Tree before CSI work.
- Confirmed the existing `/dev/video0` is the Meson video decoder, not a
  camera. No media-controller nodes currently exist.
- Confirmed that the stock 6.18.44 meson64 configuration lacks the IMX415
  sensor driver.
- Obtained the matching upstream v6.18.44 source and the upstream IMX415
  binding/driver.
- Found no reusable upstream G12B CSI receiver implementation.
- Inspected both the Khadas vendor hardware description and the staged Radxa
  Zero 2 Pro G12B IMX415 reference. The latter has a working runtime-loaded
  capture stack on Linux 6.1 but must be ported and board-adapted for this
  VIM3/Linux 6.18 environment.
- Reviewed the local IMX415 datasheet. It eliminates uncertainty about supply
  sequencing, XCLR timing, allowed clocks, CSI lane modes, and I²C addresses;
  it does not identify the VIM3 connector wiring.
- Started an isolated native build of a v6.18.44 test Image with only the
  upstream IMX415 Kconfig change. It has not been installed or booted.
- Checked the Khadas VIM3 V15 schematic against the running Device Tree:
  Camera0 J12 is a four-lane CSI connector; its I2C_AO bus is the live
  i2c-0 controller; its MCLK is GPIOAO_10 (CLK12_24); and its camera
  reset/power-down signals originate from the live TCA6408 GPIO expander at
  address 0x20.
- The connector supplies only 1.8 V and 3.3 V. The IMX415 needs 1.1 V, 1.8 V,
  and 2.9 V with a defined order, so camera-board regulator documentation or
  measurement remains mandatory before an overlay can claim supplies.
- No GPIO, I2C address, Device Tree, module, boot, or camera configuration
  was changed as part of this wiring analysis.
