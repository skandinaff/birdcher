# VIM3 IMX415 bring-up — status

**The camera works.** Sensor → CSI-2 → ISP → V4L2 → real captured frames, on the
stock Armbian 26.8.3 kernel `6.18.44-current-meson64`, with no kernel rebuild.
The whole stack is external modules plus one boot-time DT overlay.

## 1. Was a kernel rebuild necessary?

No. `CONFIG_VIDEO_IMX415` being unset did not matter — the upstream driver
builds out-of-tree and every symbol the stack needs is already exported.
`CONFIG_OF_OVERLAY` being unset also did not force a rebuild; it only rules out
the old runtime `dtbo_loader` approach, and U-Boot's `user_overlays` applies the
overlay before the kernel starts.

## 2. Modules

The stack is exactly three modules, built against the running kernel with
`Module.symvers` byte-identical to
`linux-headers-current-meson64_26.8.3_arm64.deb`, all vermagic
`6.18.44-current-meson64`:

| Module | Role |
| --- | --- |
| `isp_clkc.ko` | ISP/CSI clocks, plus the sensor MCLK (`gen_clk`) and its output pad |
| `imx415.ko` | upstream 6.18 sensor driver, **unmodified** |
| `iv009_isp.ko` | Amlogic G12B ISP + CSI-2 receiver, forward-ported to 6.18 |

Two modules were **removed** after first frame, along with the scaffolding
that propped them up:

- `ao_mclk.ko` programmed `HHI_GEN_CLK_CNTL` directly — the same register
  `isp_clkc` already owns as a proper CCF clock. Two owners of one clock, and
  the two disagreed: `ao_mclk` had the gate at bit 7 (the gxbb/axg layout),
  `isp_clkc` had the correct G12A bit 11. The sensor now consumes `gen_clk`
  through DT like any other clock consumer.
- `dtbo_loader.ko` could never work: with `CONFIG_OF_OVERLAY` unset,
  `of_overlay_fdt_apply()` is an inline stub returning `-ENOTSUPP`. The
  runtime-overlay scripts built on it (`load.sh`, `unload.sh`) went with it;
  U-Boot applies the overlay at boot instead.
- `/etc/modprobe.d/birdcher-camera.conf` (`softdep imx415 pre: ao_mclk`) is
  gone. Ordering is now a real dependency: the sensor's `clocks = <&isp_clkc 6>`
  makes `imx415` defer until the clock provider exists.
- `scripts/find-mclk-mux.sh` was a brute-force search for the MCLK mux value.
  The schematic answered that question, so the search is deleted.

### gen_clk ownership, after cleanup

```
isp_clkc  ── gen_clk_sel (xtal) ─→ gen_clk_div (/1) ─→ gen_clk (gate)
              │                                            │
              └─ routes GPIOAO_11 to mux 4 (GEN_CLK_EE)     └─ clocks = <&isp_clkc 6>
                 per birdcher,gen-clk-pad = <11 4>             consumed by imx415 as "inck"
```

Two things had to be handled for this to work, both worth knowing:

1. **The bootloader leaves `gen_clk_sel` holding 16**, which is not in the
   driver's parent table `{0,5,6,7,20,...}`. `isp_clkc_mux_get_parent()`
   reports index 0 ("xtal") for any unrecognised value, so the clock framework
   claimed 24 MHz while the hardware drove nothing — `clk_get_rate()` looked
   right and the sensor NAKed every transfer. `isp_clkc` now writes the xtal
   selection explicitly. This *cannot* be done with `assigned-clock-parents`:
   the only output exposed to DT is the final gate, whose sole parent is
   `gen_clk_div`, so reparenting it to `&xtal` is rejected with `-EINVAL`
   (`clk: failed to reparent gen_clk to xtal: -22`).
2. **`isp_clkc` holds `gen_clk` enabled while it is routed to a pad.** Mainline
   `imx415` waits ~100 us after `clk_prepare_enable()` before its first register
   write, because it assumes INCK is a free-running crystal; Amlogic's own
   IMX415 driver waits 30 ms precisely because it gates this clock. A cold
   start inside the sensor's power-on loses that race. A pad muxed to a clock
   output whose clock is gated off is just a dead pin anyway.

## 3. The four bugs that were blocking it

All four were found by checking board sources against the hardware, not by
guesswork. Each is worth recording because each was silently wrong.

### 3.1 MCLK was on the wrong pad, from the wrong clock (`ao_mclk`)

The module drove **GPIOAO_10 / CLK12_24** — the *Radxa Zero 2 Pro* routing,
carried over unverified. `docs/VIM3_CAMERA_HW_FACTS.md` already listed exactly
that value under "Radxa values that must NOT be copied to VIM3".

`docs/datasheets/vim3-sch-v15.pdf` settles it:

```
SPDIF_OUT   GPIOAO_10(AO_CEC_A//...//CLK12_24)
CAM_MCLK
BF16
GPIOAO_11(PWMAO_A_HIZ//PWMAO_A//GEN_CLK_EE//GEN_CLK_AO)
```

**CAM_MCLK is GPIOAO_11 (ball BF16), function `GEN_CLK_EE` = mux 4.** GPIOAO_10
carries SPDIF_OUT on this board. The original comment had taken the ball
designator `BF16` from the GPIOAO_11 row and paired it with GPIOAO_10's function
list. Confirmed independently by the vendor pinctrl driver:
`gen_clk_ee_ao_pins[] = { GPIOAO_11 }; GROUP(gen_clk_ee_ao, 4)`.

### 3.2 `gen_clk` gate bit is different on G12A/G12B

`HHI_GEN_CLK_CNTL` (0xff63c228) is **not** laid out the same across the family.
gxbb/axg gate at bit 7; G12A/G12B gate at **bit 11** with a 5-bit parent select.
Writing bit 7 on G12B lands *inside the divider field* (÷129) and never opens the
gate — a dead pin. Per the vendor G12A driver:

```c
g12a_gen_mux: .mask = 0x1f, .shift = 12   /* sel  [16:12], 0 = xtal */
g12a_gen_div: .shift = 0, .width = 11     /* div  [10:0], ÷(n+1)   */
g12a_gen:     .bit_idx = 11               /* gate  bit 11          */
```

Fixing this is what first made the sensor ACK on I2C.

### 3.3 Reset was the wrong expander line and the wrong polarity

`vim3-sch-v15.pdf` gives the TCA6408 (U17, 0x20) mapping directly:

| Pin | Net |  | Pin | Net |
| --- | --- | --- | --- | --- |
| P2 | **CAM_RESET** | | P4 | CAM_PDN1 |
| P3 | CAM_PDN0 | | P5 | RED_LED |

P5=RED_LED matches the live `consumer=red:status` on line 5, which confirms the
numbering. The overlay used **line 3** (CAM_PDN0) with `GPIO_ACTIVE_HIGH`.

Note the vendor DTS labels are swapped relative to the board nets — it calls
line 3 "reset" and line 2 "pwdn" — but its own IMX415 driver overrides that:

```c
/* IMX415 PIN21 RESET, defined in dts as 'ircut-gpios' */
gpio->rst_gpio = gpio->ircut_gpio;     /* = expander line 2 */
gpiod_set_value_cansleep(gpio->rst_gpio, 1);   /* 1 = run */
```

IMX415 pin 21 is XCLR, active-low. Mainline requests the line `GPIOD_OUT_HIGH`
and calls `gpiod_set_value(reset, 0)` to release, so logical-1 must map to
physical LOW. Correct DT is `<&gpio_expander 2 GPIO_ACTIVE_LOW>`.

### 3.4 `isp_clkc` collided with a clock mainline has since added

It registered a gate named `mipi_isp`. Linux 6.18's `g12a.c:3947` now registers
an unrelated clock of that name, so `clk_hw_register()` returned `-EEXIST` and
took the *whole provider* down — which left `ff140000.isp` deferred forever.
Renamed to `mipi_isp_pclk`. The module's comment claiming these clocks "do not
exist anywhere in this kernel" was stale; only that one name actually clashes,
verified against `/sys/kernel/debug/clk` on the running board.

## 4. The V4L2 port bug — systemic, not a one-off

`VIDIOC_G_SELECTION` and `VIDIOC_STREAMON` each oopsed with a NULL deref.
The cause is the same for both, and it is general:

```c
/* drivers/media/v4l2-core/v4l2-ioctl.c */
ret = ops->vidioc_g_selection(file, NULL, p);
ret = ops->vidioc_streamon(file, NULL, ...);
```

**99 call sites** in 6.18's `v4l2-ioctl.c` pass a literal `NULL` for the `fh`
argument. It is effectively deprecated: the handle must come from
`file->private_data`. NULL is not a race and not an error state — it is
guaranteed, every call. A `if (!fh) return -ENODEV;` guard therefore *masks* the
bug rather than fixing it; the handler simply never works.

Most of this driver already read `file->private_data`. Four handlers trusted the
argument and were converted: `isp_v4l2_g_selection`, `isp_v4l2_s_selection`,
`isp_v4l2_streamon`, `isp_v4l2_streamoff`, plus `isp_v4l2_g_pixelaspect`. A
helper `isp_v4l2_fh_of(file)` now centralises it so this cannot recur.
`isp_v4l2_fop_close()` also gained a NULL guard — dereferencing there is what
left `v4l2-ctl` stuck in D state after each oops.

Each fix was verified in the disassembly, not just the source, e.g.:

```
9394:  ldr x20, [x19, #24]   ; file->private_data
939c:  cmp x20, #0
93a4:  b.eq → error          ; guard precedes the deref
93a8:  ldr w6, [x20, #144]   ; sp->stream_id
```

## 5. Verified working

Clean boot, everything automatic, zero oopses:

```
isp_clkc ...: gen_clk routed to GPIOAO_11 (mux 4 -> 4), 24000000 Hz
isp_clkc ...: registered 7 aux ISP/CSI clocks via shared HHI syscon regmap
imx415 0-001a: Detected IMX415 image sensor
Matched subdev 'imx415 0-001a' for prefix 'imx415'
IMX415 bridge: 3864x2192 total 4510x2250, lines/s 135000
AM_MIPI: am_mipi_csi_init:csi version 0x3130322a
MIPI/adapter up: 4 lanes, ui 1, 3864x2192 RAW10, DIR_MODE
```

`/dev/video1` ("juno R2") enumerates 8 formats (RGB4, RGB3, NV12, Y444, YUYV,
UYVY, GREY, BYR2). Sensor subdev reports 3864x2192 `MEDIA_BUS_FMT_SGBRG10_1X10`.
Capture at native resolution produces real, in-focus images.

## 6. Known remaining issues

1. **Downscaled capture is broken.** Requesting 1920x1080 negotiates correctly
   (stride 1920, 3110400 B) but the ISP's DMA writer stays programmed for full
   sensor resolution and refuses to write:
   ```
   TRACE dma_writer: active 3864x2192 line_offset=3968 frame_size=8697856 buf_size=2076672
   DMA_WRITER: frame_size greater than available buffer. fr 8697856 vs 2076672
   ```
   Buffers come back unfilled, so the output is garbage. **Capture at native
   3864x2192 (stride 3968) works.** The scaler configuration is not being
   propagated from `S_FMT` into the ISP pipeline — next thing to fix.

2. **White balance / colour matrix untuned.** Output has a heavy green cast.
   Part of this is likely not "untuned" but *mis-tuned*: the ISP calibration
   data shipped in `isp-module/src/calibration/` is explicitly
   `acamera_calibrations_{static,dynamic}_linear_imx415.c` — "IMX415 ...
   calibration set **for the Radxa Camera 4K on a Radxa Zero 2 Pro**". It is a
   different camera module than the one on this board, and it assumes a fixed
   lens with no focus motor ("The Radxa Camera 4K has a fixed lens and no
   focus"), whereas the attached VIM3 module answers at 0x0c with a DW9714 VCM
   — i.e. it has autofocus. Before hand-tuning AWB/CCM, check whether these
   tables are appropriate at all; they also feed AE, so issue 3 may share this
   root cause.

3. **Auto-exposure fights you.** The ISP's AE overrides sensor exposure/gain
   within a frame or two and drives the scene to near-black (exposure written
   back as 244 of 2242). Early frames are correctly exposed, later ones are not.
   AE statistics look mis-scaled — likely related to issue 1.

4. **Occasional unfilled buffer** (an all-zero frame) during capture.

5. `VIDIOC_CREATE_BUFS` is not implemented — harmless, `v4l2-ctl` just probes it.

## 7. Reproducing a capture

```sh
v4l2-ctl -d /dev/video1 \
  --set-fmt-video=width=3864,height=2192,pixelformat=NV12 \
  --stream-mmap --stream-count=6 --stream-to=/tmp/cap.raw

# frames are NV12, stride 3968, 13046784 bytes each
ffmpeg -f rawvideo -pix_fmt nv12 -s 3968x2192 -i /tmp/cap.raw \
       -vf "crop=3864:2192:0:0" -frames:v 1 out.png
```

## 8. Deployment

```
/mnt/shed/khadas/birdcher-build/vim3-armbian-6.18.44-camera-modules-only/
```

Installed and running on the board. `install.sh` is dry-run by default and
verifies kernel release, package version, base-DTB hash, bundle checksums and
module vermagic before touching anything; it only adds files. Rollback:
`sudo ./rollback.sh --uninstall`. The Armbian kernel, DTB, initramfs and module
tree were never modified — confirmed by SHA-256 after install.
