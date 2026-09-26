#!/bin/bash
# Configure once, then build only the Teflon target. Safe to re-run after a
# patch: meson keeps the build directory and ninja rebuilds what changed.
set -eux
cd /src/mesa
build_dir=${BUILD_DIR:-build}
extra_args=()
if [[ -n ${CROSS_FILE:-} ]]; then
    extra_args+=("--cross-file=$CROSS_FILE")
fi
if [[ ! -f "$build_dir/meson-private/coredata.dat" ]]; then
    meson setup "$build_dir" "${extra_args[@]}" \
        -Dteflon=true \
        -Dgallium-drivers=etnaviv \
        -Dvulkan-drivers= \
        -Dplatforms= \
        -Dglx=disabled \
        -Degl=disabled \
        -Dgbm=disabled \
        -Dopengl=true \
        -Dgles1=disabled \
        -Dgles2=disabled \
        -Dllvm=disabled \
        -Dvideo-codecs= \
        -Dgallium-va=disabled \
        -Dbuildtype=debugoptimized
fi
ninja -C "$build_dir" -j "${BUILD_JOBS:-4}" src/gallium/targets/teflon/libteflon.so
ls -l "$build_dir"/src/gallium/targets/teflon/libteflon.so
