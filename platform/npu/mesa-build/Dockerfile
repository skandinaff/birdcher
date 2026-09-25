# arm64 build environment for Mesa's Teflon delegate, matching the VIM3's
# Ubuntu 26.04 userspace. Runs on x86 through qemu-user binfmt.
#
# The source and build tree live inside the image, not on a bind mount: the
# host's /mnt/shed is NTFS over FUSE and tar's mkdir returns ENOSYS there under
# emulation. Only the finished .so is copied out.
FROM ubuntu:26.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y --no-install-recommends \
        build-essential meson ninja-build pkg-config git ca-certificates \
        curl xz-utils python3-mako python3-yaml python3-setuptools \
        bison flex libdrm-dev libexpat1-dev zlib1g-dev libzstd-dev \
    && rm -rf /var/lib/apt/lists/*

# The source is extracted on the host and COPYed in. GNU tar inside the
# emulated container gets ENOSYS from a syscall qemu-user does not implement,
# on any filesystem, so extraction cannot happen in a RUN step. COPY is done by
# the daemon on the host natively, which sidesteps it.
WORKDIR /src
COPY mesa-src /src/mesa
