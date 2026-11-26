# FerricFetch — a fastfetch-style system summary for DOS

FerricFetch prints a neofetch/fastfetch-style summary of the machine
it runs on: a small ASCII logo on the left, labelled facts on the
right. It is a 16-bit real-mode DOS program that runs on any DOS
install — MS-DOS, DR-DOS, FreeDOS, DOSBox — and says more on a
Ferric machine, because a Ferric machine can answer more.

The binary is `FFETCH.EXE` because DOS filenames are 8.3;
"FerricFetch" is the project's name, `FFETCH` is what fits on the
disk. FerricFetch began life as `crates/ffetch` in the Ferric DOS
tree and was extracted into its own repo so the tool can live its
own life for any DOS install, not just Ferric ones.

## The one rule

**A line is printed only when its data source actually answered.**
Every module probes first and stays silent on no answer. Nothing is
inferred from the DOS flavour, the machine name, or what "should" be
there: a missing XMS driver, an absent APM BIOS and a machine with no
VBE are all normal outcomes, and the summary simply gets shorter.
This is the difference between a summary and a guess, and it is why
the program is built as three tiers of probes rather than one list
of printfs.

## Tiers

**Tier 1 — generic DOS and BIOS.** DOS flavour/version and true
kernel version (INT 21h), FreeDOS kernel build when the FreeDOS OEM
byte and build call agree; CPU via a FLAGS ladder (8088-safe) with
CPUID family/model/brand only after the ladder and the EFLAGS ID-bit
test both pass; conventional/XMS/EMS memory; every ready hard drive
with sizes from INT 21h AH=36h and the FAT type from the Microsoft
cluster-count rule (redirected drives are labelled "redirected"
instead, via the IOCTL remote bit — the FAT rule cannot apply to
them); EDD physical-disk sizes (INT 13h); uptime from the BIOS tick
counter; date/time; country and codepage; COMSPEC shell (FreeCOM is
identified by scanning the shell binary for its own name, a positive
identification rather than an inference); USERNAME@COMPUTERNAME and
EDITOR from the environment; resident programs by walking the MCB
chain (the DOS analogue of a process list); joystick by a bounded
gameport timing probe; the FreeDOS CONFIG.SYS menu selection from
the `CONFIG` environment variable FreeCOM sets.

**Tier 2 — generic hardware interfaces.** VBE adapter/mode info
(INT 10h), PCI device naming through the PCI BIOS (INT 1Ah) with a
small built-in ID table and a class-name fallback — unknown IDs are
printed as raw hex, never as an invented product name; BIOS date and
vendor from the F000 segment; SMBIOS Type 0/1/2/17 (host, board,
installed memory) with bounds-checked parsing; APM battery and AC
status interrogated the APMSTAT way (install check, connect, one
read-only query, disconnect); Sound Blaster DSP reset handshake and
the AdLib two-timer test, both timeout-bounded so absent hardware
cannot hang the summary; packet-driver discovery by the "PKT DRVR"
signature in INT 60h–7Fh, its MAC via the driver API, and an IP
address only when an mTCP configuration file (named by `MTCPCFG`)
states one.

**Tier 3 — Ferric.** The resident core is probed exactly the way the
other Ferric tools probe it: INT 2Fh AX=1684h, BX=0FD01h returns the
V86 API entry point; a version call (AX=0000h) must answer AX=0001h,
BX=0FD01h. Only then does the `Ferric:` line print. Extended data
comes from the status verb defined below; cores without it set
carry, and the extended lines simply never appear.

## Building

Inside this repo's pinned build container:

```
docker build -t ferricfetch-build .
docker run --rm -v "$PWD:/work" -w /work ferricfetch-build sh build.sh
```

Produces `out/FFETCH.EXE`. (The Ferric DOS tree's
`ferric-dos-build:gate0` image carries the same Watcom/NASM pins and
builds it identically.) FerricFetch is a userland tool, copied onto
a DOS disk like any other — it is not part of any boot image.

Why C and OpenWatcom, when the Ferric resident tools are Rust or
NASM: the program's whole job is real-mode contracts (DOS/BIOS
interrupts, the BIOS data area, I/O ports). A 32-bit Rust tool would
need a DOS extender in front of it, and a fetch tool that needs an
extender has failed before it starts. Why the **medium memory
model**: the module set plus the PCI name table outgrow one 64 KB
code segment, while every buffer is a small static array, so far
code with near data is the smallest model that fits without
far-pointer plumbing in every interrupt call. Why `-0` (8086 code
generation): the Tier 1 path must start on an 8088. The handful of
routines that truly need 386 instructions — the EFLAGS/CPUID work,
the register-level far calls (XMS entry, Ferric API entry) and the
32-bit-register PCI BIOS calls — live in `src/cpu.asm`, each behind
a runtime gate that keeps them unreachable on older CPUs.

## Usage

```
FFETCH [/NOLOGO] [/NOCLS] [/JSON] [/SAVE=<file>] [/LOGO=<file>]
       [/C=<file>] [/MODULES=<a,b,c>] [/GENCFG] [/?
```

- `FFETCH.CFG` beside the executable (or `/C=`) holds `key=value`
  settings: `logo=<path>|none`, `nologo=1`, `cls=0|1`,
  `color_key=0-7`, `color_logo=0-7`, `modules=<list>`. `/GENCFG`
  writes a commented default. Command-line switches beat the file.
- `/JSON` prints one JSON object (module names as keys), no logo, no
  ANSI — safe to capture and parse.
- `/SAVE=<file>` also writes the plain-text rendering to a file.
- When stdout is redirected to a file or pipe (checked via IOCTL),
  colours and the logo are suppressed automatically: captured output
  is plain text. ANSI colours are emitted only when an ANSI driver
  answers INT 2Fh AX=1A00h, so a stock console never shows escape
  garbage.
- Module names for `/MODULES=` and the config file: `title os kernel
  ferric host board bios cpu gpu display memory physmem disk
  physdisk sound network wifi modules battery power bootmgr shell
  editor terminal locale processes gamepad uptime datetime`.

## The Ferric status verb (V86 API AX=0049h)

Read-only. Verbs 0045h–0048h are taken by the Ferric core's
mouse-trampoline API, so 0049h is the first verb free in the
dispatch table. Chosen through the same INT 2Fh AX=1684h entry as
every other core service.

**Call:** `AX=0049h`, `ES:DI` = caller buffer, `CX` = buffer
capacity (512 recommended).
**Return:** carry clear and `AX` = bytes written on success; carry
set on any core that does not implement the verb (the dispatch's
standard unsupported-verb answer). The buffer is otherwise
untouched, so callers should pre-fill it with a sentinel and check
the magic before trusting any field.

**Buffer layout, version 1** (all integers little-endian):

| Offset | Size | Field |
|-------:|-----:|-------|
| 0x00 | u16 | magic, always `0x4646` ("FF") |
| 0x02 | u16 | structure version, 1 |
| 0x04 | u16 | total bytes written |
| 0x06 | u16 | flags: bit0 Wi-Fi hardware present, bit1 Wi-Fi associated, bit2 network device present, bit3 `ip` field valid |
| 0x08 | u8 | SSID length (0–32) |
| 0x09 | 32 | SSID bytes (not NUL-terminated; use the length) |
| 0x29 | 6 | BSSID of the associated AP (zero when unassociated) |
| 0x2F | u8 | channel (0 when unknown) |
| 0x30 | u8 | last received RSSI (raw driver units) |
| 0x31 | u8 | reserved, zero |
| 0x32 | 4 | IPv4 address of the core's stack (only when flag bit3) |
| 0x36 | 6 | MAC of the active network backend |
| 0x3C | u8 | module inventory count (0–8) |
| 0x3D | 14×n | inventory entries: u16 DDB ID, then 12 bytes of NUL-padded ASCII name |

**Versioning:** a future layout bumps the version word at 0x02 and
may extend the block; readers must check magic *and* version and
must not read past the total at 0x04. Fields a core cannot fill are
zeroed, never left as caller garbage — the inventory, for instance,
contains only modules the core itself can attest (today: the core,
`FERRIC`, DDB 0xFD01). The verb changes no state: it is a snapshot,
safe to call at any time, from any program.

**Phase note:** FerricFetch implements the client side
unconditionally. The core side lives in the Ferric DOS tree
(`gate1-svc`); on a core without it, FerricFetch prints the
`Ferric:` line (from the version call) and none of the verb's lines,
which is the designed degrade.

## Testing

```
python3 qemu_ffetch_smoke.py [--phase2] [--image <ferric-dos-pxe.img>]
```

(inside the container, after `sh build.sh`). Smoke A is fully
self-contained in this repo: it builds a stock FreeDOS boot floppy
and hard-disk image from the container's pinned staging
(`/opt/freedos`: `kernel.sys`, `command.com`, `fat12com.bin`; the
directory can be overridden with `FFETCH_FREEDOS_DIR`), boots them,
and checks the generic lines, the absence of the Ferric line,
parseable JSON, and the `/SAVE` file. Smoke B needs a Ferric DOS
boot image, which this repo does not build — supply one with
`--image` (or `FFETCH_FERRIC_IMG`) and it checks the Ferric line
and, with `--phase2`, the Modules line; without an image it prints
that it is skipped, and why. QEMU is a smoke screen, not an oracle:
these runs prove probing and degradation behave in emulation,
nothing about real hardware.

## Licence

GPL-2.0; see `LICENSE`. The FreeDOS components staged by the
Dockerfile (kernel, FreeCOM, boot sectors) are upstream FreeDOS
artifacts, pinned by version and SHA-256, and are not part of this
repo.
