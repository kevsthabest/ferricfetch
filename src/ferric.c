/*
 * FFETCH tier 3: the Ferric resident core.
 *
 * The core is found the way every Ferric tool finds it: INT 2Fh
 * AX=1684h with BX=0FD01h returns the V86 API entry point, and a
 * version call (AX=0000h) to that entry answers AX=0001h, BX=0FD01h.
 * Both steps must answer before any Ferric line is printed - a
 * machine without the core (every other DOS install) fails the first
 * step and this file contributes nothing.
 *
 * Extended data (Wi-Fi state, the module inventory, the stack IP)
 * comes from one read-only status verb, V86 API AX=0049h, defined in
 * the README of this crate. Cores that predate the verb set carry
 * for it, and FFETCH treats that as "no data", not as an error: the
 * Ferric line still prints from the version call alone.
 */
#include <stdio.h>
#include <string.h>
#include <dos.h>
#include <i86.h>
#include "ffetch.h"

/* From cpu.asm: the register-level far call into the core's API. */
void __cdecl ferric_api(unsigned int entry_off, unsigned int entry_seg,
                unsigned int ax_in, unsigned int es_in,
                unsigned int di_in, unsigned int cx_in,
                unsigned int *out_ax, unsigned int *out_bx,
                unsigned int *out_cf);

int ff_ferric_present = 0;
unsigned int ff_ferric_abi = 0;
struct ff_ferric_status ff_fst;

static unsigned int api_off = 0, api_seg = 0;
static int probe_done = 0;

/* The status block layout, version 1. The core writes it; FFETCH
 * reads it only after the magic and version check out, because the
 * buffer is also what an unanswering core leaves untouched. */
#define FST_MAGIC 0x4646        /* 'FF' */
#define FST_VERSION 1
#define FST_BUFSIZE 512

static unsigned char fst_buf[FST_BUFSIZE];

static unsigned int fst_w(int off)
{
    return (unsigned int)fst_buf[off] | ((unsigned int)fst_buf[off + 1] << 8);
}

int ff_ferric_probe(void)
{
    union REGS r;
    struct SREGS s;
    unsigned int oax, obx, ocf;
    if (probe_done) return ff_ferric_present;
    probe_done = 1;
    memset(&ff_fst, 0, sizeof(ff_fst));

    segread(&s);
    r.x.ax = 0x1684;
    r.x.bx = 0xFD01;
    s.es = 0;
    r.x.di = 0;
    int86x(0x2F, &r, &r, &s);
    api_seg = s.es;
    api_off = r.x.di;
    if (api_seg == 0 && api_off == 0) return 0;

    /* Version call: the contract is AX=0001h and BX=0FD01h back,
     * carry clear. Anything else means the entry point belongs to
     * something that is not the Ferric core. */
    ferric_api(api_off, api_seg, 0x0000, 0, 0, 0, &oax, &obx, &ocf);
    if (ocf || oax != 0x0001 || obx != 0xFD01) {
        api_seg = api_off = 0;
        return 0;
    }
    ff_ferric_present = 1;
    ff_ferric_abi = oax;

    /* Status verb. The buffer is pre-filled with EEh so that a core
     * which silently ignores the verb (rather than setting carry)
     * still cannot produce a passing magic check by accident. */
    memset(fst_buf, 0xEE, sizeof(fst_buf));
    ferric_api(api_off, api_seg, 0x0049,
               FP_SEG(fst_buf), FP_OFF(fst_buf), FST_BUFSIZE,
               &oax, &obx, &ocf);
    if (!ocf && fst_w(0) == FST_MAGIC && fst_w(2) == FST_VERSION) {
        unsigned int flags = fst_w(6);
        unsigned int total = fst_w(4);
        int i, n;
        if (total < 0x3D || total > FST_BUFSIZE) return 1;
        ff_fst.answered = 1;
        ff_fst.wifi_present = (flags & 1) != 0;
        ff_fst.wifi_connected = (flags & 2) != 0;
        ff_fst.ip_valid = (flags & 8) != 0;
        n = fst_buf[8];
        if (n > 32) n = 32;
        memcpy(ff_fst.ssid, fst_buf + 9, n);
        ff_fst.ssid[n] = 0;
        memcpy(ff_fst.bssid, fst_buf + 0x29, 6);
        ff_fst.channel = fst_buf[0x2F];
        memcpy(ff_fst.ip, fst_buf + 0x32, 4);
        memcpy(ff_fst.mac, fst_buf + 0x36, 6);
        n = fst_buf[0x3C];
        if (n > 8) n = 8;
        ff_fst.jlm_count = n;
        for (i = 0; i < n; i++) {
            int off = 0x3D + i * 14;
            if ((unsigned int)(off + 14) > total) {
                ff_fst.jlm_count = i;
                break;
            }
            ff_fst.jlm[i].ddb_id = fst_w(off);
            memcpy(ff_fst.jlm[i].name, fst_buf + off + 2, 12);
            ff_fst.jlm[i].name[12] = 0;
        }
    }
    return 1;
}

void mod_ferric(void)
{
    char buf[FF_VAL_MAX];
    if (!ff_ferric_probe()) return;
    snprintf(buf, sizeof(buf), "Resident core (DDB FD01), ABI v%u", ff_ferric_abi);
    ff_add("Ferric", buf);
}

void mod_wifi(void)
{
    char buf[FF_VAL_MAX];
    if (!ff_ferric_probe()) return;
    if (!ff_fst.answered || !ff_fst.wifi_present) return;
    if (ff_fst.wifi_connected) {
        if (ff_fst.ssid[0])
            snprintf(buf, sizeof(buf), "Associated to \"%s\", ch %u, BSSID %02X:%02X:%02X:%02X:%02X:%02X",
                     ff_fst.ssid, ff_fst.channel,
                     ff_fst.bssid[0], ff_fst.bssid[1], ff_fst.bssid[2],
                     ff_fst.bssid[3], ff_fst.bssid[4], ff_fst.bssid[5]);
        else
            snprintf(buf, sizeof(buf), "Associated, ch %u, BSSID %02X:%02X:%02X:%02X:%02X:%02X",
                     ff_fst.channel,
                     ff_fst.bssid[0], ff_fst.bssid[1], ff_fst.bssid[2],
                     ff_fst.bssid[3], ff_fst.bssid[4], ff_fst.bssid[5]);
        ff_add("Wi-Fi", buf);
    } else {
        ff_add("Wi-Fi", "Present, not associated");
    }
    if (ff_fst.ip_valid) {
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ff_fst.ip[0], ff_fst.ip[1], ff_fst.ip[2], ff_fst.ip[3]);
        ff_add("Stack IP", buf);
    }
}

void mod_modules(void)
{
    char buf[FF_VAL_MAX];
    size_t pos = 0;
    int i;
    if (!ff_ferric_probe()) return;
    if (!ff_fst.answered || ff_fst.jlm_count == 0) return;
    buf[0] = 0;
    for (i = 0; i < ff_fst.jlm_count; i++) {
        char entry[32];
        size_t elen;
        snprintf(entry, sizeof(entry), "%s%s (%04X)", i ? ", " : "",
                 ff_fst.jlm[i].name, ff_fst.jlm[i].ddb_id);
        elen = strlen(entry);
        if (pos + elen >= sizeof(buf)) {
            if (pos + 5 < sizeof(buf)) {
                memcpy(buf + pos, ", ...", 6);
                pos += 5;
            }
            break;
        }
        memcpy(buf + pos, entry, elen + 1);
        pos += elen;
    }
    ff_add("Modules", buf);
}
