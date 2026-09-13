# TASK: Bring Up IMX415 CSI Camera on Khadas VIM3 / Armbian 6.18

## Objective

Bring up an **IMX415 MIPI CSI-2 camera** on a **Khadas VIM3 (Amlogic A311D / G12B)** running:

- Armbian 26.8.3
- Ubuntu 26.04
- Linux 6.18.44-current-meson64

Long-term architecture:

```text
IMX415 CSI camera
    ↓
V4L2 / media pipeline
    ├── live browser stream
    └── object detection
          ↓
       Mesa Teflon
          ↓
       etnavivn
          ↓
       A311D NPU
```

This task is only about getting the CSI camera working cleanly on the modern Armbian/mainline stack.

Do **not** switch back to the Khadas 5.15 BSP as the solution.

---

## Current Known State

The board is working and booted from eMMC.

Current platform:

```text
Khadas VIM3
Amlogic A311D / G12B
Armbian 26.8.3
Ubuntu 26.04
Linux 6.18.44-current-meson64
```

The NPU is detected by etnaviv as GC8000. Preserve this functionality.

Existing `/dev/video0` is **not a camera**. It is:

```text
Amlogic Video Decoder
driver: meson-vdec
platform: meson-vdec
```

Current state:

```text
no /dev/media*
no CSI camera V4L2 device
CONFIG_VIDEO_IMX415 is not set
modinfo imx415 -> module not found
```

The active Device Tree contains no obvious `camera`, `csi`, `mipi`, or `isp` nodes.

The physical camera is **Sony IMX415**, connected through the VIM3 MIPI CSI connector.

Observed I2C buses:

```text
i2c-0:
0x0c
0x0e
0x18 claimed
0x20 claimed
0x22
0x51 claimed

i2c-1:
no devices detected

i2c-2:
0x30
0x50
0x51
0x54
```

Do not assume the sensor is absent because it is not visible in `i2cdetect`. It may currently be powered down, held in reset, or missing its external clock.

---

## Important Research Findings

### 1. IMX415 already exists upstream

Modern Linux already contains:

```text
drivers/media/i2c/imx415.c
```

Prefer the upstream V4L2 sensor driver.

Do not port the Khadas vendor IMX415 driver unless the upstream driver proves insufficient.

### 2. The missing part is mainly the A311D / G12B camera pipeline

Khadas vendor sources contain G12B-specific camera support under:

```text
drivers/armisp-g12b/
```

including:

```text
drivers/armisp-g12b/subdev/
drivers/armisp-g12b/v4l2_dev/
```

and:

```text
CONFIG_AMLOGIC_ARM_ISP_G12B
```

Use this as a hardware reference implementation, not as code to copy wholesale.

### 3. Known G12B camera hardware blocks

Khadas vendor G12B DTS contains:

```text
phy-csi@ff650000
isp-adapter@ff650000
isp-sc@ff655400
isp@ff140000
```

Known compatibles include:

```text
amlogic,phy-csi
amlogic,isp-adapter
amlogic,isp-sc
arm,isp
```

The CSI register region starts around:

```text
0xff650000
```

Take exact resource ranges, IRQs, clocks and resets from the vendor DTS/source and verify them.

### 4. Known VIM3 camera control signals

Khadas VIM3 vendor DTS contains camera control information including:

```text
clocks = <&clkc CLKID_GEN>;
clock-names = "gen_clk";

reset = <&gpio_expander 3 GPIO_ACTIVE_HIGH>;
pwdn  = <&gpio_expander 2 GPIO_ACTIVE_HIGH>;
```

Use this only as wiring/reference information. The modern upstream IMX415 node should use standard DT bindings rather than the old `soc,sensor` abstraction.

---

# Desired Architecture

Prefer:

```text
Linux 6.18
   │
upstream IMX415 driver
   │
G12B MIPI CSI-2 receiver
   │
V4L2 / media-controller
   │
RAW Bayer capture
```

The first goal is not perfect RGB, ISP tuning, Frigate or NPU inference.

The first goal is:

```text
IMX415 probes correctly
        ↓
CSI receiver receives frames
        ↓
RAW Bayer frames reach userspace
```

---

# Phase 1 — Preserve the Working System

Before changing kernel or DT:

```bash
uname -a
cat /etc/armbian-release
cat /boot/armbianEnv.txt
ls -lah /boot
lsblk -o NAME,SIZE,FSTYPE,MOUNTPOINTS
dpkg -l | grep -Ei 'linux-image|linux-dtb|linux-headers|armbian'
```

Save:

```bash
sudo cp -a /boot ~/boot-backup
```

Also save the live DT if possible:

```bash
sudo dtc -I fs -O dts /proc/device-tree > ~/current-running.dts
```

Create:

```text
docs/kernel-baseline.md
```

Do not touch SPI, U-Boot, the eMMC partition table, or the bootloader.

---

# Phase 2 — Inspect Armbian Kernel Source and Config

Identify:

- Armbian build branch
- Linux source revision
- current `meson64` kernel config
- VIM3 DT source
- relevant G12B DTSI

Stay as close as possible to:

```text
6.18.44-current-meson64
```

Verify source contains:

```text
drivers/media/i2c/imx415.c
Documentation/devicetree/bindings/media/i2c/sony,imx415.yaml
```

Enable at least:

```text
CONFIG_MEDIA_SUPPORT=y
CONFIG_MEDIA_CONTROLLER=y
CONFIG_VIDEO_DEV=y
CONFIG_VIDEO_V4L2=y
CONFIG_VIDEO_V4L2_SUBDEV_API=y
CONFIG_VIDEO_IMX415=m
```

Keep current etnaviv and meson-vdec functionality intact.

---

# Phase 3 — Build the Smallest Possible Test Kernel

First build:

```text
same Armbian 6.18 kernel
+ CONFIG_VIDEO_IMX415=m
```

Do not add CSI/ISP changes yet.

Install the test kernel while preserving:

- previous known-good kernel
- previous known-good DTB
- a rollback path

After reboot verify:

```bash
uname -a
modinfo imx415
grep CONFIG_VIDEO_IMX415 /boot/config-$(uname -r)
sudo modprobe imx415
```

Expected:

```text
CONFIG_VIDEO_IMX415=m
```

Document in:

```text
docs/imx415-kernel.md
```

This phase is successful even if the sensor does not probe yet.

---

# Phase 4 — Build a Correct Mainline-Style IMX415 DT Node

Do not use the Khadas vendor `soc,sensor` abstraction.

Use the upstream IMX415 binding:

```text
Documentation/devicetree/bindings/media/i2c/sony,imx415.yaml
```

Determine and configure:

- correct I2C controller
- correct sensor I2C address
- external clock
- reset GPIO
- power-down GPIO
- regulators if required
- CSI lane count
- link frequencies
- standard V4L2 endpoint graph

Determine real values from:

1. VIM3 schematics
2. Khadas vendor DTS
3. Khadas vendor IMX415 implementation
4. upstream IMX415 driver/binding

Do not guess.

---

# Phase 5 — Prove Sensor Communication

Before implementing full CSI capture, prove that the sensor can probe.

Inspect:

```bash
dmesg | grep -Ei 'imx415|camera|sensor|i2c'
ls -l /dev/v4l-subdev*
find /sys/bus/i2c/devices -maxdepth 2 -type f -name name -print -exec cat {} \;
```

If probe fails, verify:

- clock
- reset
- pwdn
- I2C bus
- I2C address
- supplies
- lane configuration

Do not move to CSI implementation until the sensor probes reliably.

---

# Phase 6 — Search for Existing Mainline G12B CSI Work

Before porting anything, search current Linux, Armbian patches and recent Meson/media work for:

```text
meson
g12b
csi
csi2
mipi
media
```

Inspect at least:

```text
drivers/media/platform/
drivers/phy/amlogic/
drivers/media/platform/amlogic/
```

if present.

Search recent upstream commits, patch series and mailing-list work.

Document findings in:

```text
docs/g12b-csi-research.md
```

Do not assume no reusable code exists just because the current DT lacks camera nodes.

---

# Phase 7 — Use Khadas Vendor Code as Hardware Documentation

Reference:

```text
khadas/common_drivers
branch: khadas-vims-5.15.y
```

Inspect:

```text
drivers/armisp-g12b/
drivers/armisp-g12b/subdev/sensor/
drivers/armisp-g12b/v4l2_dev/
drivers/media/vin/tvin/csi/
arch/arm64/boot/dts/amlogic/mesong12b.dtsi
arch/arm64/boot/dts/amlogic/kvim3.dts
```

Extract:

- CSI PHY register ranges
- CSI host register ranges
- IRQs
- clocks
- resets
- DMA behaviour
- lane configuration
- packet/error handling
- format handling
- CSI-to-ISP/adapter relationships
- dependencies on vendor-only subsystems

Goal: identify the minimum required path for:

```text
IMX415 → CSI receiver → RAW frame buffer
```

Do not port IQ tables, autofocus, AE/AWB, vendor sensor framework, or unrelated media code unless proven necessary.

---

# Phase 8 — Implement Minimal G12B CSI Receiver

Preferred design:

```text
V4L2
media-controller
v4l2-subdev
standard endpoint graph
vb2 buffers
```

Target pipeline:

```text
IMX415 subdev
    ↓
MIPI CSI-2
    ↓
G12B CSI receiver
    ↓
RAW10 / RAW12
    ↓
V4L2 capture node
```

Initially support only what is required for this board/camera.

A single known-good mode is acceptable.

Do not prematurely generalize.

---

# Phase 9 — Device Tree Graph

Connect sensor and CSI receiver with standard graph endpoints:

```text
IMX415 endpoint
    remote-endpoint
        ↓
G12B CSI endpoint
```

Validate DT where practical:

```bash
make dt_binding_check
make dtbs_check
```

Investigate new DT warnings rather than ignoring them.

---

# Phase 10 — Capture the First RAW Frame

Once the media graph exists:

```bash
media-ctl -p
v4l2-ctl --list-devices
v4l2-ctl -d /dev/videoX --list-formats-ext
```

Capture:

```bash
v4l2-ctl   -d /dev/videoX   --stream-mmap   --stream-count=1   --stream-to=frame.raw
```

Then:

```bash
v4l2-ctl   -d /dev/videoX   --stream-mmap   --stream-count=100   --stream-to=frames.raw
```

Watch:

```bash
dmesg -w
```

for CSI ECC/CRC errors, FIFO overflow, DMA faults, timeouts, or sync loss.

---

# Phase 11 — Validate RAW Image

Determine Bayer ordering and bit depth.

Expected possibilities include:

```text
RGGB / BGGR / GRBG / GBRG
RAW10 / RAW12
```

Convert one captured frame in userspace.

Do not introduce the A311D ISP just to prove the image.

Success criterion:

> A recognizable image can be reconstructed from RAW data captured through the modern V4L2 pipeline.

---

# Phase 12 — Regression Checks

After CSI works, verify:

```bash
dmesg | grep -i etnaviv
ls -l /dev/dri/
v4l2-ctl --list-devices
```

Ensure:

- GC8000/NPU still enumerates
- meson-vdec still works
- Wi-Fi still works
- eMMC still works
- system boots reliably
- no major new kernel warnings

Do not trade NPU support for camera support.

---

# Phase 13 — ISP Is a Separate Task

A311D ISP may later be useful for:

```text
debayer
AE/AWB
noise reduction
color correction
RAW → YUV/RGB
```

But this is out of scope until RAW CSI capture is working.

---

# Development Rules

Keep all work versioned.

Suggested layout:

```text
birdcher/
├── docs/
│   ├── kernel-baseline.md
│   ├── imx415-kernel.md
│   ├── imx415-dt.md
│   ├── g12b-csi-research.md
│   └── csi-bringup-log.md
├── patches/
│   ├── kernel/
│   └── dts/
└── scripts/
    ├── build-kernel.sh
    ├── install-test-kernel.sh
    └── camera-diag.sh
```

Do not make undocumented edits directly in `/boot` as the development workflow.

---

# Safety / Recovery

Before every risky kernel/DT boot:

1. keep previous working kernel
2. keep previous working DTB
3. confirm rollback path
4. do not alter SPI/U-Boot
5. do not erase eMMC
6. keep SD/UART recovery possible

The machine is real hardware and remote access must be preserved.

---

# Immediate Milestones

## Milestone A

Build and boot the same Armbian 6.18 generation with:

```text
CONFIG_VIDEO_IMX415=m
```

Prove:

```bash
modinfo imx415
sudo modprobe imx415
```

works.

## Milestone B

Create a correct upstream-style IMX415 DT node and establish:

```text
clock
reset
pwdn
I2C communication
```

Prove the sensor probes.

## Milestone C

Research whether newer upstream/Armbian/Meson CSI receiver code exists that can be reused.

Only after A+B+C should implementation of a new/minimal G12B CSI driver begin.

---

# Definition of Success

Minimum:

```text
IMX415 probes successfully on Linux 6.18
```

Primary:

```text
IMX415
  ↓
G12B CSI-2
  ↓
V4L2
  ↓
RAW frame captured successfully
```

Best case:

```text
stable continuous RAW capture
+
existing etnaviv/NPU remains functional
+
clean documented patches suitable for future upstream-oriented work
```

Do not proceed to browser streaming, Frigate, or bird detection until the CSI camera pipeline is stable.
