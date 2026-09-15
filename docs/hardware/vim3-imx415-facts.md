# VIM3 + IMX415 Camera Hardware Facts

## Purpose

This file is the hardware-source-of-truth for bringing up the IMX415 CSI camera on a Khadas VIM3.

Use this hierarchy when values disagree:

1. Khadas VIM3 schematic — physical board wiring
2. Khadas VIM3 vendor DTS / vendor camera code — how Khadas actually drives that wiring
3. Sony IMX415 datasheet — sensor electrical/protocol requirements
4. `winedarksea/radxa-zero2pro-camera` — working G12B/A311D CSI/ISP implementation reference
5. Inference / experimentation on the live board

Never copy Radxa board-specific GPIO, I2C, MCLK, or regulator values onto VIM3 without verification.

---

# 1. Target Hardware

Board:

```text
Khadas VIM3
SoC: Amlogic A311D
SoC family: G12B
```

Camera:

```text
Sony IMX415
MIPI CSI-2
8.46 MP effective pixels
```

Current software target:

```text
Armbian 26.8.3
Ubuntu 26.04
Linux 6.18.44-current-meson64
```

---

# 2. IMX415 Sensor Facts

Source:

```text
Sony IMX415-AAQR-C datasheet
```

## Supported master clocks

The sensor accepts these INCK frequencies:

```text
24 MHz
27 MHz
37.125 MHz
72 MHz
74.25 MHz
```

24 MHz is therefore a valid clock source.

## CSI output

Supported:

```text
MIPI CSI-2
2 lanes
4 lanes
RAW10
RAW12
```

The sensor can run in either 2-lane or 4-lane mode.

## Resolution

Relevant dimensions:

```text
total pixels:       3864 × 2228
effective pixels:   3864 × 2192
active pixels:      3864 × 2176
recommended image:  3840 × 2160
```

## Maximum advertised full-frame rates

Datasheet headline figures:

```text
12 bit: ~60.3 fps
10 bit: ~90.9 fps
```

Actual achievable rate depends on lane count, lane rate, INCK, HMAX/VMAX and selected mode.

## Supply rails

Sensor silicon nominal supplies:

```text
AVDD  = 2.9 V analog
OVDD  = 1.8 V interface
DVDD  = 1.1 V digital
```

These are sensor-level requirements.

Do not assume the VIM3 CSI connector exposes these rails directly; the camera module may contain its own regulators.

## Important pins

Relevant sensor pins/signals include:

```text
INCK   master clock
XCLR   system clear / reset
SDA    I2C data
SCL    I2C clock

DMO1P/N
DMO2P/N
DMO3P/N
DMO4P/N
DCKP/N
```

The last five pairs are four CSI data lanes plus CSI clock.

## I2C limits

The datasheet supports:

```text
Standard/Fast mode: up to 400 kHz
Fast-mode Plus:     up to 1 MHz when INCK >= 16 MHz
```

For bring-up, prefer 100 or 400 kHz unless there is a reason to use faster I2C.

## Reset requirement

`XCLR` is the sensor reset/system-clear input.

The datasheet specifies a minimum low pulse width of:

```text
4 / fINCK
```

Do not rely on power-on reset state if the board provides a controllable reset signal.

---

# 3. VIM3 CSI Connector Facts

Reference schematic:

```text
Khadas VIM3 schematic
https://dl.khadas.com/products/vim3/schematics/
```

The VIM3 CSI connector exposes the following camera-related nets:

```text
MIPI_CSI_D0P / D0N
MIPI_CSI_D1P / D1N
MIPI_CSI_D2P / D2N
MIPI_CSI_D3P / D3N

MIPI_CSI_CLKAP / CLKAN
MIPI_CSI_CLKBP / CLKBN

CAM_MCLK
CAM_RESET
CAM_PDN0
CAM_PDN1

I2C_AO_SCL
I2C_AO_SDA

VDDIO_AO18
VCC3.3V
```

Important:

- VIM3 exposes four CSI data lanes.
- There are two CSI clock-pair nets in the board schematic.
- Do not assume which clock pair the IMX415 module uses until verified from module wiring / known working Khadas configuration.
- Camera control I2C is routed through the AO I2C domain in the VIM3 schematic.

Board revision matters.

Before hardcoding connector details, determine the physical VIM3 board revision and cross-check against the matching Khadas schematic revision.

---

# 4. VIM3 Vendor DTS Camera Control Facts

Khadas vendor DTS for VIM3 contains a camera sensor abstraction with:

```dts
clocks = <&clkc CLKID_GEN>;
clock-names = "gen_clk";

reset = <&gpio_expander 3 GPIO_ACTIVE_HIGH>;
pwdn  = <&gpio_expander 2 GPIO_ACTIVE_HIGH>;
```

This tells us several useful board-level facts:

```text
camera reset is controlled through the onboard GPIO expander
camera power-down is controlled through the onboard GPIO expander
Khadas drives a generated camera clock through CLKID_GEN
```

The vendor DTS example names another sensor by default, so:

```text
sensor-name from vendor DTS is NOT authoritative for our physical IMX415
```

The wiring information is useful; the vendor `soc,sensor` abstraction is not the target architecture.

For Linux 6.18 use the upstream IMX415 DT binding where possible.

---

# 5. GPIO Expander

The VIM3 schematic contains:

```text
TCA6408A
I2C address: 0x20
```

Camera-related control nets appear on this expander:

```text
CAM_RESET
CAM_PDN0
CAM_PDN1
```

The vendor DTS associates:

```text
expander line 3 -> reset
expander line 2 -> pwdn
```

Before using those values in a new overlay:

1. identify the expander node in the live mainline DT
2. confirm its GPIO numbering
3. confirm the VIM3 board revision
4. inspect line ownership with `gpioinfo`

Do not assume Linux line numbering from the vendor kernel is identical under mainline.

---

# 6. G12B / A311D Camera Hardware Blocks

Khadas vendor G12B DTS/source exposes the following hardware blocks:

```text
ISP:
    isp@ff140000

CSI / adapter region:
    isp-adapter@ff650000
    phy-csi@ff650000

scaler:
    isp-sc@ff655400
```

Known compatibles from vendor code:

```text
"arm, isp"
"amlogic, isp-adapter"
"amlogic, phy-csi"
"amlogic, isp-sc"
```

Important physical register regions include:

```text
0xff140000   ISP
0xff650000   CSI PHY / adapter area
0xff652000   second CSI PHY region
0xff654000   CSI host
0xff654400   CSI host
0xff655400   ISP scaler
```

Treat the exact sizes, IRQs and register names in the working reference repo / Khadas source as authoritative implementation references.

---

# 7. Working A311D Reference Implementation

Primary implementation reference:

```text
https://github.com/winedarksea/radxa-zero2pro-camera
```

Why it matters:

```text
Board:   Radxa Zero 2 Pro
SoC:     Amlogic A311D / G12B
Camera:  Sony IMX415
Kernel:  Linux 6.1.x
```

The repo reports working end-to-end capture:

```text
IMX415
  ↓
MIPI CSI-2
  ↓
Amlogic CSI PHY
  ↓
adapter
  ↓
Amlogic ISP
  ↓
V4L2
```

Reported working mode:

```text
3864 × 2192
4 CSI lanes
60 fps
```

The repo provides:

```text
isp-clkc/
ao-mclk/
isp-module/
imx415/
dtbo-loader/
overlays/
scripts/
```

Use this as the main implementation base/reference for the missing G12B camera stack.

---

# 8. Radxa Values That Must NOT Be Copied to VIM3

The Radxa Zero 2 Pro board wiring is different.

Do NOT blindly copy these Radxa-specific values:

```text
I2C bus/path
camera reset GPIO
camera power-down GPIO
camera MCLK pin
AO pinmux programming
fixed-regulator assumptions
connector pin mapping
```

In particular, the Radxa implementation uses board-specific facts such as:

```text
sensor on i2c3
MCLK on GPIOAO_10 / CLK12_24
reset on GPIOA_11
```

Those values describe Radxa Zero 2 Pro, not VIM3.

Shared G12B register blocks and CSI/ISP logic are likely reusable.

Board glue is not.

---

# 9. What Can Probably Be Reused Almost Directly

Likely shared between Radxa Zero 2 Pro and VIM3 because both use A311D/G12B:

```text
CSI PHY register map
CSI host register map
ISP register map
ISP adapter register map
interrupt identities
clock topology inside G12B
vendor ISP logic
V4L2 integration logic
IMX415 mode programming
CSI packet handling
DMA / ISP frame pipeline behaviour
```

Still verify kernel-version API differences between Linux 6.1 and 6.18.

---

# 10. What Must Be Re-derived for VIM3

Board-specific values to establish before full load:

```text
exact I2C controller used by CSI connector
IMX415 I2C address on this module
MCLK source and route
MCLK frequency
reset GPIO mapping under mainline
PWDN GPIO mapping under mainline
whether CAM_PDN0 or CAM_PDN1 applies
camera-module power rail behaviour
CSI clock pair used
lane ordering
number of active data lanes
GPIO polarity
```

These must come from:

```text
VIM3 schematic
+
Khadas vendor DTS
+
live hardware observation
+
IMX415 datasheet
```

---

# 11. Live-Hardware Verification Commands

## Identify board revision / model

```bash
cat /proc/device-tree/model
cat /proc/device-tree/compatible | tr '\0' '\n'
```

Also inspect the physical PCB revision if accessible.

## GPIO controllers and lines

Install:

```bash
sudo apt install gpiod
```

Then:

```bash
gpioinfo
```

Look for:

```text
CAM_RESET
CAM_PDN0
CAM_PDN1
```

Names may not survive into the current mainline DT.

## I2C

```bash
i2cdetect -l
```

Inspect the suspected camera bus only after MCLK/reset/power have been configured.

Avoid repeatedly probing every I2C address while actively developing sensor power/reset sequencing.

## Clock tree

```bash
sudo cat /sys/kernel/debug/clk/clk_summary
```

Search:

```bash
sudo grep -Ei 'gen|mipi|csi|isp' /sys/kernel/debug/clk/clk_summary
```

## Current DT

```bash
sudo dtc -I fs -O dts /proc/device-tree > current-running.dts
```

Search:

```bash
grep -Ei 'i2c|gpio|expander|clock|csi|isp|camera' current-running.dts
```

---

# 12. Recommended Bring-Up Sequence

Do not try to load the full stack first.

Use this order:

```text
1. Determine exact VIM3 board revision
2. Verify GPIO expander under mainline
3. Identify reset / pwdn lines
4. Establish the correct MCLK
5. Verify MCLK electrically or via known clock state
6. Release sensor reset/powerdown
7. Detect IMX415 on I2C
8. Load/probe upstream IMX415 driver
9. Add G12B CSI/ISP clocks
10. Add CSI PHY / adapter / ISP
11. Apply runtime DT overlay
12. Confirm V4L2/media graph
13. Capture one frame
14. Capture continuously
```

At each step record:

```text
what changed
expected result
actual result
dmesg difference
rollback command
```

---

# 13. Agent Constraints

The agent must follow these rules:

```text
Treat VIM3 schematic as authoritative for physical board wiring.
Treat Khadas vendor DTS as authoritative evidence for how Khadas controlled that wiring.
Treat Sony IMX415 datasheet as authoritative for sensor requirements.
Treat radxa-zero2pro-camera as the working G12B implementation reference.
```

Never use Radxa board values merely because both boards use A311D.

Never modify `/boot`, SPI or U-Boot merely to test a camera DT.

Prefer runtime-loadable modules and runtime DT overlay during bring-up.

Preserve the current etnaviv/NPU path.

---

# 14. Key Sources

Khadas VIM3 schematics:

```text
https://dl.khadas.com/products/vim3/schematics/
```

Khadas common drivers:

```text
https://github.com/khadas/common_drivers
```

Working A311D + IMX415 reference:

```text
https://github.com/winedarksea/radxa-zero2pro-camera
```

Linux upstream IMX415 driver:

```text
https://github.com/torvalds/linux/blob/master/drivers/media/i2c/imx415.c
```

Sony sensor document:

```text
IMX415-AAQR-C datasheet
```

Keep a local copy of the datasheet under:

```text
docs/reference/imx415-datasheet.pdf
```

