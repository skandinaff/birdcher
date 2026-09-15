# Camera controls

What the camera actually exposes, and which knobs are real. This is the source
material for the web UI's Camera screen (architecture §23) and for
`vim3-camera-controls` (§5.2).

Enumerate them as JSON with
`platform/camera/tools/vim3-camera-controls.sh`, which merges both surfaces and
derives the current frame rate.

## Two control surfaces

| Node | What it is | Count |
| --- | --- | --- |
| `/dev/video1` | the ARM ISP — standard V4L2 controls plus 25 `isp_*` vendor extensions | 42 |
| `/dev/v4l-subdev0` | the imx415 sensor — blanking, analogue gain, test pattern | 9 |

## Verified working

Every control below was set and read back, and the ISP path was confirmed to
reach pixels: brightness 16 / 128 / 240 produced frame Y means of
**0.3 / 42.7 / 209.4**. So these are live hardware, not stubs.

| Control | Range | Default | For the UI |
| --- | --- | --- | --- |
| `brightness` | 0-255 | 128 | slider |
| `contrast` | 0-255 | 128 | slider |
| `saturation` | 0-255 | 128 | slider |
| `hue` | 0-255 | 128 | slider |
| `sharpness` | 0-255 | 128 | slider |
| `color_effects` | menu | None | None / B&W / Sepia / Negative |
| `horizontal_flip`, `vertical_flip` | bool | 0 | toggles |
| `gain_automatic` | bool | 1 | AGC on/off |
| `gain` | 100-3200 | 100 | slider, meaningful only with AGC off |
| `auto_exposure` | menu | Auto | Auto / Manual |
| `exposure_time_absolute` | 1-1000 | 33 | slider, manual mode |
| `white_balance_automatic` | bool | 1 | AWB on/off |
| `white_balance_temperature` | 2000-8000 K, step 1000 | 5000 | slider |
| `awb_red_gain_set`, `awb_blue_gain_set` | -1..65535 | -1 (auto) | manual WB gains; -1 returns control to AWB |
| `focus_absolute` | 0-255 | 0 | slider (DW9714 VCM) |
| `focus_automatic_continuous` | bool | 1 | AF on/off |
| `isp_af_refocus` | button | — | "refocus now" |
| `isp_ae_compensation` | 0-255 | 0 | EV compensation slider |
| `sensor_integration_timet_set` | -1..4000 | -1 | manual integration time |
| `sensor_analog_gain_set`, `sensor_digital_gain_set`, `isp_digital_gain_set` | -1..256 | -1 | manual gain stages |
| `max_int_time_set` | -1..5564 | -1 | caps AE's exposure, i.e. motion blur vs noise |
| `isp_test_pattern`, `isp_sensor_test_pattern` | 0-1 / 0-10 | 0 | diagnostics |
| `sensor_ir_cut_set` | -1..2 | -1 | IR-cut filter, if wired on this module |
| `set_isp_ae_zone_weight`, `set_isp_awb_zone_weight` | int64 | 0 | zone weighting — pairs naturally with the UI's detection zones |

Sensor subdev: `vertical_blanking` (58-1046383), `horizontal_blanking`,
`analogue_gain` (0-100), `test_pattern`, plus read-only `link_frequency` and
`pixel_rate`.

## Frame rate: use the sensor, not the ISP

**`vertical_blanking` on the sensor is the real rate control.** It sets the
frame period, so the sensor reads out less and the ISP processes less:

```
total_lines       = 135000 / fps        # 135000 lines/s at the 3864x2192 preset
vertical_blanking = total_lines - 2192

  60 fps ->   58     30 fps -> 2308     15 fps -> 6808
```

Measured: 2308 gives 29.81 fps, 6808 gives 14.88 fps, both with zero drops, and
the setting survives stream start — the ISP does not override it.

**`isp_ds1_fps` is not the same thing and currently does nothing.** It is
implemented as `fw_intf_set_ds1_fps()` -> `acamera_api_set_fps()` ->
`dma_writer_pipe_set_fps()`, i.e. it drops frames in the DMA writer *after* the
ISP has already processed them — so it costs the same power and heat. In
testing it also read back 0 after being set and had no effect on the delivered
rate. Do not offer it as the frame-rate control.

Rate matters thermally. At 60 fps the hottest SoC zone peaked at 65.6 C; at
15 fps, 50.6 C, for identical delivered output.

## Caveats for whoever builds the UI

- **These are runtime settings and do not persist.** A module reload or reboot
  returns everything to defaults, `vertical_blanking` included. Anything the UI
  changes has to be re-applied on start.
- **AE and AWB are open loops.** The ISP's 3A expects a userspace algorithm
  daemon over sbuf that does not exist here, so `white_balance_automatic=1`
  does not converge and the image keeps its green cast. The manual gain
  controls do work, which makes them the only way to correct colour today.
  Per architecture §5.3, understand the Khadas 3A path before treating manual
  gains as the fix — but they are legitimate as a UI feature.
- **One owner for the camera.** Controls are per-device and safe to change from
  another process, but *capture* is not: stream ids come from open order, so
  only one process may stream. See `platform/README.md`.
- `isp_sensor_preset` (0-5) exists, but the mainline imx415 driver has a single
  mode, so 3864x2192 is the only usable preset.
