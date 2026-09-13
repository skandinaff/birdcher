# IMX415 test-kernel work

## Target

The running target is `6.18.44-current-meson64`. Its deployed configuration
already enables the media core, media controller, V4L2, V4L2 subdevice API,
Meson video decoder, and etnaviv, but has `CONFIG_VIDEO_IMX415` unset.

A clean matching upstream Linux `v6.18.44` source tree is used because the
installed Armbian build ID is internal. The test configuration is copied from
`/boot/config-6.18.44-current-meson64`, retains the `-current-meson64` local
version, and changes only `CONFIG_VIDEO_IMX415=m`.

## Safety

- The current `/boot` is preserved at `~/boot-backup`.
- The live DT is preserved at `~/current-running.dts`.
- Build output lives under ignored `build/`; nothing is installed to `/boot`
  or `/lib/modules` and no reboot is scheduled automatically.

## Build status

- Build prerequisites were installed: `build-essential`, `flex`, `bison`,
  `libssl-dev`, and `libelf-dev`.
- `make modules_prepare` completed successfully.
- A direct module-only build stopped at modpost because no matching `vmlinux`
  symbol table exists in the installed kernel package. This is expected and
  does not indicate a source error.
- `scripts/build-test-kernel.sh` is building a matching test `Image` at two
  parallel jobs. After it completes, the IMX415 module and its V4L2
  dependencies can be built against that test kernel's symbol table.

No test kernel will be installed or booted until the module build succeeds,
the prior kernel remains selectable, and the rollback plan is rechecked.
