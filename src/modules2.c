/*
 * FFETCH probes, tier 2: the generic hardware interfaces.
 *
 * None of these are Ferric-specific. VBE, the PCI BIOS, SMBIOS, APM
 * and the packet-driver specification are all published DOS-era
 * contracts that any install can answer, so this file never touches
 * a Ferric API; that separation is what keeps tier 2 honest on a
 * plain MS-DOS machine.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <dos.h>
#include <i86.h>
#include "ffetch.h"

/* From cpu.asm: the PCI BIOS returns its answers in 32-bit registers,
 * which the 16-bit C interrupt wrappers cannot move, so those two
 * calls are made in assembly. Both are gated on the CPU ladder -
 * they contain 386 instructions, and a PCI machine is never older. */
unsigned short __cdecl cpu_family_ladder(void);
unsigned long __cdecl pci_bios_present(unsigned int *ok, unsigned char *last_bus);
unsigned long __cdecl pci_bios_read(unsigned int bus_devfn, unsigned int off, unsigned int *ok);

static unsigned char peekb2(unsigned int seg, unsigned int off)
{
    return *(unsigned char __far *)MK_FP(seg, off);
}
static unsigned int peekw2(unsigned int seg, unsigned int off)
{
    return *(unsigned int __far *)MK_FP(seg, off);
}

/* ---------------------------------------------------------------- */
/* PCI naming                                                        */
/* ---------------------------------------------------------------- */

/* A deliberately small ID table: it covers the vendors and devices a
 * DOS-era machine plausibly contains, and anything outside it falls
 * back to the class name plus raw IDs, which is still information -
 * inventing a product name for an unknown ID would not be. */
struct pci_name { unsigned int vid, did; const char *name; };
static const struct pci_name pci_names[] = {
    { 0x8086, 0x1237, "Intel 82441FX PMC" },
    { 0x8086, 0x7000, "Intel 82371SB PIIX3 ISA" },
    { 0x8086, 0x7010, "Intel 82371SB PIIX3 IDE" },
    { 0x8086, 0x7020, "Intel 82371SB PIIX3 USB" },
    { 0x8086, 0x7110, "Intel 82371AB PIIX4 ISA" },
    { 0x8086, 0x7111, "Intel 82371AB PIIX4 IDE" },
    { 0x8086, 0x7112, "Intel 82371AB PIIX4 USB" },
    { 0x8086, 0x7113, "Intel 82371AB PIIX4 ACPI" },
    { 0x8086, 0x1209, "Intel 82559 Fast Ethernet" },
    { 0x8086, 0x1229, "Intel 82557/8/9 Fast Ethernet" },
    { 0x8086, 0x2448, "Intel 82801 PCI Bridge" },
    { 0x8086, 0x7190, "Intel 82443BX Host Bridge" },
    { 0x8086, 0x7191, "Intel 82443BX AGP Bridge" },
    { 0x1023, 0x8500, "Trident CyberBlade" },
    { 0x1023, 0x8820, "Trident CyberBlade XP" },
    { 0x1023, 0x8620, "Trident CyberBlade i1" },
    { 0x1002, 0x4C4D, "ATI Rage Mobility" },
    { 0x1002, 0x4742, "ATI Rage Pro" },
    { 0x10DE, 0x0028, "NVIDIA RIVA TNT2" },
    { 0x10DE, 0x0100, "NVIDIA GeForce 256" },
    { 0x1033, 0x0035, "NEC USB OHCI" },
    { 0x1033, 0x00E0, "NEC USB EHCI" },
    { 0x1039, 0x6300, "SiS 630 Host" },
    { 0x1039, 0x0008, "SiS PCI-ISA Bridge" },
    { 0x10B9, 0x1644, "ALi M1644 Host Bridge" },
    { 0x10B9, 0x1533, "ALi M1533 ISA Bridge" },
    { 0x10B9, 0x1535, "ALi M1535 ISA Bridge" },
    { 0x10B9, 0x5229, "ALi M5229 IDE" },
    { 0x10B9, 0x5237, "ALi M5237 USB OHCI" },
    { 0x10B9, 0x5451, "ALi M5451 Audio" },
    { 0x168C, 0x0013, "Atheros AR5212 Wi-Fi" },
    { 0x168C, 0x001A, "Atheros AR2413 Wi-Fi" },
    { 0x106B, 0x003F, "Apple USB OHCI" },
    { 0x1234, 0x1111, "QEMU VGA" },
    { 0x80EE, 0xBEEF, "VirtualBox VGA" },
    { 0x80EE, 0xCAFE, "VirtualBox Guest" },
    { 0x15AD, 0x0405, "VMware VGA" },
    { 0x5333, 0x5631, "S3 Trio64" },
    { 0x5333, 0x8A01, "S3 ViRGE" },
    { 0x1013, 0x00B8, "Cirrus Logic GD5446" },
    { 0x1013, 0x00A0, "Cirrus Logic GD5430" },
    { 0, 0, NULL }
};

static const char *pci_class_name(unsigned char cls)
{
    switch (cls) {
    case 0x00: return "legacy device";
    case 0x01: return "storage controller";
    case 0x02: return "network controller";
    case 0x03: return "display controller";
    case 0x04: return "multimedia controller";
    case 0x05: return "memory controller";
    case 0x06: return "bridge";
    case 0x07: return "serial controller";
    case 0x08: return "system peripheral";
    case 0x09: return "input device";
    case 0x0A: return "docking station";
    case 0x0B: return "processor";
    case 0x0C: return "serial bus controller";
    case 0x0D: return "wireless controller";
    case 0x0F: return "satellite controller";
    default:   return "device";
    }
}

/* PCI BIOS presence, proven by the installation check: AX=B101h
 * returns EDX=' PCI', CL=last PCI bus number, and carry clear when
 * a PCI BIOS exists. Result is cached so repeat probes don't re-enter
 * INT 1Ah. */
static int pci_probed = 0;
static int pci_present = 0;
static unsigned char pci_last_bus = 0;

static int pci_bios_ok(void)
{
    if (!pci_probed) {
        unsigned int ok = 0;
        unsigned char last = 0;
        pci_probed = 1;
        if (cpu_family_ladder() >= 3) {
            unsigned long sig = pci_bios_present(&ok, &last);
            if (ok && sig == 0x20494350UL) {    /* ' PCI' */
                pci_present = 1;
                pci_last_bus = last;
            }
        }
    }
    return pci_present;
}

static unsigned long pci_read(unsigned char bus, unsigned char devfn, unsigned char off)
{
    unsigned int ok = 0;
    unsigned long v;
    /* Use the dword read (B10Ah) and shift: one code path for all
     * offsets avoids the byte/word variants' alignment rules. */
    v = pci_bios_read(((unsigned int)bus << 8) | devfn, off & 0xFC, &ok);
    if (!ok) return 0xFFFFFFFFUL;
    return v >> ((off & 3) * 8);
}

/* Find the first device of a given class. Returns 1 and fills the
 * IDs when found. Bounded by the last bus reported by INT 1Ah B101h. */
static int pci_find_class(unsigned char want_cls, unsigned int *vid, unsigned int *did,
                          unsigned char *cls_out, unsigned char *sub_out)
{
    unsigned int bus;
    if (!pci_bios_ok()) return 0;
    for (bus = 0; bus <= (unsigned int)pci_last_bus; bus++) {
        unsigned int slot;
        for (slot = 0; slot < 32; slot++) {
            unsigned int fn;
            for (fn = 0; fn < 8; fn++) {
                unsigned char devfn = (unsigned char)((slot << 3) | fn);
                unsigned long id = pci_read((unsigned char)bus, devfn, 0);
                unsigned int v = (unsigned int)(id & 0xFFFF);
                unsigned long classreg;
                unsigned char cls, sub;
                if (v == 0xFFFF || v == 0) {
                    if (fn == 0) break;     /* no device in this slot */
                    continue;
                }
                classreg = pci_read((unsigned char)bus, devfn, 8);
                cls = (unsigned char)(classreg >> 24);
                sub = (unsigned char)(classreg >> 16);
                if (cls == want_cls) {
                    *vid = v;
                    *did = (unsigned int)(id >> 16);
                    *cls_out = cls;
                    *sub_out = sub;
                    return 1;
                }
                if (fn == 0) {
                    /* Multi-function devices declare it in the
                     * header type; single-function slots have no
                     * functions 1-7 worth probing. */
                    unsigned long hdr = pci_read((unsigned char)bus, devfn, 0x0C);
                    if ((hdr & 0x800000UL) == 0) break;
                }
            }
        }
    }
    return 0;
}

static const char *pci_lookup(unsigned int vid, unsigned int did)
{
    int i;
    for (i = 0; pci_names[i].name; i++)
        if (pci_names[i].vid == vid && pci_names[i].did == did) return pci_names[i].name;
    return NULL;
}

/* ---------------------------------------------------------------- */
/* GPU / Display via VBE, with the PCI name when both answer         */
/* ---------------------------------------------------------------- */

static unsigned char vbe_info[512];
static unsigned char vbe_mode[256];
static int vbe_ok = 0;
static unsigned int vbe_version = 0;
static unsigned int vbe_mem_64k = 0;
static char vbe_oem[64];

static void vbe_probe(void)
{
    union REGS r;
    struct SREGS s;
    if (vbe_ok) return;
    segread(&s);
    memset(vbe_info, 0, sizeof(vbe_info));
    memcpy(vbe_info, "VBE2", 4);    /* ask for the extended block */
    r.x.ax = 0x4F00;
    r.x.di = (unsigned int)vbe_info;
    s.es = s.ds;
    int86x(0x10, &r, &r, &s);
    if (r.x.ax != 0x004F) return;
    vbe_ok = 1;
    vbe_version = *(unsigned int *)(vbe_info + 4);
    vbe_mem_64k = *(unsigned int *)(vbe_info + 18);
    {
        unsigned int oseg = *(unsigned int *)(vbe_info + 8);
        unsigned int ooff = *(unsigned int *)(vbe_info + 6);
        int i = 0;
        if (oseg) {
            while (i < 63) {
                char c = (char)peekb2(oseg, ooff + i);
                if (!c) break;
                vbe_oem[i] = c;
                i++;
            }
        }
        vbe_oem[i] = 0;
    }
}

void mod_gpu(void)
{
    char buf[FF_VAL_MAX];
    char mem_s[16];
    unsigned int vid = 0, did = 0;
    unsigned char cls = 0, sub = 0;
    const char *name = NULL;
    int have_pci;
    vbe_probe();
    have_pci = pci_find_class(0x03, &vid, &did, &cls, &sub);
    if (have_pci) name = pci_lookup(vid, did);
    if (name) {
        if (vbe_ok) {
            ff_fmt_kb(mem_s, (unsigned long)vbe_mem_64k * 64UL);
            snprintf(buf, sizeof(buf), "%s, VBE %u.%u, %s VRAM", name, vbe_version >> 8, vbe_version & 0xFF, mem_s);
        } else {
            snprintf(buf, sizeof(buf), "%s", name);
        }
        ff_add("GPU", buf);
        return;
    }
    if (vbe_ok) {
        ff_fmt_kb(mem_s, (unsigned long)vbe_mem_64k * 64UL);
        if (vbe_oem[0])
            snprintf(buf, sizeof(buf), "%s, VBE %u.%u, %s VRAM", vbe_oem, vbe_version >> 8, vbe_version & 0xFF, mem_s);
        else
            snprintf(buf, sizeof(buf), "VBE %u.%u adapter, %s VRAM", vbe_version >> 8, vbe_version & 0xFF, mem_s);
        ff_add("GPU", buf);
        return;
    }
    if (have_pci) {
        snprintf(buf, sizeof(buf), "%s %04X:%04X", pci_class_name(cls), vid, did);
        ff_add("GPU", buf);
    }
}

void mod_display(void)
{
    union REGS r;
    struct SREGS s;
    char buf[FF_VAL_MAX];
    vbe_probe();
    if (vbe_ok) {
        unsigned int mode;
        r.x.ax = 0x4F03;
        int86(0x10, &r, &r);
        if (r.x.ax == 0x004F) {
            mode = r.x.bx & 0x3FFF;
            segread(&s);
            memset(vbe_mode, 0, sizeof(vbe_mode));
            r.x.ax = 0x4F01;
            r.x.cx = mode;
            r.x.di = (unsigned int)vbe_mode;
            s.es = s.ds;
            int86x(0x10, &r, &r, &s);
            if (r.x.ax == 0x004F) {
                unsigned int x = *(unsigned int *)(vbe_mode + 18);
                unsigned int y = *(unsigned int *)(vbe_mode + 20);
                unsigned int bpp = vbe_mode[25];
                if (x && y) {
                    sprintf(buf, "%ux%u, %u bpp (VESA mode %03Xh)", x, y, bpp, mode);
                    ff_add("Display", buf);
                    return;
                }
            }
        }
    }
    /* Text-mode fallback: the current mode's geometry is in the
     * BIOS data area, placed there by the adapter BIOS itself. */
    r.h.ah = 0x0F;
    int86(0x10, &r, &r);
    {
        unsigned int cols = r.h.ah;
        unsigned int rows = peekb2(0x40, 0x84) + 1;
        if (cols == 0) return;
        if (rows < 20 || rows > 100) rows = 25;
        sprintf(buf, "Text %ux%u (mode %02Xh)", cols, rows, r.h.al);
        ff_add("Display", buf);
    }
}

/* ---------------------------------------------------------------- */
/* BIOS strings and SMBIOS                                           */
/* ---------------------------------------------------------------- */

static int smbios_locate(void);
static unsigned char smb_peek(unsigned long addr);
static void smb_string(unsigned long str_area, unsigned int idx, char *out, size_t cap);
static unsigned long smb_find(unsigned char type, int which);

static int rom_date_at(unsigned int off, char *out)
{
    unsigned char b[10];
    int k, mon, day, n = 8;
    for (k = 0; k < 8; k++) b[k] = peekb2(0xF000, off + k);
    if (!(b[0] >= '0' && b[0] <= '1' && b[1] >= '0' && b[1] <= '9' && b[2] == '/' &&
          b[3] >= '0' && b[3] <= '3' && b[4] >= '0' && b[4] <= '9' && b[5] == '/' &&
          b[6] >= '0' && b[6] <= '9' && b[7] >= '0' && b[7] <= '9'))
        return 0;
    mon = (b[0] - '0') * 10 + (b[1] - '0');
    day = (b[3] - '0') * 10 + (b[4] - '0');
    if (mon < 1 || mon > 12 || day < 1 || day > 31) return 0;
    if (off <= 0xFFF5U &&
        peekb2(0xF000, off + 8) >= '0' && peekb2(0xF000, off + 8) <= '9' &&
        peekb2(0xF000, off + 9) >= '0' && peekb2(0xF000, off + 9) <= '9')
        n = 10;
    for (k = 0; k < n; k++) out[k] = (char)peekb2(0xF000, off + k);
    out[n] = 0;
    return 1;
}

void mod_bios(void)
{
    /* SMBIOS Type 0 is the authoritative source when the firmware
     * publishes one: vendor, version and release date are proper
     * strings there, not patterns fished out of ROM bytes. */
    if (smbios_locate()) {
        unsigned long t0 = smb_find(0, 0);
        if (t0 && smb_peek(t0 + 1) >= 9) {
            unsigned long strings = t0 + smb_peek(t0 + 1);
            char ven[40], ver[32], dat[16], buf[FF_VAL_MAX];
            smb_string(strings, smb_peek(t0 + 4), ven, sizeof(ven));
            smb_string(strings, smb_peek(t0 + 5), ver, sizeof(ver));
            smb_string(strings, smb_peek(t0 + 8), dat, sizeof(dat));
            if (ven[0] || ver[0] || dat[0]) {
                buf[0] = 0;
                if (ven[0]) { snprintf(buf, sizeof(buf), "%s BIOS", ven); }
                if (ver[0]) {
                    size_t l = strlen(buf);
                    snprintf(buf + l, sizeof(buf) - l, "%s%s", l ? " " : "", ver);
                }
                if (dat[0]) {
                    size_t l = strlen(buf);
                    snprintf(buf + l, sizeof(buf) - l, "%s%s", l ? ", " : "", dat);
                }
                ff_add("BIOS", buf);
                return;
            }
        }
    }
    /* Pre-SMBIOS fallback: check the standard IBM PC BIOS release date
     * at F000:FFF5 first, then scan F000 for an MM/DD/YY(YY) pattern.
     * Vendor names are recognised only at non-letter boundaries - a bare
     * substring search once "found" a two-letter vendor inside unrelated
     * ROM bytes and printed it as fact. */
    {
        static const char *vendors[] = { "SeaBIOS", "Award", "Phoenix", "American Megatrends", "AMI", "IBM", "Dell", "Toshiba", "Compaq", NULL };
        char date[12];
        const char *vendor = NULL;
        unsigned int off;
        int i;
        date[0] = 0;
        if (!rom_date_at(0xFFF5, date)) {
            for (off = 0; off < 0xFFF0 && !date[0]; off++) {
                if (peekb2(0xF000, off) >= '0' && peekb2(0xF000, off) <= '1')
                    rom_date_at(off, date);
            }
        }
        for (i = 0; vendors[i] && !vendor; i++) {
            size_t vl = strlen(vendors[i]);
            for (off = 0x0000; off < 0xFFF0 - (unsigned int)vl; off += 1) {
                size_t k;
                int match = 1;
                unsigned char prev, next;
                for (k = 0; k < vl; k++)
                    if (peekb2(0xF000, off + k) != (unsigned char)vendors[i][k]) { match = 0; break; }
                if (!match) continue;
                prev = off ? peekb2(0xF000, off - 1) : 0;
                next = peekb2(0xF000, off + (unsigned int)vl);
                if ((prev >= 'A' && prev <= 'Z') || (prev >= 'a' && prev <= 'z') ||
                    (next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z'))
                    continue;   /* inside a longer word: not a vendor stamp */
                vendor = vendors[i];
                break;
            }
        }
        if (date[0] || vendor) {
            char buf[FF_VAL_MAX];
            if (vendor && date[0]) snprintf(buf, sizeof(buf), "%s BIOS, %s", vendor, date);
            else if (vendor) snprintf(buf, sizeof(buf), "%s BIOS", vendor);
            else snprintf(buf, sizeof(buf), "BIOS dated %s", date);
            ff_add("BIOS", buf);
        }
    }
}

/* SMBIOS: the _SM_ anchor sits on a 16-byte boundary between
 * F0000h and FFFFFh. Structures are walked by their length fields
 * with a string set after each; every read is bounds-checked against
 * the table length the anchor declares, and tables above 1 MiB are
 * rejected cleanly since real mode cannot address them without wrapping. */
static int smbios_probed = 0;
static unsigned long smbios_addr = 0;
static unsigned int smbios_len = 0;

static int smbios_locate(void)
{
    unsigned int off;
    if (smbios_probed) return smbios_addr != 0;
    smbios_probed = 1;
    for (off = 0; off <= 0xFFE0; off += 16) {
        if (peekb2(0xF000, off) == '_' && peekb2(0xF000, off + 1) == 'S' &&
            peekb2(0xF000, off + 2) == 'M' && peekb2(0xF000, off + 3) == '_') {
            unsigned int ep_len = peekb2(0xF000, off + 5);
            unsigned char csum = 0;
            unsigned int i, tlen;
            unsigned long taddr;
            if (ep_len < 0x1F || (unsigned long)off + (unsigned long)ep_len > 0x10000UL)
                continue;
            for (i = 0; i < ep_len; i++) csum = (unsigned char)(csum + peekb2(0xF000, off + i));
            if (csum != 0) continue;
            if (peekb2(0xF000, off + 0x10) != '_' || peekb2(0xF000, off + 0x11) != 'D' ||
                peekb2(0xF000, off + 0x12) != 'M' || peekb2(0xF000, off + 0x13) != 'I' ||
                peekb2(0xF000, off + 0x14) != '_')
                continue;
            tlen = peekw2(0xF000, off + 0x16);
            taddr = peekw2(0xF000, off + 0x18) | ((unsigned long)peekw2(0xF000, off + 0x1A) << 16);
            /* Real-mode addressing stops at 1 MiB; reject tables in high memory. */
            if (taddr == 0 || tlen == 0 || taddr >= 0x100000UL || taddr + tlen > 0x100000UL)
                continue;
            smbios_len = tlen;
            smbios_addr = taddr;
            return 1;
        }
        if (off == 0xFFE0) break;
    }
    return 0;
}

static unsigned char smb_peek(unsigned long addr)
{
    unsigned int seg = (unsigned int)(addr >> 4);
    unsigned int off = (unsigned int)(addr & 0xF);
    return peekb2(seg, off);
}

/* Copy string `idx` of the structure whose formatted part starts at
 * `str_area` (the byte right after the formatted section), bounded
 * by the SMBIOS table end. */
static void smb_string(unsigned long str_area, unsigned int idx, char *out, size_t cap)
{
    unsigned long p = str_area;
    unsigned long end = smbios_addr + smbios_len;
    unsigned int cur = 1;
    size_t n = 0;
    out[0] = 0;
    if (idx == 0 || p >= end) return;
    while (cur < idx) {
        while (p < end && smb_peek(p)) p++;
        p++;
        if (p >= end || smb_peek(p) == 0) return;   /* ran off the set */
        cur++;
    }
    while (p < end && smb_peek(p) && n < cap - 1) out[n++] = (char)smb_peek(p++);
    out[n] = 0;
}

/* Walk to the structure of `type`; returns its table offset or 0.
 * `which` selects the nth structure of that type (0-based). */
static unsigned long smb_find(unsigned char type, int which)
{
    unsigned long p = smbios_addr, end = smbios_addr + smbios_len;
    int seen = 0;
    if (!smbios_locate()) return 0;
    while (p + 4 <= end) {
        unsigned char t = smb_peek(p);
        unsigned int len = smb_peek(p + 1);
        unsigned long strings;
        if (len < 4 || p + len > end) return 0;
        strings = p + len;
        if (t == 127) return 0;
        if (t == type) {
            if (seen == which) return p;
            seen++;
        }
        /* skip the string set: it ends with two consecutive NULs */
        while (strings + 1 < end && !(smb_peek(strings) == 0 && smb_peek(strings + 1) == 0)) strings++;
        p = strings + 2;
    }
    return 0;
}

void mod_host(void)
{
    unsigned long t1;
    char mfr[48], prod[48], buf[FF_VAL_MAX];
    if (!smbios_locate()) return;
    t1 = smb_find(1, 0);
    if (!t1 || smb_peek(t1 + 1) < 6) return;
    smb_string(t1 + smb_peek(t1 + 1), smb_peek(t1 + 4), mfr, sizeof(mfr));
    smb_string(t1 + smb_peek(t1 + 1), smb_peek(t1 + 5), prod, sizeof(prod));
    if (!mfr[0] && !prod[0]) return;
    if (mfr[0] && prod[0]) snprintf(buf, sizeof(buf), "%s %s", mfr, prod);
    else snprintf(buf, sizeof(buf), "%s", mfr[0] ? mfr : prod);
    ff_add("Host", buf);
}

void mod_board(void)
{
    unsigned long t2;
    char prod[48], mfr[48], buf[FF_VAL_MAX];
    if (!smbios_locate()) return;
    t2 = smb_find(2, 0);
    if (!t2 || smb_peek(t2 + 1) < 6) return;
    smb_string(t2 + smb_peek(t2 + 1), smb_peek(t2 + 4), mfr, sizeof(mfr));
    smb_string(t2 + smb_peek(t2 + 1), smb_peek(t2 + 5), prod, sizeof(prod));
    if (!prod[0] && !mfr[0]) return;
    if (mfr[0] && prod[0]) snprintf(buf, sizeof(buf), "%s %s", mfr, prod);
    else snprintf(buf, sizeof(buf), "%s", prod[0] ? prod : mfr);
    ff_add("Board", buf);
}

void mod_physmem(void)
{
    /* Type 17 structures are one per memory device; the size field
     * is in MB (bit 15 set means the value is in KB; 0xFFFF means
     * unknown; 0x7FFF defers to the 32-bit Extended Size at +0x1C). */
    unsigned long total_kb = 0;
    int i, found = 0;
    char buf[FF_VAL_MAX], s[16];
    if (!smbios_locate()) return;
    for (i = 0; i < 16; i++) {
        unsigned long t = smb_find(17, i);
        unsigned int slen, size;
        if (!t) break;
        slen = smb_peek(t + 1);
        if (slen < 0x0E) continue;
        size = smb_peek(t + 0x0C) | ((unsigned int)smb_peek(t + 0x0D) << 8);
        if (size == 0 || size == 0xFFFF) continue;
        if (size == 0x7FFF) {
            if (slen >= 0x20) {
                unsigned long ext_mb =
                    (unsigned long)smb_peek(t + 0x1C) |
                    ((unsigned long)smb_peek(t + 0x1D) << 8) |
                    ((unsigned long)smb_peek(t + 0x1E) << 16) |
                    ((unsigned long)(smb_peek(t + 0x1F) & 0x7F) << 24);
                if (ext_mb > 0 && ext_mb <= 0x003FFFFFUL) {
                    total_kb += ext_mb * 1024UL;
                    found = 1;
                }
            }
            continue;
        }
        found = 1;
        if (size & 0x8000) total_kb += (size & 0x7FFF);
        else total_kb += (unsigned long)size * 1024UL;
    }
    if (!found || total_kb == 0) return;
    ff_fmt_kb(s, total_kb);
    snprintf(buf, sizeof(buf), "%s installed (SMBIOS)", s);
    ff_add("Physical Memory", buf);
}

/* ---------------------------------------------------------------- */
/* APM battery and power                                             */
/* ---------------------------------------------------------------- */

/* APM is interrogated exactly the way APMSTAT does: installation
 * check, real-mode connect, one read-only status query, disconnect.
 * The connect matters - some BIOSes refuse status calls without it -
 * and the disconnect returns the BIOS to its previous state. */
static int apm_status(unsigned char *ac, unsigned char *batt, unsigned char *life)
{
    union REGS r;
    r.x.ax = 0x5300;
    r.x.bx = 0;
    r.x.cx = 0;
    int86(0x15, &r, &r);
    if (r.x.cflag) return 0;
    if (r.x.bx != 0x504D) return 0;     /* 'PM' signature */
    r.x.ax = 0x5301;
    r.x.bx = 0;
    int86(0x15, &r, &r);
    if (r.x.cflag) return 0;
    r.x.ax = 0x530A;
    r.x.bx = 0x0001;                    /* all devices */
    int86(0x15, &r, &r);
    if (r.x.cflag) {
        union REGS d;
        d.x.ax = 0x5304;
        d.x.bx = 0;
        int86(0x15, &d, &d);
        return 0;
    }
    *ac = r.h.bh;
    *batt = r.h.bl;
    *life = r.h.cl;
    r.x.ax = 0x5304;
    r.x.bx = 0;
    int86(0x15, &r, &r);
    return 1;
}

static int apm_done = 0, apm_ok = 0;
static unsigned char apm_ac, apm_batt, apm_life;

static void apm_probe(void)
{
    if (apm_done) return;
    apm_done = 1;
    apm_ok = apm_status(&apm_ac, &apm_batt, &apm_life);
}

void mod_battery(void)
{
    char buf[FF_VAL_MAX];
    const char *state;
    apm_probe();
    if (!apm_ok) return;
    if (apm_batt == 0xFF) return;       /* no battery present */
    switch (apm_batt) {
    case 0: state = "high"; break;
    case 1: state = "low"; break;
    case 2: state = "critical"; break;
    case 3: state = "charging"; break;
    default: state = "unknown"; break;
    }
    if (apm_life <= 100) sprintf(buf, "%u%% (%s)", apm_life, state);
    else sprintf(buf, "Present (%s)", state);
    ff_add("Battery", buf);
}

void mod_power(void)
{
    apm_probe();
    if (!apm_ok) return;
    if (apm_ac == 0) ff_add("Power", "Battery (AC off-line)");
    else if (apm_ac == 1) ff_add("Power", "AC on-line");
    /* AC status 2 (backup) and FFh (unknown) say nothing definite,
     * so they stay unprinted rather than guessed at. */
}

/* ---------------------------------------------------------------- */
/* Sound                                                             */
/* ---------------------------------------------------------------- */

void mod_sound(void)
{
    char buf[FF_VAL_MAX];
    const char *blaster = ff_getenv("BLASTER");
    int sb_found = 0, adlib_found = 0;
    unsigned int base = 0x220;
    /* The DSP reset handshake is the documented presence test: write
     * 1 then 0 to the reset port, then the read-data port must yield
     * AAh within a bounded poll. The bound matters: on a machine
     * with no card the port floats and the byte never comes. */
    if (blaster) {
        const char *p = blaster;
        while (*p) {
            if ((*p == 'A' || *p == 'a') && p[1]) {
                unsigned int v = 0;
                p++;
                /* BLASTER addresses are written in hex (A220). */
                while ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F') || (*p >= 'a' && *p <= 'f')) {
                    unsigned int dgt = (*p <= '9') ? (unsigned int)(*p - '0') : (unsigned int)((*p & 0xDF) - 'A' + 10);
                    v = v * 16 + dgt;
                    p++;
                }
                if (v >= 0x200 && v <= 0x280) base = v;
                continue;
            }
            p++;
        }
    }
    {
        unsigned int i;
        unsigned char v = 0;
        outp(base + 0x6, 1);
        for (i = 0; i < 100; i++) (void)inp(base + 0x6);
        outp(base + 0x6, 0);
        for (i = 0; i < 1000; i++) {
            if (inp(base + 0xE) & 0x80) { v = (unsigned char)inp(base + 0xA); break; }
        }
        if (v == 0xAA) sb_found = 1;
    }
    /* AdLib detection uses its two timers: program timer 1 to fire,
     * and the status register must show both the flag and the mask
     * pattern. It is the test every tracker of the era used, and it
     * is read-back based, so a missing card cannot fake it. Skip it
     * when a Sound Blaster DSP already answered. */
    if (!sb_found) {
        unsigned char s1, s2;
        outp(0x388, 0x04); outp(0x389, 0x60);   /* reset timers */
        outp(0x388, 0x04); outp(0x389, 0x80);   /* enable IRQ flag */
        s1 = (unsigned char)inp(0x388) & 0xE0;
        outp(0x388, 0x02); outp(0x389, 0xFF);   /* timer 1 value */
        outp(0x388, 0x04); outp(0x389, 0x21);   /* start timer 1 */
        {
            unsigned int i;
            for (i = 0; i < 2000; i++) (void)inp(0x388);
        }
        s2 = (unsigned char)inp(0x388) & 0xE0;
        outp(0x388, 0x04); outp(0x389, 0x60);
        outp(0x388, 0x04); outp(0x389, 0x80);
        if (s1 == 0x00 && s2 == 0xC0) adlib_found = 1;
    }
    if (sb_found) {
        snprintf(buf, sizeof(buf), "Sound Blaster DSP at %03Xh%s", base, blaster ? " (BLASTER set)" : "");
        ff_add("Sound", buf);
    } else if (adlib_found) {
        ff_add("Sound", "AdLib (OPL2) at 388h");
    } else if (blaster) {
        /* The environment claims a card the DSP probe could not
         * confirm; report the claim as a claim, not as hardware. */
        snprintf(buf, sizeof(buf), "BLASTER=%s (DSP unconfirmed)", blaster);
        ff_add("Sound", buf);
    }
}

/* ---------------------------------------------------------------- */
/* Terminal                                                          */
/* ---------------------------------------------------------------- */

void mod_terminal(void)
{
    union REGS r;
    char buf[FF_VAL_MAX];
    unsigned int cols, rows, font;
    r.h.ah = 0x0F;
    int86(0x10, &r, &r);
    cols = r.h.ah;
    rows = peekb2(0x40, 0x84) + 1;
    if (rows < 20 || rows > 100) rows = 25;
    /* Font cell height via the ROM font information call (BH=06h
     * returns the 8x14 font's points in CX on VGA; the value that
     * matters is the current cell height from the BDA at 40:85,
     * which the same call's context maintains). */
    font = peekw2(0x40, 0x85);
    if (font == 0 || font > 32) {
        sprintf(buf, "%ux%u text", cols, rows);
    } else {
        sprintf(buf, "%ux%u text, %u-px font", cols, rows, font);
    }
    ff_add("Terminal", buf);
}

/* ---------------------------------------------------------------- */
/* Network: the packet-driver specification                         */
/* ---------------------------------------------------------------- */

void mod_network(void)
{
    /* A packet driver announces itself with the ASCII signature
     * "PKT DRVR" at offset 3 of its interrupt handler, somewhere in
     * INT 60h-7Fh. Finding it is the specification's own discovery
     * method. The MAC comes from the driver's get_address call; an
     * IP is only ever printed when a real stack config states one
     * (the mTCP configuration file, named by MTCPCFG). */
    int vec;
    for (vec = 0x60; vec <= 0x7F; vec++) {
        unsigned int hseg = peekw2(0, vec * 4 + 2);
        unsigned int hoff = peekw2(0, vec * 4);
        static const char sig[8] = { 'P','K','T',' ','D','R','V','R' };
        int i, match = 1;
        union REGS r;
        struct SREGS s;
        static unsigned char macbuf[16];
        char buf[FF_VAL_MAX];
        if (hseg == 0) continue;
        for (i = 0; i < 8; i++)
            if (peekb2(hseg, hoff + 3 + i) != (unsigned char)sig[i]) match = 0;
        if (!match) continue;
        /* driver_info: AH=01h, AL=FFh */
        r.h.ah = 0x01;
        r.h.al = 0xFF;
        int86(vec, &r, &r);
        if (r.x.cflag) continue;
        /* get_address: AH=06h, handle 0, ES:DI buffer, CX=len */
        segread(&s);
        memset(macbuf, 0, sizeof(macbuf));
        r.h.ah = 0x06;
        r.x.bx = 0;
        r.x.cx = 6;
        r.x.di = (unsigned int)macbuf;
        s.es = s.ds;
        int86x(vec, &r, &r, &s);
        if (r.x.cflag) {
            sprintf(buf, "Packet driver at INT %02Xh", vec);
            ff_add("Network", buf);
            return;
        }
        sprintf(buf, "Packet driver INT %02Xh, MAC %02X:%02X:%02X:%02X:%02X:%02X",
                vec, macbuf[0], macbuf[1], macbuf[2], macbuf[3], macbuf[4], macbuf[5]);
        ff_add("Network", buf);
        /* mTCP configuration: the stack used with packet drivers on
         * DOS names its config file in MTCPCFG; an IPADDR line in it
         * is the stack's own statement of its address. */
        {
            const char *cfgpath = ff_getenv("MTCPCFG");
            if (cfgpath && cfgpath[0]) {
                FILE *f = fopen(cfgpath, "r");
                if (f) {
                    char line[128];
                    while (fgets(line, sizeof(line), f)) {
                        if (strnicmp(line, "IPADDR", 6) == 0) {
                            char *p = line + 6;
                            char ip[20];
                            int n = 0;
                            while (*p == ' ' || *p == '\t') p++;
                            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && n < 19)
                                ip[n++] = *p++;
                            ip[n] = 0;
                            if (n >= 7) {
                                sprintf(buf, "%s", ip);
                                ff_add("Local IP", buf);
                            }
                            break;
                        }
                    }
                    fclose(f);
                }
            }
        }
        return;
    }
}
