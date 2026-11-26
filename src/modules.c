/*
 * FFETCH probes, tiers 1 and 2.
 *
 * Every function here follows the same contract: interrogate one
 * source, and add a line only if the source answered with data that
 * passed a sanity check. A missing XMS driver, an absent APM BIOS or
 * a machine with no VBE are all normal outcomes, not errors, and the
 * summary simply gets shorter on such machines.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <conio.h>
#include <dos.h>
#include <i86.h>
#include "ffetch.h"

/* From cpu.asm. The ladder is 8088-safe; cpu_cpuid and
 * cpu_has_cpuid are only reached after the ladder proves a
 * 486-class part, because both need 386 instructions. */
unsigned short __cdecl cpu_family_ladder(void);
unsigned short __cdecl cpu_has_cpuid(void);
void __cdecl cpu_cpuid(unsigned long leaf, unsigned long __far *regs);
int __cdecl xms_query_free_kb(unsigned int entry_off, unsigned int entry_seg,
                       unsigned int *total_kb, unsigned int *largest_kb);

/* From cpu.asm: an INT 24h handler that answers "Fail" to every
 * critical error. The disk probe borrows the critical-error vector
 * while it runs; see mod_disk for the failure this prevents. */
extern void __cdecl __far crit24_handler(void);

static unsigned int crit24_old_seg;
static unsigned int crit24_old_off;

static void crit24_install(void)
{
    union REGS r;
    struct SREGS s;
    r.h.ah = 0x35;
    r.h.al = 0x24;
    int86x(0x21, &r, &r, &s);
    crit24_old_seg = s.es;
    crit24_old_off = r.x.bx;
    r.h.ah = 0x25;
    r.h.al = 0x24;
    r.x.dx = FP_OFF(crit24_handler);
    s.ds = FP_SEG(crit24_handler);
    int86x(0x21, &r, &r, &s);
}

static void crit24_restore(void)
{
    union REGS r;
    struct SREGS s;
    r.h.ah = 0x25;
    r.h.al = 0x24;
    r.x.dx = crit24_old_off;
    s.ds = crit24_old_seg;
    int86x(0x21, &r, &r, &s);
}

static unsigned char peekb(unsigned int seg, unsigned int off)
{
    return *(unsigned char __far *)MK_FP(seg, off);
}
static unsigned int peekw(unsigned int seg, unsigned int off)
{
    return *(unsigned int __far *)MK_FP(seg, off);
}
static unsigned long peekd(unsigned int seg, unsigned int off)
{
    return *(unsigned long __far *)MK_FP(seg, off);
}

/* ---------------------------------------------------------------- */
/* OS and kernel                                                     */
/* ---------------------------------------------------------------- */

/* DOS version facts gathered once, shared by the OS and Kernel
 * lines so the two can never contradict each other. */
static int dos_facts_done = 0;
static unsigned char dos_major, dos_minor, dos_oem, dos_true_major, dos_true_minor;
static unsigned int dos_freedos_build = 0;

static void dos_facts(void)
{
    union REGS r;
    if (dos_facts_done) return;
    dos_facts_done = 1;
    r.x.ax = 0x3000;
    int86(0x21, &r, &r);
    dos_major = r.h.al;
    dos_minor = r.h.ah;
    dos_oem = r.h.bh;
    r.x.ax = 0x3306;
    int86(0x21, &r, &r);
    dos_true_major = r.h.bl;
    dos_true_minor = r.h.bh;
    if (dos_oem == 0xFD) {
        /* The FreeDOS kernel reports its build number through
         * AX=33FFh. Other kernels leave AX alone or return junk, so
         * the result is only trusted when the OEM byte already said
         * FreeDOS and the value looks like a build (1000-9999). */
        union REGS f;
        f.x.ax = 0x33FF;
        f.x.dx = 0;
        int86(0x21, &f, &f);
        if (f.x.ax >= 1000 && f.x.ax <= 9999) dos_freedos_build = f.x.ax;
    }
}

void mod_os(void)
{
    char buf[FF_VAL_MAX];
    const char *flavour;
    dos_facts();
    switch (dos_oem) {
    case 0xFD: flavour = "FreeDOS"; break;
    case 0xFF: flavour = "MS-DOS"; break;
    case 0x00: flavour = "PC-DOS"; break;
    case 0xEE: flavour = "DR-DOS"; break;
    default:   flavour = NULL; break;
    }
    if (flavour)
        sprintf(buf, "%s %u.%u", flavour, dos_major, dos_minor);
    else
        sprintf(buf, "DOS %u.%u (OEM %02Xh)", dos_major, dos_minor, dos_oem);
    ff_add("OS", buf);
}

void mod_kernel(void)
{
    char buf[FF_VAL_MAX];
    dos_facts();
    if (dos_freedos_build)
        sprintf(buf, "FreeDOS kernel build %u (DOS %u.%u)", dos_freedos_build, dos_true_major, dos_true_minor);
    else
        sprintf(buf, "DOS kernel %u.%u", dos_true_major, dos_true_minor);
    ff_add("Kernel", buf);
}

/* ---------------------------------------------------------------- */
/* CPU                                                               */
/* ---------------------------------------------------------------- */

void mod_cpu(void)
{
    char buf[FF_VAL_MAX];
    unsigned short fam = cpu_family_ladder();
    if (fam >= 4 && cpu_has_cpuid()) {
        unsigned long r[4];
        unsigned long max_leaf, max_ext;
        char vendor[13];
        char brand[49];
        unsigned long eax1;
        unsigned int efam, emod, f, m;
        int i, have_brand = 0;
        cpu_cpuid(0, r);
        max_leaf = r[0];
        memcpy(vendor + 0, &r[1], 4);
        memcpy(vendor + 4, &r[3], 4);
        memcpy(vendor + 8, &r[2], 4);
        vendor[12] = 0;
        cpu_cpuid(0x80000000UL, r);
        max_ext = r[0];
        brand[0] = 0;
        if (max_ext >= 0x80000004UL) {
            unsigned long b[12];
            cpu_cpuid(0x80000002UL, b + 0);
            cpu_cpuid(0x80000003UL, b + 4);
            cpu_cpuid(0x80000004UL, b + 8);
            memcpy(brand, b, 48);
            brand[48] = 0;
            /* Brand strings are space-padded; trim both ends so the
             * line does not carry the padding into the summary. */
            {
                char *s = brand;
                while (*s == ' ') s++;
                if (s != brand) memmove(brand, s, strlen(s) + 1);
                i = (int)strlen(brand);
                while (i > 0 && brand[i-1] == ' ') brand[--i] = 0;
            }
            have_brand = brand[0] != 0;
        }
        if (max_leaf >= 1) {
            cpu_cpuid(1, r);
            eax1 = r[0];
        } else eax1 = 0;
        f = (unsigned int)((eax1 >> 8) & 0xF);
        m = (unsigned int)((eax1 >> 4) & 0xF);
        efam = (unsigned int)((eax1 >> 20) & 0xFF);
        emod = (unsigned int)((eax1 >> 16) & 0xF);
        if (f == 0xF) f += efam;
        if (f == 0x6 || f == 0xF) m += (emod << 4);
        if (have_brand)
            sprintf(buf, "%s", brand);
        else
            sprintf(buf, "%s family %u model %u", vendor, f, m);
        ff_add("CPU", buf);
        return;
    }
    /* Pre-CPUID parts get named from the ladder alone. Anything more
     * specific (clock speed, cache) would be a guess on this path. */
    switch (fam) {
    case 0:  ff_add("CPU", "8088/8086 class"); break;
    case 2:  ff_add("CPU", "80286"); break;
    case 3:  ff_add("CPU", "80386"); break;
    default: ff_add("CPU", "80486 class (no CPUID)"); break;
    }
}

/* ---------------------------------------------------------------- */
/* Memory                                                            */
/* ---------------------------------------------------------------- */

void mod_memory(void)
{
    char buf[FF_VAL_MAX];
    char total_s[16], free_s[16], xms_s[16], ems_s[16];
    unsigned int conv_total_kb, conv_free_kb;
    unsigned long xms_kb = 0, ems_kb = 0;
    int have_xms = 0, have_ems = 0;
    union REGS r;

    /* INT 12h reports the conventional memory size the BIOS found;
     * it is the one figure every DOS machine can state. */
    int86(0x12, &r, &r);
    conv_total_kb = r.x.ax;
    /* Largest free block via an allocation probe that must fail: the
     * kernel answers AH=48h with BX set to the largest available
     * block when BX=FFFFh asks for more than exists. */
    r.h.ah = 0x48;
    r.x.bx = 0xFFFF;
    int86(0x21, &r, &r);
    conv_free_kb = (unsigned int)((unsigned long)r.x.bx * 16UL / 1024UL);

    /* XMS: the driver announces itself through INT 2Fh AX=4300h and
     * hands out its entry point through AX=4310h. The query itself
     * is a raw far call with register arguments, so it goes through
     * the assembly helper in cpu.asm. */
    r.x.ax = 0x4300;
    int86(0x2F, &r, &r);
    if (r.h.al == 0x80) {
        struct SREGS s;
        unsigned int total = 0, largest = 0;
        r.x.ax = 0x4310;
        int86x(0x2F, &r, &r, &s);
        if (xms_query_free_kb(r.x.bx, s.es, &total, &largest)) {
            xms_kb = total;
            have_xms = 1;
        }
    }

    /* EMS: an expanded-memory manager installs a device whose header
     * name is EMMXXXX0 at the INT 67h vector's segment. Checking the
     * name is the documented presence test and touches no hardware. */
    {
        unsigned int ivt_seg = peekw(0, 0x67 * 4 + 2);
        static const char emm_name[8] = { 'E','M','M','X','X','X','X','0' };
        int match = 1, i;
        if (ivt_seg != 0) {
            for (i = 0; i < 8; i++)
                if (peekb(ivt_seg, 0x0A + i) != (unsigned char)emm_name[i]) match = 0;
            if (match) {
                union REGS e;
                e.h.ah = 0x03;      /* total pages (16 KB each) */
                int86(0x67, &e, &e);
                if (e.h.ah == 0) {
                    ems_kb = (unsigned long)e.x.dx * 16UL;
                    have_ems = 1;
                }
            }
        }
    }

    ff_fmt_kb(total_s, conv_total_kb);
    ff_fmt_kb(free_s, conv_free_kb);
    sprintf(buf, "%s total, %s free (conventional)", total_s, free_s);
    ff_add("Memory", buf);
    if (have_xms) {
        ff_fmt_kb(xms_s, xms_kb);
        sprintf(buf, "XMS %s free", xms_s);
        ff_add("XMS", buf);
    }
    if (have_ems) {
        ff_fmt_kb(ems_s, ems_kb);
        sprintf(buf, "EMS %s total", ems_s);
        ff_add("EMS", buf);
    }
}

/* ---------------------------------------------------------------- */
/* Disks                                                             */
/* ---------------------------------------------------------------- */

void mod_disk(void)
{
    int drive;
    int shown = 0;
    /* Start at C: probing A:/B: can spin a floppy drive that has no
     * disk in it and stall the summary on real hardware; fastfetch
     * parity matters less than not hanging the machine. */
    /* The free-space call below raises a critical error on a letter
     * whose driver is loaded but whose media is not mounted, and the
     * default critical-error handler stops the machine on an
     * Abort/Retry/Fail prompt - the opposite of this tool's contract
     * that an unavailable source just shortens the summary. Borrow
     * the vector for the duration of the probe: the borrowed handler
     * answers "Fail", the call returns an error, and the letter is
     * skipped like any other absent source. */
    crit24_install();
    for (drive = 3; drive <= 26 && shown < 8; drive++) {
        union REGS r;
        char buf[FF_VAL_MAX];
        char tot_s[16], free_s[16];
        unsigned long sec_per_cluster, bytes_per_sec, total_clusters, free_clusters;
        unsigned long total_kb, free_kb;
        const char *fs;
        /* A letter DOS has never assigned fails the IOCTL
         * local-drive check with "invalid drive" (AX=000Fh) without
         * any media access, so most phantom letters are filtered
         * here for free. A letter that exists but has no media
         * mounted passes this check; the borrowed critical-error
         * handler above is what keeps its free-space call quiet. */
        r.x.ax = 0x4409;
        r.h.bl = (unsigned char)drive;
        int86(0x21, &r, &r);
        if (r.x.cflag && r.x.ax == 0x000F) continue;
        r.h.ah = 0x36;
        r.h.dl = (unsigned char)drive;
        int86(0x21, &r, &r);
        if (r.x.ax == 0xFFFF) continue;
        sec_per_cluster = r.x.ax;
        free_clusters = r.x.bx;
        bytes_per_sec = r.x.cx;
        total_clusters = r.x.dx;
        /* Work in sectors first: clusters x sectors-per-cluster
         * stays inside 32 bits for any real volume, and with the
         * near-universal 512-byte sector the KiB figure is exact. */
        total_kb = (total_clusters * sec_per_cluster) * (bytes_per_sec / 512UL) / 2UL;
        free_kb = (free_clusters * sec_per_cluster) * (bytes_per_sec / 512UL) / 2UL;
        /* A redirected drive is not a local FAT volume at all, so it
         * is labelled by its nature (IOCTL 4409h remote bit) rather
         * than by a FAT rule that cannot apply to it. */
        r.x.ax = 0x4409;
        r.h.bl = (unsigned char)drive;
        int86(0x21, &r, &r);
        if (!r.x.cflag && (r.x.dx & 0x1000)) {
            fs = "redirected";
        } else if (total_clusters < 4085UL) {
            /* FAT type follows the Microsoft determination rule,
             * which is a cluster-count test, not a guess: <4085
             * clusters is FAT12, <65525 is FAT16, else FAT32. */
            fs = "FAT12";
        } else if (total_clusters < 65525UL) {
            fs = "FAT16";
        } else {
            fs = "FAT32";
        }
        ff_fmt_kb(tot_s, total_kb);
        ff_fmt_kb(free_s, free_kb);
        sprintf(buf, "%c: %s total, %s free (%s)", 'A' + drive - 1, tot_s, free_s, fs);
        ff_add("Disk", buf);
        shown++;
    }
    crit24_restore();
}

void mod_physdisk(void)
{
    int d;
    for (d = 0; d < 4; d++) {
        union REGS r;
        struct SREGS s;
        static unsigned char edd[48];
        char buf[FF_VAL_MAX];
        char size_s[16];
        unsigned long sectors_lo, sectors_hi;
        unsigned int bps;
        /* EDD installation check first: AH=41h with the 55AAh
         * signature. A BIOS without EDD clears nothing and sets
         * carry, which is the whole probe. */
        r.h.ah = 0x41;
        r.x.bx = 0x55AA;
        r.h.dl = (unsigned char)(0x80 + d);
        int86(0x13, &r, &r);
        if (r.x.cflag || r.x.bx != 0xAA55) continue;
        segread(&s);
        memset(edd, 0, sizeof(edd));
        *(unsigned int *)edd = sizeof(edd);
        r.h.ah = 0x48;
        r.h.dl = (unsigned char)(0x80 + d);
        r.x.si = (unsigned int)edd;
        int86x(0x13, &r, &r, &s);
        if (r.x.cflag) continue;
        sectors_lo = *(unsigned long *)(edd + 16);
        sectors_hi = *(unsigned long *)(edd + 20);
        bps = *(unsigned int *)(edd + 24);
        if ((sectors_lo == 0 && sectors_hi == 0) || bps == 0) continue;
        /* Work in KiB (or GiB for >2 TiB disks) so drives >= 4 GiB
         * never overflow 32-bit byte counts. */
        if (sectors_hi == 0) {
            unsigned long kb;
            if (bps >= 512 && (bps % 512) == 0) {
                unsigned long mult = (unsigned long)(bps / 512);
                kb = (sectors_lo / 2UL) * mult + ((sectors_lo & 1UL) * mult) / 2UL;
            } else {
                kb = (sectors_lo / 1024UL) * (unsigned long)bps +
                     ((sectors_lo % 1024UL) * (unsigned long)bps) / 1024UL;
            }
            ff_fmt_kb(size_s, kb);
            snprintf(buf, sizeof(buf), "Drive %d: %s (%lu sectors x %u)", d, size_s, sectors_lo, bps);
        } else {
            unsigned long gb = (sectors_hi * 2048UL) + (sectors_lo >> 21);
            if (bps > 512) gb *= (unsigned long)(bps / 512);
            snprintf(buf, sizeof(buf), "Drive %d: %lu GB (%u B/sector)", d, gb, bps);
        }
        ff_add("Physical Disk", buf);
    }
}

/* ---------------------------------------------------------------- */
/* Session facts                                                     */
/* ---------------------------------------------------------------- */

void mod_uptime(void)
{
    /* The BIOS tick counter at 0040:006C runs at 18.2 Hz (ticks since
     * midnight; DOS has no monotonic boot-tick counter without a TSR)
     * and sets 0040:0070 on midnight rollover. Read the pair twice and
     * require both fields to match so neither a rollover nor a torn
     * 32-bit read across IRQ0 can corrupt the sample. */
    unsigned long ticks, ticks2;
    unsigned int days, days2;
    unsigned long seconds, d, h, m;
    char buf[FF_VAL_MAX];
    do {
        days = peekb(0x40, 0x70);
        ticks = peekd(0x40, 0x6C);
        days2 = peekb(0x40, 0x70);
        ticks2 = peekd(0x40, 0x6C);
    } while (days != days2 || ticks != ticks2);
    seconds = (ticks * 10UL) / 182UL;   /* 18.2 ticks per second */
    d = days + seconds / 86400UL;
    h = (seconds % 86400UL) / 3600UL;
    m = (seconds % 3600UL) / 60UL;
    if (d > 0) snprintf(buf, sizeof(buf), "%lud %luh %lum", d, h, m);
    else if (h > 0) snprintf(buf, sizeof(buf), "%luh %lum", h, m);
    else snprintf(buf, sizeof(buf), "%lum", m);
    ff_add("Uptime", buf);
}

void mod_datetime(void)
{
    union REGS r;
    char buf[FF_VAL_MAX];
    r.h.ah = 0x2A;
    int86(0x21, &r, &r);
    {
        unsigned int y = r.x.cx;
        unsigned int mo = r.h.dh, da = r.h.dl;
        r.h.ah = 0x2C;
        int86(0x21, &r, &r);
        snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u", y, mo, da, r.h.ch, r.h.cl, r.h.dh);
    }
    ff_add("DateTime", buf);
}

void mod_locale(void)
{
    static unsigned char info[64];
    union REGS r;
    struct SREGS s;
    unsigned int country, codepage;
    char buf[FF_VAL_MAX];
    segread(&s);
    memset(info, 0, sizeof(info));
    r.x.ax = 0x6501;
    r.x.bx = 0xFFFF;        /* current codepage */
    r.x.dx = 0xFFFF;        /* current country */
    r.x.cx = sizeof(info);
    r.x.di = (unsigned int)info;
    s.es = s.ds;
    int86x(0x21, &r, &r, &s);
    if (r.x.cflag || info[0] != 0x01) return;
    /* Extended country information layout: byte 0 is the info type (1),
     * then a size word, then the country ID and codepage words. */
    country = *(unsigned int *)(info + 3);
    codepage = *(unsigned int *)(info + 5);
    if (country == 0) return;
    r.x.ax = 0x6601;
    int86(0x21, &r, &r);
    if (!r.x.cflag && r.x.bx != 0) codepage = r.x.bx;
    snprintf(buf, sizeof(buf), "Country %u, codepage %u", country, codepage);
    ff_add("Locale", buf);
}

void mod_title(void)
{
    char user[48], host[48];
    char buf[FF_VAL_MAX];
    int have_user = ff_getenv_buf("USERNAME", user, sizeof(user)) ||
                    ff_getenv_buf("USER", user, sizeof(user));
    int have_host = ff_getenv_buf("COMPUTERNAME", host, sizeof(host)) ||
                    ff_getenv_buf("HOSTNAME", host, sizeof(host));
    if (!have_user && !have_host) return;
    if (have_user && have_host) snprintf(buf, sizeof(buf), "%s@%s", user, host);
    else snprintf(buf, sizeof(buf), "%s", have_user ? user : host);
    ff_add("Title", buf);
}

void mod_editor(void)
{
    const char *ed = ff_getenv("EDITOR");
    if (ed && ed[0]) ff_add("Editor", ed);
}

void mod_shell(void)
{
    char comspec[80];
    char buf[FF_VAL_MAX];
    const char *base;
    int have_comspec = ff_getenv_buf("COMSPEC", comspec, sizeof(comspec));
    /* Positive INT 2Fh installation checks for 4DOS (AX=D44Dh -> 44DDh)
     * and NDOS (AX=E44Dh -> 44EEh): both return major in BL, minor in BH. */
    {
        union REGS r;
        r.x.ax = 0xD44D;
        r.x.bx = 0x0000;
        int86(0x2F, &r, &r);
        if (r.x.ax == 0x44DD) {
            if (have_comspec)
                snprintf(buf, sizeof(buf), "%s (4DOS %u.%02u)", comspec, r.h.bl, r.h.bh);
            else
                snprintf(buf, sizeof(buf), "4DOS %u.%02u", r.h.bl, r.h.bh);
            ff_add("Shell", buf);
            return;
        }
        r.x.ax = 0xE44D;
        r.x.bx = 0x0000;
        int86(0x2F, &r, &r);
        if (r.x.ax == 0x44EE) {
            if (have_comspec)
                snprintf(buf, sizeof(buf), "%s (NDOS %u.%02u)", comspec, r.h.bl, r.h.bh);
            else
                snprintf(buf, sizeof(buf), "NDOS %u.%02u", r.h.bl, r.h.bh);
            ff_add("Shell", buf);
            return;
        }
    }
    if (!have_comspec || !comspec[0]) return;
    base = comspec;
    {
        const char *p = comspec;
        while (*p) {
            if (*p == '\\' || *p == '/') base = p + 1;
            p++;
        }
    }
    /* FreeCOM identifies itself inside its own binary. Scanning the
     * shell's file for its name is a positive identification, not an
     * inference from the DOS flavour, which is why it is trusted
     * here; a file that cannot be opened just yields the plain name. */
    if (stricmp(base, "COMMAND.COM") == 0) {
        FILE *f = fopen(comspec, "rb");
        int is_freecom = 0;
        if (f) {
            static char chunk[4096];
            char tail[8];
            size_t tlen = 0, n;
            while (!is_freecom && (n = fread(chunk + tlen, 1, sizeof(chunk) - tlen, f)) > 0) {
                size_t total = tlen + n, i;
                for (i = 0; i + 7 <= total; i++) {
                    if (memcmp(chunk + i, "FreeCOM", 7) == 0) { is_freecom = 1; break; }
                }
                tlen = total > 7 ? 7 : total;
                memmove(tail, chunk + total - tlen, tlen);
                memcpy(chunk, tail, tlen);
            }
            fclose(f);
        }
        snprintf(buf, sizeof(buf), "%s%s", comspec, is_freecom ? " (FreeCOM)" : "");
    } else {
        snprintf(buf, sizeof(buf), "%s", comspec);
    }
    ff_add("Shell", buf);
}

void mod_bootmgr(void)
{
    const char *cfg = ff_getenv("CONFIG");
    char buf[FF_VAL_MAX];
    if (!cfg || !cfg[0]) return;
    snprintf(buf, sizeof(buf), "FreeDOS menu profile %s", cfg);
    ff_add("Bootmgr", buf);
}

/* ---------------------------------------------------------------- */
/* Resident programs (the DOS analogue of a process list)            */
/* ---------------------------------------------------------------- */

void mod_processes(void)
{
    /* Walk the memory control block chain from its head, which the
     * List of Lists points at (INT 21h AH=52h, ES:[BX-2]). Every
     * resident program's primary PSP MCB has owner == mcb_seg + 1;
     * DOS 4+ also stores the program's name in that MCB. Names are
     * collected, not trusted blindly: an MCB that fails the signature,
     * monotonicity or PSP checks ends the walk, because a corrupt
     * chain says nothing worth printing. */
    union REGS r;
    struct SREGS s;
    unsigned int mcb_seg;
    char names[FF_VAL_MAX];
    int count = 0;
    int steps = 0;
    names[0] = 0;
    r.h.ah = 0x52;
    int86x(0x21, &r, &r, &s);
    mcb_seg = peekw(s.es, r.x.bx - 2);
    while (mcb_seg != 0 && mcb_seg != 0xFFFF && steps < 512) {
        unsigned char sig = peekb(mcb_seg, 0);
        unsigned int owner = peekw(mcb_seg, 1);
        unsigned int size_paras = peekw(mcb_seg, 3);
        unsigned long next_seg;
        steps++;
        if (sig != 'M' && sig != 'Z') break;
        if (owner == mcb_seg + 1 && owner != _psp) {
            /* PSP signature check: a real PSP starts with INT 20h. */
            if (peekb(owner, 0) == 0xCD && peekb(owner, 1) == 0x20) {
                char nm[9];
                int i, printable = 1;
                for (i = 0; i < 8; i++) {
                    nm[i] = (char)peekb(mcb_seg, 8 + i);
                    if (nm[i] && (nm[i] < 0x20 || nm[i] > 0x7E)) printable = 0;
                }
                nm[8] = 0;
                for (i = 7; i >= 0 && nm[i] == ' '; i--) nm[i] = 0;
                if (printable && nm[0]) {
                    count++;
                    if (count <= 6) {
                        if (names[0]) strcat(names, ", ");
                        strcat(names, nm);
                    }
                }
            }
        }
        if (sig == 'Z') break;
        next_seg = (unsigned long)mcb_seg + (unsigned long)size_paras + 1UL;
        if (next_seg > 0xFFFFUL || next_seg <= (unsigned long)mcb_seg) break;
        mcb_seg = (unsigned int)next_seg;
    }
    if (count == 0) return;
    if (count > 6) {
        char extra[24];
        snprintf(extra, sizeof(extra), ", +%d more", count - 6);
        if (strlen(names) + strlen(extra) < sizeof(names))
            strcat(names, extra);
    }
    ff_add("Processes", names);
}

/* ---------------------------------------------------------------- */
/* Gamepad                                                           */
/* ---------------------------------------------------------------- */

void mod_gamepad(void)
{
    /* The gameport lives at I/O 201h. Writing to it fires the axis
     * one-shots: an axis that exists holds its bit high while its
     * timer runs, then drops it. So presence is proven by watching a
     * bit that started high go low within a bounded poll. A port
     * with nothing attached floats all bits high forever, and one
     * whose bits start low proves nothing either, so both of those
     * outcomes stay silent. The poll cap keeps an absent joystick at
     * microseconds, not a hang. */
    unsigned int i;
    unsigned char first, v;
    outp(0x201, 0xFF);
    first = (unsigned char)inp(0x201);
    v = first;
    for (i = 0; i < 60000U && (v & 0x0F) != 0; i++) v = (unsigned char)inp(0x201);
    /* Present means: at least one axis bit started high (its timer
     * was running) and every bit reached low before the cap (the
     * timers fired). Bits that started low prove nothing, and bits
     * that never drop are a floating port, so both stay silent. */
    if ((first & 0x0F) == 0 || (v & 0x0F) != 0) return;
    {
        char buf[FF_VAL_MAX];
        int buttons = 0;
        v = (unsigned char)inp(0x201);
        if (!(v & 0x10)) buttons++;
        if (!(v & 0x20)) buttons++;
        if (!(v & 0x40)) buttons++;
        if (!(v & 0x80)) buttons++;
        sprintf(buf, "Joystick present (%d button(s) down)", buttons);
        ff_add("Gamepad", buf);
    }
}
