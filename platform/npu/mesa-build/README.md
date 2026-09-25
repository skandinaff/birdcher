# Local Mesa build, for testing the Teflon/etnaviv detector defect

Builds Mesa 26.0.8's `libteflon.so` for arm64 so the etnaviv ML code can be
instrumented and patched. The board's packaged Mesa is never touched: the probe
takes an alternate delegate through `TEFLON_LIB`.

```sh
# once: extract the source on the HOST (see the note below), then build the image
curl -fL -o mesa.tar.gz \
  https://gitlab.freedesktop.org/mesa/mesa/-/archive/mesa-26.0.8/mesa-mesa-26.0.8.tar.gz
mkdir mesa-src && tar -xzf mesa.tar.gz -C mesa-src --strip-components=1
docker build --platform linux/arm64 -t mesa-teflon-arm64:26.0.8 .

# each build, keeping the build directory in a named volume so a patch is incremental
docker run --rm --platform linux/arm64 \
    -v mesa-teflon-build:/src/mesa/build \
    -v "$PWD":/scripts \
    mesa-teflon-arm64:26.0.8 bash /scripts/configure-and-build.sh

# then copy the .so to the board and A/B against the packaged one
TEFLON_LIB=/home/skf/birdcher-tools/npu/libteflon-local.so ./tflite-tensor-probe ...
```

## Two things that cost an evening

**qemu-user cannot run GNU tar.** Extracting inside the emulated container
fails with `Cannot mkdir: Function not implemented` (ENOSYS) on *any*
filesystem -- it is a syscall qemu-aarch64 does not implement, not a mount
problem. Extract on the host and `COPY` the tree in; `COPY` is performed by the
daemon natively.

**`/mnt/shed` is NTFS over FUSE.** Do not put a build tree there, bind-mounted
or otherwise. This was the first, wrong explanation for the tar failure.

## Status

The image builds. `meson setup` and `ninja` have **not** been run yet. The first
run must be the unpatched control: it has to reproduce the detector's all-zero
outputs, or the test bench proves nothing. See
[../../../docs/tasks/2026-09-25-teflon-detector-investigation.md](../../../docs/tasks/2026-09-25-teflon-detector-investigation.md).
