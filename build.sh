#!/bin/sh
set -eu

# Build the FerricFetch system-information tool inside the build
# container (see Dockerfile; the ferric-dos-build:gate0 image from the
# Ferric DOS tree carries the same Watcom/NASM pins and works too).
# FerricFetch is a 16-bit real-mode DOS program written in C
# (OpenWatcom), because its whole job is talking to real-mode
# interfaces - DOS and BIOS interrupts, the BIOS data area, I/O
# ports - from a program any DOS install can run. A 32-bit tool
# would need a DOS extender, and a fetch tool that requires an
# extender has already failed its purpose.
#
# Memory model: medium (far code, near data). The module set plus the
# PCI name table outgrow a single 64 KB code segment, while every
# buffer is a small static array, so near data is never a constraint.
# Code generation targets the 8086 (-0): the few routines that need
# 386 instructions (CPUID, EFLAGS) live in src/cpu.asm behind a
# runtime CPU ladder, so the C code itself stays legal on an 8088.
ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${ROOT_DIR}"

OUT_DIR="${ROOT_DIR}/out"
OBJ_DIR="${OUT_DIR}/ffetch-obj"
mkdir -p "${OUT_DIR}" "${OBJ_DIR}"

export WATCOM="${WATCOM:-/opt/watcom}"
export PATH="${WATCOM}/binl64:${PATH}"

echo "[ffetch] Assembling low-level call helpers (src/cpu.asm)..."
nasm -f obj -o "${OBJ_DIR}/cpu.obj" src/cpu.asm

for src in ffetch modules modules2 ferric; do
    echo "[ffetch] Compiling src/${src}.c (OpenWatcom, 16-bit medium model)..."
    wcc -bt=dos -mm -0 -os -s -zq \
        -fo="${OBJ_DIR}/${src}.obj" "src/${src}.c"
done

echo "[ffetch] Linking out/FFETCH.EXE..."
cat > "${OBJ_DIR}/ffetch.lnk" <<EOF
format dos
name ${OUT_DIR}/FFETCH.EXE
option quiet
option map=${OUT_DIR}/FFETCH.MAP
libpath ${WATCOM}/lib286/dos
libpath ${WATCOM}/lib286
file ${OBJ_DIR}/ffetch.obj
file ${OBJ_DIR}/modules.obj
file ${OBJ_DIR}/modules2.obj
file ${OBJ_DIR}/ferric.obj
file ${OBJ_DIR}/cpu.obj
EOF
wlink @"${OBJ_DIR}/ffetch.lnk"

EXE_SIZE="$(wc -c < "${OUT_DIR}/FFETCH.EXE")"
echo "[ffetch] Built ${OUT_DIR}/FFETCH.EXE: total=${EXE_SIZE} bytes"
sha256sum "${OUT_DIR}/FFETCH.EXE"
