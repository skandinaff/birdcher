# VIM3 IMX415 bring-up — status

**Bottom line: the kernel does not need to be replaced.**  The whole camera
stack builds as external modules against the running Armbian kernel, and the
device tree change rides the supported U-Boot overlay path.  Nothing has been
installed, loaded, or rebooted.

## 1. Was a kernel rebuild necessary?

No.  `CONFIG_VIDEO_IMX415` being unset turned out not to matter — the upstream
IMX415 driver builds cleanly out-of-tree, and every symbol the camera stack
needs is already exported by the running kernel.

The one config gap that *is* real, `CONFIG_OF_OVERLAY` unset, does not force a
rebuild either.  It only rules out the old runtime `dtbo_loader` approach; the
U-Boot `user_overlays` mechanism applies the overlay before the kernel starts
and needs no kernel support at all.

## 2. Modules built

Built against the exact running kernel, `Module.symvers` byte-identical to
`linux-headers-current-meson64_26.8.3_arm64.deb`:

| Module | Source | vermagic |
| --- | --- | --- |
| `imx415.ko` | upstream 6.18 `drivers/media/i2c/imx415.c`, **unmodified** | `6.18.44-current-meson64` |
| `isp_clkc.ko` | Birdcher — VIM3 ISP/CSI clock provider | `6.18.44-current-meson64` |
| `ao_mclk.ko` | Birdcher — 24 MHz INCK on GPIOAO_10 | `6.18.44-current-meson64` |
| `iv009_isp.ko` | Amlogic G12B ISP/CSI, forward-ported to 6.18 | `6.18.44-current-meson64` |

All four: no unresolved symbols, all dependencies (`videodev`, `v4l2-fwnode`,
`v4l2-cci`, `mc`, `v4l2-async`, `videobuf2-*`) already present on the board.

The local `imx415/imx415.c` was checked and is byte-identical (SHA-256
`d0e2d7b7…`) to the Armbian-patched tree's copy, so no Radxa mode tables or
reset hacks leaked in.

`dtbo_loader.ko` is **dropped from the deliverable**.  It compiles, but with
`CONFIG_OF_OVERLAY` unset `<linux/of.h>` substitutes an inline
`of_overlay_fdt_apply()` returning `-ENOTSUPP`, so it could never apply
anything.

## 3. Device tree validation

Performed offline against the **exact running DTB**
(`60eeaaff3b14ee93220a20a4994a4c92804e240db64df7bfd997b047a6f8136d`):

- `fdtoverlay` applies the overlay cleanly — **PASS**.
- Every phandle resolves; no dangling references.
- `reset-gpios` resolves to the `ti,tca6408` at `reg = <0x20>`, pin 3 — matching
  the Khadas vendor `kvim3.dts` exactly.
- CSI graph is bidirectional: `isp_ep` ↔ `imx415_ep`.
- 4 data lanes, `link-frequencies = 720 MHz` → 1440 Mbps/lane, which the
  upstream IMX415 driver supports at 24 MHz INCK.
- The sensor lands on `/soc/bus@ff800000/i2c@5000` (AO I2C), corroborated
  independently by the base DTB and by the vendor tree's `&i2c_AO` camera node.

Decompiled before/after trees are in the bundle under `dt/` for review.

The only dtc complaint is a cosmetic duplicate unit-address between
`isp-adapter@ff650000` and `phy-csi@ff650000`; those regions genuinely overlap
in the vendor design.

## 4. What is still genuinely unknown

Two things cannot be settled offline, and were **not** guessed at:

1. **The sensor's 7-bit I2C address.**  The overlay uses `0x1a`, the IMX415
   default with SLASEL low (`0x1b` when high).  Worth flagging because an
   earlier note in this project claimed the vendor tree proves the address is
   `0x6c` — it does not.  That vendor node is the ARM ISP framework's
   8-bit-addressed proxy for an **os08a10**, an entirely different sensor.
   Resolve with `i2cdetect -y -r 0` once the camera is attached.
2. **PWDN.**  The vendor drives `pwdn = <&gpio_expander 2>`, but the mainline
   IMX415 binding has no `pwdn` property, so the overlay does not model it.  If
   the attached module needs PWDN de-asserted to answer on I2C, that has to be
   done out-of-band before the driver probes.

The `*-supply` nodes are deliberately fixed always-on placeholders representing
"the module regulates itself onboard".  They are not a claim about the board's
regulator topology, and no fake rail was invented to satisfy a binding.

## 5. Deliverable

```
/mnt/shed/khadas/birdcher-build/vim3-armbian-6.18.44-camera-modules-only/
```

SHA256SUMS verified.  `install.sh` is dry-run by default and was exercised
end-to-end in a sandbox: its guards correctly refuse a wrong kernel release, a
wrong package version, a wrong base-DTB hash, a tampered bundle, and any module
name that would shadow an in-tree Armbian module.

## 6. Proposed first boot (not yet executed — needs your go-ahead)

Nothing below overwrites an Armbian-owned file.  It adds two new directories
and one line to `armbianEnv.txt`.

```sh
# on the VIM3, with the camera attached and UART console connected
cd ~/birdcher-deploy/vim3-armbian-6.18.44-camera-modules-only
sudo ./install.sh              # dry-run, review the output
sudo ./install.sh --install    # applies; loads nothing, reboots nothing
sudo reboot
```

After boot, before loading anything:

```sh
sudo i2cdetect -y -r 0         # confirm the sensor address is really 0x1a
```

Then load in dependency order and watch `dmesg`:

```sh
sudo modprobe isp_clkc && sudo modprobe ao_mclk
sudo modprobe imx415 && sudo modprobe iv009_isp
media-ctl -p ; v4l2-ctl --list-devices
```

If `i2cdetect` shows the sensor at `0x1b` instead, change `reg` and the node
name in `overlays/vim3-camera-overlay.dts`, rebuild the `.dtbo`, and reinstall —
no module needs rebuilding for that.

## 7. Rollback

```sh
sudo ./rollback.sh --uninstall    # dry-run without the flag
sudo reboot
```

If the board will not boot: U-Boot already restores the original DT by itself
when an overlay fails to apply, so it should come up on the stock kernel
regardless.  Failing that, mount the boot partition elsewhere and delete the
`user_overlays=` line.  The Armbian kernel, DTB, initramfs and module tree are
never touched by any of this.
