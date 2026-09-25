#!/bin/bash
# Configure once, then build only the Teflon target. Safe to re-run after a
# patch: meson keeps the build directory and ninja rebuilds what changed.
set -eux
cd /src/mesa
if [ ! -d build ]; then
    meson setup build \
        -Dteflon=true \
        -Dgallium-drivers=etnaviv \
        -Dvulkan-drivers= \
        -Dplatforms= \
        -Dglx=disabled \
        -Degl=disabled \
        -Dgbm=disabled \
        -Dopengl=false \
        -Dgles1=disabled \
        -Dgles2=disabled \
        -Dllvm=disabled \
        -Dshared-glapi=disabled \
        -Dvideo-codecs= \
        -Dgallium-va=disabled \
        -Dgallium-vdpau=disabled \
        -Dbuildtype=debugoptimized
fi
ninja -C build src/gallium/targets/teflon/libteflon.so
ls -l build/src/gallium/targets/teflon/libteflon.so
