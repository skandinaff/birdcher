# G12B CSI research

## Result

Linux `v6.18.44` contains the upstream IMX415 sensor driver and binding, but no
G12B/A311D CSI receiver, CSI PHY capture, ISP adapter, or camera Device Tree
nodes. The current VIM3 DT likewise has display MIPI nodes only. Therefore an
IMX415 sensor module alone cannot create a capture device.

## Sources inspected

| Source | Revision | Finding |
| --- | --- | --- |
| Linux stable | `v6.18.44` (`1efe5d048a391de3ead2804b2e7f86376c356cc5`) | `drivers/media/i2c/imx415.c` and `sony,imx415.yaml` exist; no Meson/G12B CSI capture candidate was found under media-platform, PHY, or Amlogic DTS paths. |
| Armbian build | public main at `3b432cdc04fd7be53b49793aefefe8edb4f21455` | `current` maps to the 6.18 kernel generation and provides meson64 6.18 patches, none of which adds a CSI camera receiver. The deployed build ID `fd4ebfd1e` refers to an internal build repository and is not present in public history. |
| Khadas common drivers | `khadas-vims-5.15.y`, `3a11a86a02e759fc57fc79410215f7c0c3a0d8e0` | Provides hardware documentation only: legacy CSI/ISP stack is coupled to Amlogic media and ISP subsystems and is not a mainline V4L2 receiver. |
| `external/radxa-zero2pro-camera` | submodule `779d06bdf6870d3c54f99a71377b016b74fdf331` | A working, runtime-loaded G12B IMX415/ISP reference on Linux 6.1. It proves the blocks can capture, but is board-specific and cannot be loaded on this VIM3 6.18 kernel without a careful port. |

## Vendor hardware facts

The Khadas G12B DTS describes these legacy blocks:

| Block | Register range | Interrupt(s) |
| --- | --- | --- |
| CSI PHY/hosts | `0xff650000/0x2000`, `0xff652000/0x2000`, `0xff63c300/0x100`, `0xff654000/0x100`, `0xff654400/0x100` | SPI 41, 42, 72, 74, 87, 88 |
| ISP adapter | `0xff650000/0x6000` | SPI 179 |
| ISP scaler | `0xff655400/0x1000` | SPI 17 |
| ISP | `0xff140000/0x40000` | SPI 142 |

The vendor VIM3 sensor node uses `CLKID_GEN` and GPIO-expander lines 3 (reset)
and 2 (power down), but it is a legacy `soc,sensor` node for an `os08a10` at
I²C address `0x6c`. Those values are not evidence for this IMX415 module and
must not be copied into a modern IMX415 node.

## Implementation decision

1. Compile and validate the upstream IMX415 module first.
2. Determine VIM3-specific I²C, clock, rail, reset, and power-down wiring from
   the VIM3 schematic and direct probe evidence.
3. Port or replace the G12B receiver/ISP path only after the sensor probes.
   The Radxa implementation is a valuable regression/reference target, not a
   drop-in driver: it is built against different 6.1 kernel headers and has
   different connector routing.
4. Do not install a runtime overlay or an ISP/clock module before those facts
   and a 6.18 build are validated.
