# Handover: camera + live preview, end of 2026-09-15

Read [../STATUS.md](../STATUS.md) first; this page contains only what a next
session needs that is not obvious from the code. It includes the indoor image
tuning completed after the original streaming handover.

## Where the project is

| Milestone | State |
| --- | --- |
| M0 camera transport | **done** — DS1 1920x1080 NV12, cold boot, repeatable |
| M1 live observability | **done** — DS1 -> MJPEG -> HTTP, viewable in a browser, soak-measured |
| M2 NPU proof | **not started** — next: MobileNet V2 / SSDLite MobileDet UINT8 via Teflon/etnaviv, with the CPU-vs-NPU comparison |

## Running it

```sh
ssh 192.168.1.38
sudo ~/birdcher-tools/preview-ctl.sh start|stop|restart|status
# -> http://192.168.1.38:8090/
```

`start` restores the measured 30 fps indoor profile: 10 ms exposure and 18 dB
analogue gain. See [../camera/image-tuning.md](../camera/image-tuning.md).

Tools live in `platform/camera/tools/`, built on the board in
`~/birdcher-tools/` with `gcc -O2 -o X X.c`. Board tools are copies; edit in
the repo and scp.

## Four things that will bite

1. **Stream ids come from open order** on `/dev/video1` (0=FR, 1=META, 2=DS1),
   and the stream object dies on close. One `open()` lands on FR at 3864x2192 —
   which is why `ffmpeg -i /dev/video1` does not give you DS1. Only one process
   may stream; a leftover capture makes the next one silently pick up the wrong
   streams. This cost an hour of misdiagnosis that looked like an ffmpeg bug.
2. **Runtime controls do not persist.** A module reload or reboot resets the
   sensor to 60 fps and every ISP control to default. `preview-ctl.sh start`
   re-applies the rate, exposure and gain; any other camera owner must too.
3. **The WiFi link drops constantly** — roughly every other ssh command returned
   nothing during this work. Long-running work goes under `systemd-run`, and
   results get written to a file, not a terminal.
4. **`isp_ds1_fps` is not the frame-rate control.** It decimates in the DMA
   writer after the ISP has done the work, and did nothing when set. The
   sensor's `vertical_blanking` is the real one: `135000/fps - 2192`
   (2308 = 30 fps). Worth 15 C between 60 and 15 fps.

## Measured

- 30 fps: 29.81 fps, 0 drops. 60 fps: 65.6 C peak; 15 fps: 50.6 C peak.
- MJPEG 1080p q7: **4.7-5.9 Mbit/s**, ~45 kB/frame.
- Longest run 642 s: no memory leak (Slab +-0.2 MB), no Oops/WARN while streaming.
- 51 camera controls, all set/read-back verified, ISP path confirmed to reach
  pixels (brightness 16/128/240 -> Y mean 0.3/42.7/209.4). See
  [../camera/controls.md](../camera/controls.md).
- Indoor profile after photographic gamma + 50 Hz correction: whole-frame Y
  mean 78.8, far half 19.9, near half 137.8, with 0.505% above Y=250. Preview
  read-back: exposure 1350 lines, sensor gain 60 (18 dB).

## Open

- **Image colour / adaptive 3A.** Static indoor brightness is fixed and usable.
  A green cast remains because AE/AWB are open loops and WB/CCM are neutral.
  Per architecture §5.3, find the Khadas 3A path before hand-tuning colour.
  Manual AWB gains do work and are the only colour correction available today.
- **Preview serves one client** (`ffmpeg -listen 1`); it restarts per client so
  a refresh reconnects, but two viewers cannot watch at once. Fan-out is a
  later measurement-driven transport decision.
- **Module reload leaks sysfs attrs** (`adapt_frame`/`inject_frame`/`dol_frame`),
  so a reload throws three duplicate-filename WARNs. Cold boot is clean.
- **Long-run stability** measured to ~11 min, not the roadmap's 30.
- **A1019 module temperature** — unknown whether one is readable (architecture
  §5.4); motivated by a 50 C thermal-camera reading on the module.

## Standing rule

`AGENTS.md`: diff against Khadas `external/khadas-common_drivers` (pinned
`3a11a86`) before inventing a fix. It has settled every hard bug so far.
