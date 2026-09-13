# Phase 0 — Collect Baseline Information

## Objective

Capture a complete, read-only record of the VIM3 and its camera-related state before installing packages or changing configuration.

## Guardrails

- [ ] Do not change the kernel, bootloader, SPI flash, eMMC boot configuration, Device Tree, or overlays.
- [ ] Record command failures and absent devices exactly as observed.
- [ ] Keep all evidence in `docs/hardware-baseline.md`.

## Tasks

- [ ] Record OS and kernel details:

  ```bash
  uname -a
  cat /etc/os-release
  cat /etc/armbian-release 2>/dev/null || true
  ```

- [ ] Record storage layout and free space:

  ```bash
  lsblk -o NAME,SIZE,FSTYPE,MOUNTPOINTS
  df -h
  ```

- [ ] Record network interfaces and routes:

  ```bash
  ip addr
  ip route
  ```

- [ ] Record discovered video, media, and DRM device nodes:

  ```bash
  ls -l /dev/video* 2>/dev/null || true
  ls -l /dev/media* 2>/dev/null || true
  ls -l /dev/dri/ 2>/dev/null || true
  ```

- [ ] Record relevant boot-time driver messages:

  ```bash
  dmesg | grep -Ei 'camera|csi|mipi|isp|sensor|video|v4l2|media|etnaviv|npu|galcore'
  ```

- [ ] Refresh APT metadata, then record the available camera-tool package versions:

  ```bash
  sudo apt update
  apt policy v4l-utils ffmpeg gstreamer1.0-tools
  ```

- [x] Write all outputs, timestamps, command failures, and initial conclusions to `docs/hardware-baseline.md`.

## Exit criteria

- [x] `docs/hardware-baseline.md` contains the complete command output or a clearly marked failure for every command above.
- [x] Existing camera/media/NPU device nodes and relevant kernel messages are identified.
- [x] No destructive boot, kernel, or Device Tree change was made.

## Execution status — 2026-09-13

- [x] Baseline system, storage, network, device-node, kernel-message, and package-policy evidence saved in `docs/hardware-baseline.md`.
- [x] No destructive bootloader, SPI, eMMC, kernel, or Device Tree changes made.
- [x] `sudo apt update` was initially blocked by interactive sudo authentication, then completed by the system owner.
- [x] The refreshed package policy is appended to `docs/hardware-baseline.md`; Phase 0 is complete.
