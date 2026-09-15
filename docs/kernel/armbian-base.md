# Running Armbian kernel baseline

This document records the kernel ABI that Birdcher must target.  Collection
was read-only; no boot files, loaded modules, or module trees were changed.

## Running VIM3

| Item | Value |
| --- | --- |
| Board | Khadas VIM3 (`meson-g12b`, A311D/G12B) |
| Armbian release | 26.8.3, `current` |
| Kernel release | `6.18.44-current-meson64` |
| `uname -a` build line | `#1 SMP PREEMPT Sun Aug 9 18:25:30 UTC 2026` |
| Linux source revision | `1efe5d048a391de3ead2804b2e7f86376c356cc5` |
| Image/DTB packages | `linux-image-current-meson64=26.8.3`, `linux-dtb-current-meson64=26.8.3` |
| Headers package | not installed; exact candidate is `linux-headers-current-meson64=26.8.3` |
| Board DTB | `amlogic/meson-g12b-a311d-khadas-vim3.dtb` |

The installed image package identifies its build as drivers hash
`539705d8_ceaab8e6`, patches hash `3034a00a66799285`, config hash
`0dacf821bddd39b7`, config-hook hash `1c3a9337c4583b24`, variables hash
`e3771f6e9d9ac20167c59982d4ccc58a8e68b59ea73d40feb1c1c8172d251477`,
and framework bash hash `4990fb4ba983d439`.

`/etc/armbian-release` records framework commit `fd4ebfd1e` and an internal
build URL.  The older `/etc/armbian-image-release` describes the original
26.8.1 image and must not be used as the current-kernel provenance.

The recorded framework commit was not fetchable from public
`https://github.com/armbian/build`, and the recorded internal URL did not
answer a read-only `git ls-remote` query.

**This gap has since been closed by content, not by commit id.**  Driving the
closest public revision (`ea18947b`) through the Armbian framework reproduced
every hash the installed package records — drivers `539705d8_ceaab8e6`,
patches `3034a00a66799285`, config `0dacf821bddd39b7`, config-hook
`1c3a9337c4583b24` — and yielded Linux revision
`1efe5d048a391de3ead2804b2e7f86376c356cc5`.  Identical content is stronger
evidence than an identical commit id would have been.  See
[`build-provenance.md`](build-provenance.md).

## Boot path and immutable baseline

The installed U-Boot script loads fixed `/boot/Image` and `/boot/uInitrd`.
There is no `/boot/extlinux/extlinux.conf`.  `/boot/Image` points at
`vmlinuz-6.18.44-current-meson64`; `/boot/uInitrd` points at
`uInitrd-6.18.44-current-meson64`; `/boot/dtb` points at
`dtb-6.18.44-current-meson64`.

`/boot/armbianEnv.txt` selects
`amlogic/meson-g12b-a311d-khadas-vim3.dtb`.  The existing release must remain
untouched; a future Birdcher kernel needs a separate boot selection mechanism,
not replacement of these symlinks.

SHA-256 at collection time:

| Artifact | SHA-256 |
| --- | --- |
| `/boot/config-6.18.44-current-meson64` | `a63c70d02f0c12e3558f493c200b5cf4ebd73da2ea48164b73f60e0dc2e72e68` |
| `/boot/vmlinuz-6.18.44-current-meson64` | `22ddd70dc5e0f5838414ec0341a277b585a10a1367aa9d0aaab3423efd449301` |
| `/boot/initrd.img-6.18.44-current-meson64` | `4531bd28c3b39e8f49749f0d180e3eddb715017f413d86c5040945614d77729d` |
| `/boot/uInitrd-6.18.44-current-meson64` | `1c945157d5e7514ef2af991f87f9f2b189f8e1aa89c1bd544a621e8d57fdaf25` |
| VIM3 DTB | `60eeaaff3b14ee93220a20a4994a4c92804e240db64df7bfd997b047a6f8136d` |

## Module-only assessment prerequisites

The running configuration supplies the main media dependencies:

- `CONFIG_MEDIA_SUPPORT=m`, `CONFIG_VIDEO_DEV=m`, `CONFIG_MEDIA_CONTROLLER=y`;
- `CONFIG_VIDEO_V4L2_SUBDEV_API=y`, `CONFIG_V4L2_FWNODE=m`;
- `CONFIG_VIDEOBUF2_V4L2=m`, `CONFIG_VIDEOBUF2_MEMOPS=m`,
  `CONFIG_VIDEOBUF2_VMALLOC=m`;
- `CONFIG_I2C=y`, `CONFIG_I2C_MESON=y`, and fixed-voltage regulator support.

`CONFIG_VIDEO_IMX415` is not set, which is not by itself evidence that a
kernel rebuild is required: the upstream driver can be tested as an external
module.

`CONFIG_OF_OVERLAY` is not set.  The prior runtime `dtbo_loader` approach
therefore cannot be used with this kernel.  The existing U-Boot script does
support Armbian `user_overlays`; a boot-time DTBO is the candidate modules-only
mechanism and must be validated offline against this exact DTB.

The exact headers `.deb` was downloaded but not installed:

`linux-headers-current-meson64_26.8.3_arm64.deb`

SHA-256: `8272b006f1e365a60905648fa25d00e5cbb7c0803b7e95cef75acd4742bab8f2`.
It contains the matching `Module.symvers`, but is not a complete standalone
cross-build output tree: it lacks prebuilt host Kbuild helpers and source files
needed by `modules_prepare` (its `fixdep` is an ARM64 binary).  That was worked
around by running `modules_prepare` against the Armbian-prepared source tree and
then restoring this package's authoritative `Module.symvers`.

## Status — resolved

All four external camera modules build and modpost cleanly against a
provenance-matched Armbian output tree, with vermagic
`6.18.44-current-meson64` and a `Module.symvers` byte-identical to the headers
package.  **Kernel replacement is not required.**

The `CONFIG_OF_OVERLAY` finding above stands and settles the DT mechanism: the
runtime `dtbo_loader` approach is dead on this kernel, and the supported U-Boot
`user_overlays` path is used instead.  That path needs no kernel change either,
so it does not reopen the rebuild question.  The overlay has been proven to
apply offline against the exact running DTB.

The prior vanilla-kernel replacement bundle remains reference-only and must not
be installed.  The deliverable is
`/mnt/shed/khadas/birdcher-build/vim3-armbian-6.18.44-camera-modules-only/`.
