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

Unpatched 26.0.8 and main have been built and tested on the board. The 26.0.8
control reproduces zero outputs with kernel faults; main eliminates those faults
in the recorded runs but still gives incorrect detections. Two candidate patches
on main restore CPU-like results on three saved frames. See the
[measured results](../../../docs/tasks/2026-09-26-teflon-two-fixes.md).

## Native cross-build (2026-09-26)

The existing `mesa-teflon-arm64:26.0.8` image supplies the arm64 development
headers/libraries. `Dockerfile.cross` runs the compiler natively on x86_64 and
copies that sysroot; it does not install anything on the board. It also adds
`python3-pycparser`, required by the etnaviv hardware database generator.

From this directory, with a Mesa source checkout at `/tmp/birdcher-mesa-main`:

```sh
docker build -f Dockerfile.cross -t birdcher-mesa-cross:26.04 .
docker run --rm --platform linux/amd64 \
  -v /tmp/birdcher-mesa-main:/src/mesa \
  -v "$PWD":/scripts:ro \
  -e CROSS_FILE=/scripts/aarch64-cross.ini -e BUILD_DIR=build-cross \
  -e BUILD_JOBS=8 \
  birdcher-mesa-cross:26.04 bash /scripts/configure-and-build.sh
```

Keep the cross-file mounted at the same path on subsequent builds: Meson
records it. The build script checks `coredata.dat`, not merely the directory's
existence (Docker creates an empty named-volume mountpoint before Meson runs).
The current main needs OpenGL enabled to pull in NIR/TGSI dependencies even
when only the Teflon target is requested; `-Dopengl=false` failed to link.
No source patch is needed for that configuration adjustment.

Copy the resulting `build-cross/src/gallium/targets/teflon/libteflon.so` into a
separate board test directory and pass its absolute path with `TEFLON_LIB`.
Record source commit, build options, library SHA-256 and runtime dependencies.
The system Mesa library must remain in place for the A/B control.

Use `../tools/run-probe-checked.sh` on the board to retain kernel messages for
each invocation. Exit 3 means a kernel GPU/NPU fault was observed, regardless
of the probe's exit status. Exit 0 is only an execution check, not an accuracy
claim. Run one hardware test at a time.

```sh
mkdir -p /tmp/npu-check/dumps
./run-probe-checked.sh /tmp/npu-check/main \
  env TEFLON_LIB=/tmp/libteflon-main.so TEFLON_DEBUG=verbose \
      DUMP_DIR=/tmp/npu-check/dumps \
  ./tflite-tensor-probe model.tflite frame.rgb npu 198 203 324 325 326
```

The probe now preserves TFLite arena tensors and writes requested/declared
outputs as binary files when `DUMP_DIR` names an existing directory. This does
not materialize tensors internal to a delegate. `+poison` accepts only declared
outputs, fills them before copying the input, and cannot prove hardware job
completion. `EXTRA_OUTPUTS` changes the graph and must be treated as a separate
experiment.

## Candidate patches

Apply both files under `patches/` in order to main commit
`a5d39a4b743b719cfc2a0da410910ea3fd8ff770`, then rerun the build command above:

```sh
git -C /tmp/birdcher-mesa-main apply "$PWD"/patches/0001-*.patch
git -C /tmp/birdcher-mesa-main apply "$PWD"/patches/0002-*.patch
```

Patch 1 bounds ADD synthetic-weight reads during v7 bias correction. Patch 2
restores NHWC order before reshape. These are separately measured candidates;
they have not been accepted upstream or validated across models/hardware. Keep
an unpatched library for comparison and check numerical outputs as well as the
kernel journal. The patches do not establish the cause of 26.0.8 MMU faults.
