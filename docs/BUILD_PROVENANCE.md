# Birdcher VIM3 IMX415 — build provenance

**Outcome: a kernel rebuild is NOT required.**  The entire camera stack builds
as external modules against the running Armbian kernel.  Nothing in `/boot` or
`/lib/modules` was modified, and no module was loaded.

## Running target

| Item | Value |
| --- | --- |
| Board | Khadas VIM3, Amlogic A311D / G12B (`meson-g12b`) |
| Armbian | 26.8.3, branch `current` |
| Kernel release | `6.18.44-current-meson64` |
| `linux-image-current-meson64` | `26.8.3` |
| `linux-dtb-current-meson64` | `26.8.3` |
| Board DTB | `amlogic/meson-g12b-a311d-khadas-vim3.dtb` |
| DTB SHA-256 | `60eeaaff3b14ee93220a20a4994a4c92804e240db64df7bfd997b047a6f8136d` |
| Running config SHA-256 | `a63c70d02f0c12e3558f493c200b5cf4ebd73da2ea48164b73f60e0dc2e72e68` |

## Source provenance — exactly reproduced

`/etc/armbian-release` names framework commit `fd4ebfd1e` and an internal build
URL; neither is fetchable from outside Armbian's infrastructure.  Rather than
guess, the closest public revision was driven through the Armbian framework and
the framework's **own content hashes were used to confirm identity**.  Every
identifier the installed package records was reproduced bit-for-bit:

| Identifier | Installed package | Reproduced |
| --- | --- | --- |
| Linux source revision | `1efe5d048a391de3ead2804b2e7f86376c356cc5` | same |
| drivers hash | `539705d8_ceaab8e6` | same |
| patches hash | `3034a00a66799285` | same |
| config hash | `0dacf821bddd39b7` | same |
| config-hook hash | `1c3a9337c4583b24` | same |

This is stronger evidence than a matching commit id would have been: it shows
the produced source *content* is identical, not merely that the inputs looked
similar.  The provenance gap recorded earlier in
[`armbian-kernel-base.md`](armbian-kernel-base.md) is therefore **closed**.

| Item | Value |
| --- | --- |
| Armbian build framework | `ea18947bed789c829a260df129e815c353c14908` (public `armbian/build`) |
| Linux base tag | `v6.18.44` (`git.kernel.org` stable) |
| Linux revision after Armbian patching | `1efe5d048a391de3ead2804b2e7f86376c356cc5` |
| Armbian patch directory | `patch/kernel/archive/meson64-6.18` |
| Headers package | `linux-headers-current-meson64_26.8.3_arm64.deb` |
| Headers package SHA-256 | `8272b006f1e365a60905648fa25d00e5cbb7c0803b7e95cef75acd4742bab8f2` |
| `Module.symvers` SHA-256 | `723345f99d9e36a0d4162d49af5686f02e6732366d6a3c130bec1f2f5711f4be` |
| Birdcher camera tree | `9299cf2156efbb3cc4ae4a5b7853deef7199542c` |
| Khadas hardware reference (read-only) | `khadas/common_drivers` `khadas-vims-5.15.y` @ `3a11a86a02e759fc57fc79410215f7c0c3a0d8e0` |
| Cross compiler | `aarch64-linux-gnu-gcc-14` (Ubuntu 14.2.0-4ubuntu2~24.04.1) 14.2.0 |
| Binutils | system `aarch64-linux-gnu-*` 2.42 |

### Deviations from the installed build, and why they are safe

1. **`EXTRAWIFI=no`.**  Armbian's dynamically generated third-party Wi-Fi patch
   was malformed in this framework revision (it tried to touch the whole tree)
   and the patcher correctly refused it.  Those drivers are unrelated to CSI/ISP.
   Effect is confined to `CONFIG_RTL8*` symbols.
2. **Build-host toolchain differs** from Armbian's Debian GCC 14.2.0-19.  This
   only moves `CONFIG_*_VERSION` style symbols.

Neither deviation touches module ABI: `Module.symvers` is byte-identical to the
shipped headers package, and every module reports the correct vermagic.  The
complete diff is 20 lines and is reproduced in
[`kernel-config-diff.txt`](kernel-config-diff.txt).

## Kernel config

The running `/boot/config-6.18.44-current-meson64` was used verbatim as the
baseline; `olddefconfig` was run but **no camera option was changed** and
`CONFIG_VIDEO_IMX415` was deliberately left unset — the driver is supplied as an
external module instead.  Nothing was taken from `defconfig` or from
`linux-meson64-current.config`.

## Reproducing this build

```sh
# 1. Prepare the Armbian-patched source (no kernel is compiled: PATCH_ONLY=yes)
cd /mnt/shed/khadas/birdcher-build/armbian-build
git checkout --detach ea18947bed789c829a260df129e815c353c14908
cp <birdcher>/tools/armbian-safe-git-container.sh userpatches/extensions/
./compile.sh kernel BOARD=khadas-vim3 BRANCH=current \
    KERNELBRANCH=tag:v6.18.44 EXTRAWIFI=no PATCH_ONLY=yes ARTIFACT_IGNORE_CACHE=yes

# 2. Build an external-module output tree carrying the *running* config
KSRC=/mnt/shed/khadas/birdcher-build/armbian-build/cache/sources/linux-kernel-worktree/6.18__meson64__arm64
OUT=/mnt/shed/khadas/birdcher-build/armbian-vim3-camera/phase1-running-kernel
GCC14=/mnt/shed/khadas/birdcher-build/toolchains/gcc14-arm64/root/usr/bin/aarch64-linux-gnu-gcc-14
BDIR=/mnt/shed/khadas/birdcher-build/toolchains/gcc14-arm64/target-binutils
install -D /boot/config-6.18.44-current-meson64 "$OUT/.config"   # from the VIM3
make -C "$KSRC" O="$OUT" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
     CC="$GCC14 -B$BDIR" LOCALVERSION=-current-meson64 olddefconfig modules_prepare
# restore the authoritative symbol table from the headers package
cp <headers>/usr/src/linux-headers-6.18.44-current-meson64/Module.symvers "$OUT/"

# 3. Build the camera modules
cd <birdcher>/external/radxa-zero2pro-camera
KDIR="$OUT" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CC="$GCC14 -B$BDIR" \
    ./scripts/build.sh
```

### Build-host notes

Two host problems were solved along the way and are recorded so the build does
not have to rediscover them:

- The extracted GCC 14 cross-compiler ships no binutils, so it falls back to the
  **host x86 assembler**.  `-B<dir>` pointing at symlinks to the system
  `aarch64-linux-gnu-as`/`ld` fixes this without touching the host toolchain.
- Armbian's build container sees the bind-mounted cache under a foreign UID and
  git rejects it as "dubious ownership".  `tools/armbian-safe-git-container.sh`
  scopes `safe.directory` to the ephemeral container only.  It must declare an
  `add_host_dependencies__*` function, because Armbian loads only extensions
  carrying that hook before launching Docker.
