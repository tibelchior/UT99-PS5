# Linux build host for the PS5 title: RADV (Mesa) for the console, the title link and
# signing tool from mihawk-99/PS5_Vulkan, and the final link of the game.
# Fedora runs natively on Apple silicon; every compile here is a cross-compile to the PS5.
FROM fedora:43

RUN dnf -y install \
        bash coreutils diffutils findutils which file util-linux tar unzip wget curl git rsync make patch \
        clang clang-devel clang-tools-extra llvm llvm-devel lld \
        spirv-llvm-translator-devel spirv-tools-devel libclc-devel \
        gcc gcc-c++ meson ninja-build cmake pkgconf-pkg-config ccache bison flex glslang ffmpeg-free bsdtar perl-Digest-SHA xz \
        python3 python3-mako python3-pyyaml python3-packaging python3-pyelftools python3-pip \
        zlib-devel libzstd-devel elfutils-libelf-devel expat-devel \
    && dnf clean all

# Sources and build trees live in a volume mounted at /build; the Mac's project folder at /work.
ENV LLVM_CONFIG=/usr/bin/llvm-config
WORKDIR /build
