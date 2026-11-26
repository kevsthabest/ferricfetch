#!/usr/bin/env python3
"""Boot FFETCH under QEMU TCG in the two environments that define it.

Smoke A is a stock FreeDOS boot with no Ferric anywhere. It is fully
self-contained: the boot floppy and the hard-disk image are built
here from the container's FreeDOS staging (kernel.sys, command.com
and the fat12com.bin boot sector in /opt/freedos, overridable with
FFETCH_FREEDOS_DIR - see the Dockerfile for how they are pinned).
CONFIG.SYS is a bare shell line, so no memory manager and no Ferric
component ever load. FFETCH must print its generic lines, print no
Ferric line, emit JSON that parses, and exit cleanly.

Smoke B boots a Ferric DOS boot image (the Gate 1 resident profile,
menu 1), so FERRIC.JLM is resident when FFETCH runs. This repo does
not build such an image, so smoke B runs only when one is supplied
via --image or the FFETCH_FERRIC_IMG environment variable; without
one it is skipped, and the skip is reported, not hidden. Phase 1
requires the Ferric line (the version ping). With --phase2 it
additionally requires the Modules line, which can only appear when
the core's status verb (AX=0049h) answers; the Wi-Fi line is
reported either way, because QEMU has no AR2413 for the core to
find.

QEMU is a smoke screen, not an oracle: these runs prove the program
starts, probes and degrades correctly in emulation. They say nothing
about real hardware behaviour.

Usage: qemu_ffetch_smoke.py [--phase2] [--only a|b] [--image PATH] [--keep]
Run inside the build container after `sh build.sh`; expects
out/FFETCH.EXE and the FreeDOS staging described above.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def mcopy_in(img: Path, src: Path, name: str) -> None:
    subprocess.run(["mcopy", "-o", "-i", str(img), str(src), "::/" + name], check=True)


def mdel(img: Path, name: str) -> None:
    subprocess.run(["mdel", "-i", str(img), "::/" + name],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def mcopy_out(img: Path, name: str, dst: Path) -> bool:
    r = subprocess.run(["mcopy", "-o", "-i", str(img), "::/" + name, str(dst)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return r.returncode == 0


def write_dos_file(td: Path, name: str, text: str) -> Path:
    p = td / name
    p.write_bytes(text.replace("\n", "\r\n").encode("ascii"))
    return p


def install_boot_sector(img: Path, boot_sector: Path) -> None:
    """Splice a FreeDOS boot sector into a freshly mkfs.fat-formatted
    image, preserving the BPB mkfs.fat computed for the geometry
    (bytes [3..62) stay; jump and code come from the boot sector).
    This is the same splice the Ferric DOS image build performs."""
    boot = boot_sector.read_bytes()
    assert len(boot) == 512, f"unexpected boot sector size: {len(boot)}"
    with open(img, "r+b") as f:
        vbr = bytearray(f.read(512))
        vbr[0:3] = boot[0:3]
        vbr[62:512] = boot[62:512]
        vbr[0x24] = 0x00  # Floppy BIOS drive number
        f.seek(0)
        f.write(vbr)


def build_floppy(td: Path, freedos: Path) -> Path:
    """A stock 1.44 MiB FreeDOS boot floppy: release kernel, FreeCOM
    shell, nothing else resident."""
    img = td / "a.img"
    with open(img, "wb") as f:
        f.write(b"\x00" * (2880 * 512))
    subprocess.run(["mkfs.fat", "-F", "12", "-n", "FERRICFETCH",
                    "-i", "46455443", str(img)],
                   check=True, stdout=subprocess.DEVNULL)
    install_boot_sector(img, freedos / "fat12com.bin")
    mcopy_in(img, freedos / "kernel.sys", "KERNEL.SYS")
    mcopy_in(img, freedos / "command.com", "COMMAND.COM")
    return img


def lba_to_chs(lba: int, heads: int = 16, spt: int = 63) -> bytes:
    cyl = lba // (heads * spt)
    rem = lba % (heads * spt)
    head = rem // spt
    sec = (rem % spt) + 1
    if cyl > 1023:
        cyl, head, sec = 1023, heads - 1, spt
    return bytes([head & 0xFF, (sec & 0x3F) | ((cyl >> 2) & 0xC0), cyl & 0xFF])


def build_hdd(td: Path) -> Path:
    """A 16 MiB hard disk with one FAT16 partition at LBA 63, the
    geometry the Ferric DOS USB image uses. The MBR carries no boot
    code - this disk is never booted, it only has to answer as C: -
    so the partition table and the partition's own BPB (from
    mkfs.fat) are all the kernel needs to enumerate it."""
    part_start, total_sectors = 63, 32768
    part_sectors = total_sectors - part_start
    part = td / "part.img"
    with open(part, "wb") as f:
        f.write(b"\x00" * (part_sectors * 512))
    subprocess.run(["mkfs.fat", "-F", "16", "-h", str(part_start),
                    "-S", "512", "-s", "4", "-n", "FERRICFETCH",
                    "-i", "46455443", str(part)],
                   check=True, stdout=subprocess.DEVNULL)
    mbr = bytearray(512)
    mbr[440:444] = b"FDOS"  # Deterministic disk signature
    entry = (
        bytes([0x80])
        + lba_to_chs(part_start)
        + bytes([0x06])  # FAT16B
        + lba_to_chs(part_start + part_sectors - 1)
        + struct.pack("<II", part_start, part_sectors)
    )
    mbr[446:462] = entry
    mbr[510:512] = b"\x55\xAA"
    img = td / "hdd.img"
    with open(img, "wb") as out:
        out.write(mbr)
        out.write(b"\x00" * ((part_start - 1) * 512))
        out.write(part.read_bytes())
        assert out.tell() == total_sectors * 512
    return img


def boot_and_wait(img: Path, log: Path, end_marker: str, timeout_s: float,
                  extra_args: list[str]) -> str:
    if log.exists():
        log.unlink()
    cmd = [
        "qemu-system-i386", "-machine", "pc", "-accel", "tcg", "-m", "64",
        "-display", "none", "-serial", f"file:{log}", "-no-reboot",
        *extra_args,
        "-drive", f"file={img},format=raw,if=floppy",
        "-boot", "order=a",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + timeout_s
    try:
        while time.time() < deadline:
            if log.exists() and end_marker in log.read_text(errors="replace"):
                break
            if proc.poll() is not None:
                break
            time.sleep(0.5)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    return log.read_text(errors="replace") if log.exists() else ""


def smoke_a(root: Path, td: Path) -> bool:
    print("=== FFETCH SMOKE A: stock FreeDOS, no Ferric ===")
    freedos = Path(os.environ.get("FFETCH_FREEDOS_DIR", "/opt/freedos"))
    for name in ("kernel.sys", "command.com", "fat12com.bin"):
        if not (freedos / name).exists():
            print(f"missing FreeDOS staging file: {freedos / name} "
                  f"(see the Dockerfile, or set FFETCH_FREEDOS_DIR)")
            return False
    img = build_floppy(td, freedos)
    config = write_dos_file(td, "CONFIG.SYS",
                            "SHELL=COMMAND.COM /P /E:512 /F\n"
                            "DOS=LOW\nFILES=40\nBUFFERS=20\nLASTDRIVE=Z\n")
    autoexec = write_dos_file(td, "AUTOEXEC.BAT",
                              "@ECHO OFF\n"
                              "ECHO === FFETCH A BEGIN === > COM1\n"
                              "FFETCH.EXE /NOCLS > COM1\n"
                              "ECHO === FFETCH A JSON BEGIN === > COM1\n"
                              "FFETCH.EXE /JSON > COM1\n"
                              "ECHO === FFETCH A JSON END === > COM1\n"
                              "FFETCH.EXE /SAVE=FFSAVE.TXT /NOCLS\n"
                              "ECHO === FFETCH A END === > COM1\n")
    mcopy_in(img, config, "CONFIG.SYS")
    mcopy_in(img, autoexec, "AUTOEXEC.BAT")
    mcopy_in(img, root / "out" / "FFETCH.EXE", "FFETCH.EXE")

    # A hard disk for C:: FFETCH deliberately never probes the floppy
    # drives (a dry spin can stall real hardware), so a floppy-only
    # boot has no Disk line to check - the honest outcome an early
    # version of this smoke mistook for a failure. The disk built
    # above only has to answer as C:; nothing on it is loaded.
    hdd = build_hdd(td)

    log = td / "a.log"
    text = boot_and_wait(img, log, "=== FFETCH A END ===", 90.0,
                         ["-cpu", "pentium3", "-nic", "none",
                          "-drive", f"file={hdd},format=raw,if=ide"])
    ok = True
    for marker in ("OS:", "CPU:", "Memory:", "Disk: C:", "Uptime:"):
        if marker not in text:
            print(f"FAIL A: expected marker {marker!r} in serial output")
            ok = False
    if "Ferric:" in text:
        print("FAIL A: Ferric line printed on a machine with no core")
        ok = False
    m = re.search(r"=== FFETCH A JSON BEGIN ===\s*(.*?)\s*=== FFETCH A JSON END ===",
                  text, re.DOTALL)
    if not m:
        print("FAIL A: JSON block not found in serial output")
        ok = False
    else:
        try:
            data = json.loads(m.group(1))
            print(f"JSON OK: parsed {len(data)} keys: {sorted(data.keys())}")
            if "os" not in data:
                print("FAIL A: JSON has no 'os' key")
                ok = False
            if "disk" not in data:
                print("FAIL A: JSON has no 'disk' key")
                ok = False
        except json.JSONDecodeError as e:
            print(f"FAIL A: JSON did not parse: {e}")
            print("---- raw JSON block ----")
            print(m.group(1))
            ok = False
    saved = td / "FFSAVE.TXT"
    if mcopy_out(img, "FFSAVE.TXT", saved):
        body = saved.read_text(errors="replace")
        print(f"SAVE OK: FFSAVE.TXT {len(body)} bytes, has OS line: {'OS:' in body}")
        if "OS:" not in body:
            ok = False
    else:
        print("FAIL A: /SAVE did not produce FFSAVE.TXT")
        ok = False
    pretty = re.search(r"=== FFETCH A BEGIN ===\s*(.*?)\s*=== FFETCH A JSON BEGIN ===",
                       text, re.DOTALL)
    if pretty:
        print("---- FFETCH output (smoke A) ----")
        print(pretty.group(1))
    print("SMOKE A:", "PASS" if ok else "FAIL")
    return ok


def smoke_b(root: Path, td: Path, phase2: bool, ferric_img: Path) -> bool:
    print(f"=== FFETCH SMOKE B: Ferric resident profile (phase2={phase2}) ===")
    img = td / "b.img"
    shutil.copy2(ferric_img, img)
    for name in ("EXFAT.JLM", "NTFS.JLM", "VBE.JLM"):
        mdel(img, name)
    # Menu profile 1 (Gate 1 resident) with a 1-second default timer.
    cfg = td / "CONFIG.SYS"
    subprocess.run(["mcopy", "-o", "-i", str(img), "::/CONFIG.SYS", str(cfg)], check=True)
    data = cfg.read_bytes()
    if data.count(b"MENUDEFAULT=0,3") != 1:
        print("FAIL B: CONFIG.SYS patch anchor missing "
              "(the supplied image is not a Ferric DOS boot image)")
        return False
    cfg.write_bytes(data.replace(b"MENUDEFAULT=0,3", b"MENUDEFAULT=1,1"))
    mcopy_in(img, cfg, "CONFIG.SYS")
    # Run FFETCH right after G1LIVE in the :LIVE branch.
    ae = td / "AUTOEXEC.BAT"
    subprocess.run(["mcopy", "-o", "-i", str(img), "::/AUTOEXEC.BAT", str(ae)], check=True)
    data = ae.read_bytes()
    anchor = b"G1LIVE.COM\r\nIF ERRORLEVEL 1 GOTO FAILED\r\nGOTO END"
    if data.count(anchor) != 1:
        print("FAIL B: AUTOEXEC.BAT patch anchor missing "
              "(the supplied image is not a Ferric DOS boot image)")
        return False
    patched = (b"G1LIVE.COM\r\nIF ERRORLEVEL 1 GOTO FAILED\r\n"
               b"ECHO === FFETCH B BEGIN === > COM1\r\n"
               b"FFETCH.EXE /NOCLS > COM1\r\n"
               b"ECHO === FFETCH B END === > COM1\r\n"
               b"GOTO END")
    ae.write_bytes(data.replace(anchor, patched))
    mcopy_in(img, ae, "AUTOEXEC.BAT")
    mcopy_in(img, root / "out" / "FFETCH.EXE", "FFETCH.EXE")

    log = td / "b.log"
    text = boot_and_wait(img, log, "=== FFETCH B END ===", 120.0,
                         ["-cpu", "pentium3", "-nic", "none"])
    block = re.search(r"=== FFETCH B BEGIN ===\s*(.*?)\s*=== FFETCH B END ===",
                      text, re.DOTALL)
    ok = True
    if not block:
        print("FAIL B: FFETCH block not found in serial output")
        print("---- serial tail ----")
        print(text[-2000:])
        return False
    body = block.group(1)
    print("---- FFETCH output (smoke B) ----")
    print(body)
    if "Ferric:" not in body:
        print("FAIL B: Ferric line missing with the core resident")
        ok = False
    if phase2:
        if "Modules:" not in body:
            print("FAIL B(phase2): Modules line missing - status verb did not answer")
            ok = False
        print("Wi-Fi line present:", "Wi-Fi:" in body)
    print("SMOKE B:", "PASS" if ok else "FAIL")
    return ok


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--phase2", action="store_true",
                    help="require the Phase 2 status-verb lines in smoke B")
    ap.add_argument("--only", choices=("a", "b"), default=None,
                    help="run just one smoke")
    ap.add_argument("--image", type=Path, default=None,
                    help="Ferric DOS boot image for smoke B "
                         "(or set FFETCH_FERRIC_IMG); without one, smoke B is skipped")
    ap.add_argument("--keep", action="store_true", help="keep the temp directory")
    args = ap.parse_args()
    root = Path(__file__).resolve().parent
    if not (root / "out" / "FFETCH.EXE").exists():
        print(f"missing build product: {root / 'out' / 'FFETCH.EXE'}")
        return 2
    ferric_img = args.image
    if ferric_img is None and os.environ.get("FFETCH_FERRIC_IMG"):
        ferric_img = Path(os.environ["FFETCH_FERRIC_IMG"])
    run_b = args.only != "a"
    if run_b and ferric_img is None:
        print("SMOKE B: SKIPPED - no Ferric boot image supplied "
              "(pass --image PATH or set FFETCH_FERRIC_IMG). This repo "
              "does not build one; smoke B needs an image from the "
              "Ferric DOS tree, where the resident core lives.")
        run_b = False
    if run_b and not ferric_img.exists():
        print(f"supplied Ferric boot image not found: {ferric_img}")
        return 2
    td_ctx = tempfile.TemporaryDirectory(prefix="ffetch-smoke-")
    td = Path(td_ctx.name)
    try:
        ok_a = smoke_a(root, td) if args.only != "b" else True
        ok_b = smoke_b(root, td, args.phase2, ferric_img) if run_b else True
    finally:
        if args.keep:
            print(f"kept temp dir: {td}")
            td_ctx = None
        else:
            td_ctx.cleanup()
    print("FFETCH SMOKES:", "PASS" if (ok_a and ok_b) else "FAIL")
    return 0 if (ok_a and ok_b) else 1


if __name__ == "__main__":
    sys.exit(main())
