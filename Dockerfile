# FerricFetch pinned build environment
# Modelled on the Ferric DOS tree's build image: same base image,
# same OpenWatcom and FreeCOM pins, so binaries built here and there
# are comparable. Two deliberate trims versus that image: no Rust
# toolchain (FerricFetch is C and NASM only) and no Jemm/HDPMI
# staging (nothing in this repo loads them - smoke A boots a bare
# shell). Everything downloaded is pinned by version and SHA-256.
FROM debian:bookworm-slim@sha256:734a72b3d6142a71993b4bc580ca01d8ca687f77a2c422d96a961d3376b179bc

ARG OPENWATCOM_VERSION=2026-11-01-Build
ARG OPENWATCOM_URL=https://github.com/open-watcom/open-watcom-v2/releases/download/${OPENWATCOM_VERSION}/ow-snapshot.tar.xz
ARG OPENWATCOM_SHA256=89e90b7544cf3ea4a38124ec4c104a5027f31101405d7115c3f833daa465bb48
ARG FREECOM_VERSION=com086
ARG FREECOM_URL=https://github.com/FDOS/freecom/releases/download/${FREECOM_VERSION}/English.zip
ARG FREECOM_ZIP_SHA256=42093a00286f66bf922eebd8204a3b74b429f4c6973cd960ae1ab52ce80b4bd5
ARG FREECOM_CMD_SHA256=fc74a98b51fc10119ea2e52554915d6a405eebdb58f44b03109b952f10835c21
ARG FREECOM_XMS_CMD_SHA256=d33d3e0a3a0c02ef74d9b9e6d72a3adc7969630bacb7ec8e4e6a7cb197dcefb2
# FreeDOS kernel 2045, the same kernel version the Ferric DOS tree
# builds from vendored source; here the official binary release is
# staged instead, because smoke A only needs a stock kernel to boot.
ARG KERNEL_VERSION=ke2045
ARG KERNEL_URL=https://github.com/FDOS/kernel/releases/download/${KERNEL_VERSION}/ke2045_86f16.zip
ARG KERNEL_ZIP_SHA256=4d20c0a30d63509aa7e97e41d9097104b60bcc899f87ae28653d2c51d38e4fff
ARG KERNEL_SYS_SHA256=7b44a8aebfd3c83414daf93d22ae00e7366fbe26b25f9b249e39f5a834a8f0b0
# The kernel source release supplies boot/boot.asm, which NASM turns
# into the FAT12/FAT16 boot sectors smoke A splices into its images
# (the same sectors, assembled the same way, as the Ferric DOS build).
ARG KERNEL_SRC_URL=https://github.com/FDOS/kernel/releases/download/${KERNEL_VERSION}/ke2045s.zip
ARG KERNEL_SRC_SHA256=b0d61e0a057346facde98bc215d9348d29fa27b29ca668bc695ea206695deec0
ARG FAT12COM_SHA256=b53d07aa6b999dc8379ceebb641b1375b8c82fa30839552868356ec783d5714a
ARG FAT16COM_SHA256=545a556405919560022ecbc55b48cff5e8047c655b30f81ac59762da9bb4fa39

ENV DEBIAN_FRONTEND=noninteractive \
    WATCOM=/opt/watcom \
    EDPATH=/opt/watcom/eddat \
    INCLUDE=/opt/watcom/h \
    PATH=/opt/watcom/binl64:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    SOURCE_DATE_EPOCH=1795132800

# Disk/FAT utilities, QEMU i386 emulator, NASM, and Python 3
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        dosfstools \
        mtools \
        nasm \
        python3 \
        qemu-system-x86 \
        xz-utils && \
    rm -rf /var/lib/apt/lists/*

# OpenWatcom v2 pinned by dated release tag and SHA-256 content hash (not floating snapshot)
RUN curl --proto '=https' --tlsv1.2 -sSfL "${OPENWATCOM_URL}" -o /tmp/ow-snapshot.tar.xz && \
    echo "${OPENWATCOM_SHA256}  /tmp/ow-snapshot.tar.xz" | sha256sum -c - && \
    mkdir -p /opt/watcom && \
    tar -xJf /tmp/ow-snapshot.tar.xz -C /opt/watcom && \
    rm -f /tmp/ow-snapshot.tar.xz && \
    rm -rf /opt/watcom/binnt /opt/watcom/binnt64 /opt/watcom/binw /opt/watcom/binp /opt/watcom/binl /opt/watcom/binb && \
    chmod -R a+rX /opt/watcom

# FreeCOM com086 plainedt/command.com and xmsswap/command.com pinned by version and SHA-256
RUN curl --proto '=https' --tlsv1.2 -sSfL "${FREECOM_URL}" -o /tmp/freecom-english.zip && \
    echo "${FREECOM_ZIP_SHA256}  /tmp/freecom-english.zip" | sha256sum -c - && \
    mkdir -p /opt/freedos && \
    python3 -c "import zipfile; z = zipfile.ZipFile('/tmp/freecom-english.zip'); open('/opt/freedos/command.com', 'wb').write(z.read('plainedt/command.com')); open('/opt/freedos/xmscmd.com', 'wb').write(z.read('xmsswap/command.com'))" && \
    echo "${FREECOM_CMD_SHA256}  /opt/freedos/command.com" | sha256sum -c - && \
    echo "${FREECOM_XMS_CMD_SHA256}  /opt/freedos/xmscmd.com" | sha256sum -c - && \
    rm -f /tmp/freecom-english.zip && \
    chmod 0644 /opt/freedos/command.com /opt/freedos/xmscmd.com

# FreeDOS kernel 2045 (8086, FAT16 build) pinned by version and SHA-256
RUN curl --proto '=https' --tlsv1.2 -sSfL "${KERNEL_URL}" -o /tmp/kernel.zip && \
    echo "${KERNEL_ZIP_SHA256}  /tmp/kernel.zip" | sha256sum -c - && \
    python3 -c "import zipfile; z = zipfile.ZipFile('/tmp/kernel.zip'); open('/opt/freedos/kernel.sys', 'wb').write(z.read('bin/kernel.sys'))" && \
    echo "${KERNEL_SYS_SHA256}  /opt/freedos/kernel.sys" | sha256sum -c - && \
    rm -f /tmp/kernel.zip && \
    chmod 0644 /opt/freedos/kernel.sys

# FAT12/FAT16 boot sectors assembled from the kernel 2045 source with NASM, pinned by SHA-256
RUN curl --proto '=https' --tlsv1.2 -sSfL "${KERNEL_SRC_URL}" -o /tmp/kernel-src.zip && \
    echo "${KERNEL_SRC_SHA256}  /tmp/kernel-src.zip" | sha256sum -c - && \
    python3 -c "import zipfile; z = zipfile.ZipFile('/tmp/kernel-src.zip'); [open('/tmp/' + n, 'wb').write(z.read('SOURCE/ke2045/boot/' + n)) for n in ('boot.asm', 'magic.mac')]" && \
    cd /tmp && \
    nasm -dISFAT12 boot.asm -o /opt/freedos/fat12com.bin && \
    nasm -dISFAT16 boot.asm -o /opt/freedos/fat16com.bin && \
    echo "${FAT12COM_SHA256}  /opt/freedos/fat12com.bin" | sha256sum -c - && \
    echo "${FAT16COM_SHA256}  /opt/freedos/fat16com.bin" | sha256sum -c - && \
    rm -f /tmp/kernel-src.zip /tmp/boot.asm /tmp/magic.mac && \
    chmod 0644 /opt/freedos/fat12com.bin /opt/freedos/fat16com.bin

WORKDIR /work
