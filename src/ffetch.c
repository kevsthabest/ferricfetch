/*
 * FFETCH - a fastfetch-style system summary for DOS.
 *
 * main() only orchestrates: parse the command line and the config
 * file, run the probes in the chosen order, then render. All machine
 * knowledge lives in modules.c and ferric.c; keeping this file free
 * of hardware detail is what lets the module list grow without the
 * rendering rules ever changing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dos.h>
#include <i86.h>
#include "ffetch.h"

struct ff_line ff_lines[FF_MAX_LINES];
int ff_nlines = 0;
struct ff_config ff_cfg;

void ff_add(const char *key, const char *value)
{
    if (ff_nlines >= FF_MAX_LINES) return;
    strncpy(ff_lines[ff_nlines].key, key, FF_KEY_MAX - 1);
    ff_lines[ff_nlines].key[FF_KEY_MAX - 1] = 0;
    strncpy(ff_lines[ff_nlines].value, value, FF_VAL_MAX - 1);
    ff_lines[ff_nlines].value[FF_VAL_MAX - 1] = 0;
    ff_nlines++;
}

/* ------------------------------------------------------------------ */
/* Small shared helpers                                                */
/* ------------------------------------------------------------------ */

void ff_fmt_kb(char *out, unsigned long kb)
{
    if (kb >= 1048576UL) {
        unsigned long g = kb / 1048576UL;
        unsigned long frac = (kb % 1048576UL) * 10UL / 1048576UL;
        sprintf(out, "%lu.%lu GB", g, frac);
    } else if (kb >= 1024UL) {
        unsigned long m = kb / 1024UL;
        unsigned long frac = (kb % 1024UL) * 10UL / 1024UL;
        sprintf(out, "%lu.%lu MB", m, frac);
    } else {
        sprintf(out, "%lu KB", kb);
    }
}

void ff_fmt_bytes(char *out, unsigned long bytes)
{
    ff_fmt_kb(out, bytes / 1024UL + ((bytes % 1024UL) >= 512UL ? 1UL : 0UL));
}

/* Read an environment variable straight from the DOS environment
 * block named by our PSP. The C library's copy would do for most
 * names, but shells write live variables (such as FreeCOM's CONFIG
 * or 4DOS variables) into the environment block directly. Name
 * comparison is case-insensitive, matching DOS convention. */
int ff_getenv_buf(const char *name, char *out, size_t cap)
{
    unsigned int envseg;
    const char __far *env;
    size_t nlen;
    if (!out || cap == 0) return 0;
    out[0] = 0;
    envseg = *(unsigned int __far *)MK_FP(_psp, 0x2C);
    nlen = strlen(name);
    if (envseg == 0 || nlen == 0) return 0;
    env = (const char __far *)MK_FP(envseg, 0);
    while (*env) {
        const char __far *eq = env;
        while (*eq && *eq != '=') eq++;
        if ((size_t)(eq - env) == nlen && *eq == '=') {
            size_t k;
            int match = 1;
            for (k = 0; k < nlen; k++) {
                if (toupper((unsigned char)env[k]) != toupper((unsigned char)name[k])) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                size_t i = 0;
                eq++;
                while (*eq && i < cap - 1) out[i++] = *eq++;
                out[i] = 0;
                return out[0] != 0;
            }
        }
        while (*env) env++;
        env++;
    }
    return 0;
}

const char *ff_getenv(const char *name)
{
    static char value[128];
    if (!ff_getenv_buf(name, value, sizeof(value))) return NULL;
    return value;
}

/* ------------------------------------------------------------------ */
/* Built-in logo. Lines are 26 columns wide so the summary text       */
/* still fits an 80-column screen with room to spare, and the art     */
/* uses pure 7-bit ASCII so no codepage can mangle it.                */
/* ------------------------------------------------------------------ */

static const char *ff_builtin_logo[] = {
    "  .----------------------.",
    "  | .------------------. |",
    "  | |   ___            | |",
    "  | |  | __|__         | |",
    "  | |  | _| -_)  DOS   | |",
    "  | |  |_|\\___|  [Fe]  | |",
    "  | |  C:\\> FFETCH _   | |",
    "  | '------------------' |",
    "  '------.--------.------'",
    "    .----'--------'----.  ",
    "   /  [====]     o  []  \\ ",
    "  '----------------------'",
    NULL
};

#define FF_MAX_LOGO_LINES 24
#define FF_MAX_LOGO_WIDTH 40

static char ff_logo_store[FF_MAX_LOGO_LINES][FF_MAX_LOGO_WIDTH + 1];
static int ff_logo_n = 0;
static int ff_logo_w = 0;
static int ff_logo_builtin = 0;

static void logo_load(void)
{
    int i;
    ff_logo_n = 0;
    ff_logo_w = 0;
    ff_logo_builtin = 0;
    if (ff_cfg.logo_path[0]) {
        FILE *f = fopen(ff_cfg.logo_path, "r");
        if (f) {
            char buf[96];
            while (ff_logo_n < FF_MAX_LOGO_LINES && fgets(buf, sizeof(buf), f)) {
                size_t n = strlen(buf);
                while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
                if (n > FF_MAX_LOGO_WIDTH) n = FF_MAX_LOGO_WIDTH;
                memcpy(ff_logo_store[ff_logo_n], buf, n);
                ff_logo_store[ff_logo_n][n] = 0;
                if ((int)n > ff_logo_w) ff_logo_w = (int)n;
                ff_logo_n++;
            }
            fclose(f);
        }
        /* A logo file that cannot be read is not an error worth
         * stopping for: fall back to no logo rather than the builtin,
         * because the user explicitly chose a file. */
        return;
    }
    ff_logo_builtin = 1;
    for (i = 0; ff_builtin_logo[i] && ff_logo_n < FF_MAX_LOGO_LINES; i++) {
        size_t n = strlen(ff_builtin_logo[i]);
        memcpy(ff_logo_store[ff_logo_n], ff_builtin_logo[i], n + 1);
        if ((int)n > ff_logo_w) ff_logo_w = (int)n;
        ff_logo_n++;
    }
}

/* ------------------------------------------------------------------ */
/* Module registry. The default order mirrors fastfetch's: identity
 * first, then hardware, then session. Config files and /MODULES
 * reorder or subset this list by name.                              */
/* ------------------------------------------------------------------ */

struct ff_module {
    const char *name;
    void (*run)(void);
};

static const struct ff_module ff_modules[] = {
    { "title",     mod_title },
    { "os",        mod_os },
    { "kernel",    mod_kernel },
    { "ferric",    mod_ferric },
    { "host",      mod_host },
    { "board",     mod_board },
    { "bios",      mod_bios },
    { "cpu",       mod_cpu },
    { "gpu",       mod_gpu },
    { "display",   mod_display },
    { "memory",    mod_memory },
    { "physmem",   mod_physmem },
    { "disk",      mod_disk },
    { "physdisk",  mod_physdisk },
    { "sound",     mod_sound },
    { "network",   mod_network },
    { "wifi",      mod_wifi },
    { "modules",   mod_modules },
    { "battery",   mod_battery },
    { "power",     mod_power },
    { "bootmgr",   mod_bootmgr },
    { "shell",     mod_shell },
    { "editor",    mod_editor },
    { "terminal",  mod_terminal },
    { "locale",    mod_locale },
    { "processes", mod_processes },
    { "gamepad",   mod_gamepad },
    { "uptime",    mod_uptime },
    { "datetime",  mod_datetime },
    { NULL, NULL }
};

static void run_modules(const char *selection)
{
    char list[640];
    char *tok;
    if (selection && selection[0]) {
        strncpy(list, selection, sizeof(list) - 1);
        list[sizeof(list) - 1] = 0;
        tok = strtok(list, ",");
        while (tok) {
            int i;
            size_t len;
            while (*tok == ' ' || *tok == '\t') tok++;
            len = strlen(tok);
            while (len > 0 && (tok[len - 1] == ' ' || tok[len - 1] == '\t')) tok[--len] = 0;
            for (i = 0; ff_modules[i].name; i++) {
                if (stricmp(tok, ff_modules[i].name) == 0) {
                    ff_modules[i].run();
                    break;
                }
            }
            tok = strtok(NULL, ",");
        }
    } else {
        int i;
        for (i = 0; ff_modules[i].name; i++) ff_modules[i].run();
    }
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

/* Map ANSI 0-7 colour index to bright CGA/VGA text foreground attribute. */
static unsigned char ansi_to_bios_fg(unsigned char ansi_color)
{
    static const unsigned char map[8] = {
        0x08, /* 0: dark grey */
        0x0C, /* 1: bright red */
        0x0A, /* 2: bright green */
        0x0E, /* 3: bright yellow */
        0x09, /* 4: bright blue */
        0x0D, /* 5: bright magenta */
        0x0B, /* 6: bright cyan */
        0x0F  /* 7: bright white */
    };
    return map[ansi_color & 7];
}

/* Map ANSI 0-7 colour index to CGA/VGA background attribute for palette bar. */
static unsigned char ansi_to_bios_bg(unsigned char ansi_color)
{
    static const unsigned char map[8] = {
        0x00, 0x40, 0x20, 0x60, 0x10, 0x50, 0x30, 0x70
    };
    return map[ansi_color & 7];
}

static unsigned char bios_page = 0;

static void bios_init_page(void)
{
    union REGS r;
    r.h.ah = 0x0F;
    int86(0x10, &r, &r);
    bios_page = r.h.bh;
}

static void bios_putc_attr(char ch, unsigned char attr)
{
    union REGS r;
    if (ch == '\r' || ch == '\n') {
        r.h.ah = 0x0E;
        r.h.al = (unsigned char)ch;
        r.h.bh = bios_page;
        r.h.bl = 0x07;
        int86(0x10, &r, &r);
        return;
    }
    /* AH=09h writes char + attribute at cursor; AH=0Eh advances cursor
     * (with wrap and scroll) while keeping the cell's attribute byte. */
    r.h.ah = 0x09;
    r.h.al = (unsigned char)ch;
    r.h.bh = bios_page;
    r.h.bl = attr;
    r.x.cx = 1;
    int86(0x10, &r, &r);
    r.h.ah = 0x0E;
    r.h.al = (unsigned char)ch;
    r.h.bh = bios_page;
    r.h.bl = attr;
    int86(0x10, &r, &r);
}

static void bios_puts_attr(const char *s, unsigned char attr)
{
    while (*s) bios_putc_attr(*s++, attr);
}

static void emit_ansi_color(FILE *out, int color)
{
    fprintf(out, "\x1b[1;3%dm", color & 7);
}

/* Draw one logo line, applying two-tone highlighting to the inner CRT
 * screen of the built-in logo when colour is active. */
static void render_logo_line(FILE *out, int row, const char *art)
{
    int len = (int)strlen(art);
    int pad = ff_logo_w - len;
    int two_tone = ff_logo_builtin && row >= 2 && row <= 6 && len >= 23;
    if (ff_cfg.use_bios_color) {
        unsigned char c_logo = ansi_to_bios_fg(ff_cfg.color_logo);
        unsigned char c_scr  = ansi_to_bios_fg(ff_cfg.color_key);
        int j;
        for (j = 0; j < len; j++) {
            unsigned char attr = (two_tone && j >= 5 && j <= 20) ? c_scr : c_logo;
            bios_putc_attr(art[j], attr);
        }
        while (pad-- > 0) bios_putc_attr(' ', 0x07);
        bios_puts_attr("  ", 0x07);
    } else if (ff_cfg.use_ansi) {
        if (two_tone) {
            emit_ansi_color(out, ff_cfg.color_logo);
            fprintf(out, "%.5s", art);
            emit_ansi_color(out, ff_cfg.color_key);
            fprintf(out, "%.16s", art + 5);
            emit_ansi_color(out, ff_cfg.color_logo);
            fprintf(out, "%s", art + 21);
        } else {
            emit_ansi_color(out, ff_cfg.color_logo);
            fprintf(out, "%s", art);
        }
        fprintf(out, "\x1b[0m");
        while (pad-- > 0) fputc(' ', out);
        fprintf(out, "  ");
    } else {
        fprintf(out, "%-*s  ", ff_logo_w, art);
    }
}

static void render_palette_bar(FILE *out)
{
    int c, pad;
    if (!ff_cfg.use_ansi && !ff_cfg.use_bios_color) return;
    pad = ff_logo_n > 0 ? ff_logo_w + 2 : 0;
    if (ff_cfg.use_bios_color) {
        bios_puts_attr("\r\n", 0x07);
        while (pad-- > 0) bios_putc_attr(' ', 0x07);
        for (c = 0; c < 8; c++) {
            unsigned char bg = ansi_to_bios_bg((unsigned char)c);
            bios_puts_attr("   ", (unsigned char)(bg | 0x07));
        }
        bios_puts_attr("\r\n", 0x07);
    } else {
        fprintf(out, "\r\n");
        while (pad-- > 0) fputc(' ', out);
        for (c = 0; c < 8; c++) fprintf(out, "\x1b[4%dm   ", c);
        fprintf(out, "\x1b[0m\r\n");
    }
}

static int probe_screen_cols(void)
{
    union REGS r;
    r.h.ah = 0x0F;
    int86(0x10, &r, &r);
    return r.h.ah ? (int)r.h.ah : 80;
}

static void emit_plain_n(FILE *out, const char *s, int n)
{
    if (ff_cfg.use_bios_color) {
        while (n-- > 0 && *s) bios_putc_attr(*s++, 0x07);
    } else {
        fprintf(out, "%.*s", n, s);
    }
}

static void emit_newline(FILE *out)
{
    if (ff_cfg.use_bios_color) bios_puts_attr("\r\n", 0x07);
    else fprintf(out, "\r\n");
}

/* Choose how many characters of `p` fit within `limit`, breaking at a
 * space when possible so words and MAC addresses stay intact. */
static int wrap_chunk_len(const char *p, int limit)
{
    int len = (int)strlen(p);
    int k;
    if (limit <= 0) return 0;
    if (len <= limit) return len;
    for (k = limit; k > 0; k--) {
        if (p[k] == ' ') return k;
    }
    return limit;
}

static void render_pretty(FILE *out)
{
    int i, screen_row = 0;
    int use_logo = ff_cfg.show_logo && ff_cfg.stdout_is_console && !ff_cfg.json;
    int max_col = 0, left_w = 0, avail = 0;
    if (use_logo) logo_load(); else ff_logo_n = 0;
    if (ff_cfg.use_bios_color) {
        fflush(out);
        bios_init_page();
    }
    if (ff_cfg.stdout_is_console) {
        int cols = probe_screen_cols();
        /* Keep 1 column of margin so writing to the rightmost cell
         * never triggers an automatic BIOS wrap + stray blank line. */
        max_col = cols > 1 ? cols - 1 : 79;
        if (ff_logo_n > 0 && max_col - (ff_logo_w + 2) < 20) {
            ff_logo_n = 0;
        }
        left_w = ff_logo_n > 0 ? ff_logo_w + 2 : 0;
        avail = max_col - left_w;
    }
    for (i = 0; i < ff_nlines; i++) {
        if (!ff_cfg.stdout_is_console) {
            fprintf(out, "%s: %s\r\n", ff_lines[i].key, ff_lines[i].value);
            continue;
        }
        {
            const char *p = ff_lines[i].value;
            int klen = (int)strlen(ff_lines[i].key);
            int first = 1;
            do {
                if (ff_logo_n > 0) {
                    const char *art = screen_row < ff_logo_n ? ff_logo_store[screen_row] : "";
                    render_logo_line(out, screen_row, art);
                }
                if (first) {
                    int limit = avail - (klen + 2);
                    if (ff_cfg.use_bios_color) {
                        bios_puts_attr(ff_lines[i].key, ansi_to_bios_fg(ff_cfg.color_key));
                        bios_puts_attr(": ", 0x07);
                    } else {
                        if (ff_cfg.use_ansi) emit_ansi_color(out, ff_cfg.color_key);
                        fprintf(out, "%s", ff_lines[i].key);
                        if (ff_cfg.use_ansi) fprintf(out, "\x1b[0m");
                        fprintf(out, ": ");
                    }
                    if (limit > 0) {
                        int n = wrap_chunk_len(p, limit);
                        emit_plain_n(out, p, n);
                        p += n;
                        while (*p == ' ') p++;
                    }
                    first = 0;
                } else {
                    int limit = avail > 2 ? avail - 2 : 1;
                    int n = wrap_chunk_len(p, limit);
                    emit_plain_n(out, "  ", 2);
                    emit_plain_n(out, p, n);
                    p += n;
                    while (*p == ' ') p++;
                }
                emit_newline(out);
                screen_row++;
            } while (*p);
        }
    }
    while (screen_row < ff_logo_n) {
        render_logo_line(out, screen_row, ff_logo_store[screen_row]);
        emit_newline(out);
        screen_row++;
    }
    render_palette_bar(out);
}

/* The save file gets the same layout as the screen but never any ANSI
 * escapes: it is meant to be read later in an editor, where escape
 * bytes are garbage. The logo is kept because in plain text it is
 * part of the document, not a terminal effect. */
static void render_save(const char *path)
{
    FILE *f = fopen(path, "w");
    int i;
    if (!f) return;
    if (ff_cfg.show_logo) {
        int rows;
        logo_load();
        rows = ff_nlines > ff_logo_n ? ff_nlines : ff_logo_n;
        for (i = 0; i < rows; i++) {
            const char *art = i < ff_logo_n ? ff_logo_store[i] : "";
            fprintf(f, "%-*s  ", ff_logo_w, art);
            if (i < ff_nlines) fprintf(f, "%s: %s", ff_lines[i].key, ff_lines[i].value);
            fprintf(f, "\r\n");
        }
    } else {
        for (i = 0; i < ff_nlines; i++)
            fprintf(f, "%s: %s\r\n", ff_lines[i].key, ff_lines[i].value);
    }
    fclose(f);
}

static void json_escape(FILE *out, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(out, "\\%c", c);
        else if (c < 0x20 || c >= 0x80) fprintf(out, "\\u%04x", c);
        else fputc(c, out);
    }
}

static void json_norm_key(const char *src, char *dst, size_t cap)
{
    size_t j;
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
    for (j = 0; dst[j]; j++) {
        if (dst[j] == ' ') dst[j] = '_';
        else dst[j] = (char)tolower((unsigned char)dst[j]);
    }
}

static void render_json(FILE *out)
{
    unsigned char emitted[FF_MAX_LINES];
    int i;
    memset(emitted, 0, sizeof(emitted));
    fprintf(out, "{\r\n  \"ffetch\": \"1.0\"");
    for (i = 0; i < ff_nlines; i++) {
        char key[FF_KEY_MAX];
        int j, count = 0;
        if (emitted[i]) continue;
        json_norm_key(ff_lines[i].key, key, sizeof(key));
        for (j = i; j < ff_nlines; j++) {
            char k2[FF_KEY_MAX];
            if (emitted[j]) continue;
            json_norm_key(ff_lines[j].key, k2, sizeof(k2));
            if (strcmp(key, k2) == 0) count++;
        }
        fprintf(out, ",\r\n  \"%s\": ", key);
        if (count == 1) {
            emitted[i] = 1;
            fputc('"', out);
            json_escape(out, ff_lines[i].value);
            fputc('"', out);
        } else {
            int seen = 0;
            fputc('[', out);
            for (j = i; j < ff_nlines; j++) {
                char k2[FF_KEY_MAX];
                if (emitted[j]) continue;
                json_norm_key(ff_lines[j].key, k2, sizeof(k2));
                if (strcmp(key, k2) == 0) {
                    emitted[j] = 1;
                    if (seen++) fprintf(out, ", ");
                    fputc('"', out);
                    json_escape(out, ff_lines[j].value);
                    fputc('"', out);
                }
            }
            fputc(']', out);
        }
    }
    fprintf(out, "\r\n}\r\n");
}

/* ------------------------------------------------------------------ */
/* Config file                                                         */
/* ------------------------------------------------------------------ */

static const char ff_default_cfg[] =
    "; FFETCH.CFG - written by FFETCH /GENCFG\r\n"
    "; Lines starting with ; or # are comments. Every setting here can\r\n"
    "; also be given on the command line, where it wins.\r\n"
    ";\r\n"
    "; logo=<path>   custom ASCII logo file (omit for the built-in art)\r\n"
    "; logo=none     no logo\r\n"
    "; nologo=1      same as logo=none\r\n"
    "; cls=1         clear the screen before drawing (default 1)\r\n"
    "; color=1       enable colour on the DOS console (default 1)\r\n"
    "; color_key=6   label colour, 0-7 (default 6, cyan)\r\n"
    "; color_logo=3  logo colour, 0-7 (default 3, yellow)\r\n"
    "; modules=os,cpu,memory,disk,uptime\r\n"
    ";               comma list in display order; omit for all modules\r\n"
    "cls=1\r\n"
    "color=1\r\n"
    "color_key=6\r\n"
    "color_logo=3\r\n";

static void cfg_set(char *key, char *val)
{
    while (*key == ' ' || *key == '\t') key++;
    while (*val == ' ' || *val == '\t') val++;
    {
        size_t n = strlen(val);
        while (n > 0 && (val[n-1] == ' ' || val[n-1] == '\t' || val[n-1] == '\r' || val[n-1] == '\n')) val[--n] = 0;
    }
    if (stricmp(key, "logo") == 0) {
        if (stricmp(val, "none") == 0) ff_cfg.show_logo = 0;
        else {
            strncpy(ff_cfg.logo_path, val, sizeof(ff_cfg.logo_path) - 1);
            ff_cfg.logo_path[sizeof(ff_cfg.logo_path) - 1] = 0;
            ff_cfg.show_logo = 1;
        }
    } else if (stricmp(key, "nologo") == 0) {
        if (atoi(val)) ff_cfg.show_logo = 0;
    } else if (stricmp(key, "cls") == 0) {
        ff_cfg.clear_screen = atoi(val) != 0;
    } else if (stricmp(key, "color") == 0) {
        ff_cfg.allow_color = atoi(val) != 0;
    } else if (stricmp(key, "color_key") == 0) {
        ff_cfg.color_key = (unsigned char)(atoi(val) & 7);
    } else if (stricmp(key, "color_logo") == 0) {
        ff_cfg.color_logo = (unsigned char)(atoi(val) & 7);
    } else if (stricmp(key, "modules") == 0) {
        strncpy(ff_cfg.modules, val, sizeof(ff_cfg.modules) - 1);
        ff_cfg.modules[sizeof(ff_cfg.modules) - 1] = 0;
    }
    /* Unknown keys are ignored on purpose: a config written for a
     * newer FFETCH must not break an older one sharing the directory. */
}

static void cfg_load(const char *path)
{
    FILE *f = fopen(path, "r");
    char buf[256];
    if (!f) return;
    while (fgets(buf, sizeof(buf), f)) {
        char *eq = strchr(buf, '=');
        char *p = buf;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ';' || *p == '#' || *p == '\r' || *p == '\n' || *p == 0) continue;
        if (!eq) continue;
        *eq = 0;
        cfg_set(buf, eq + 1);
    }
    fclose(f);
}

/* The default config lives beside the executable, so a single
 * FFETCH directory dropped on any DOS install behaves the same
 * regardless of the current directory when it is run. */
static void cfg_default_path(char *out, size_t cap)
{
    unsigned int psp_seg = _psp;
    /* argv[0] is not reliable under every DOS shell, so use the
     * environment's program path instead: DOS 3+ appends it after
     * the environment strings. Fall back to the current directory. */
    unsigned int envseg = *(unsigned int __far *)MK_FP(psp_seg, 0x2C);
    const char *fallback = "FFETCH.CFG";
    if (envseg != 0) {
        const char __far *p = (const char __far *)MK_FP(envseg, 0);
        unsigned int count;
        while (*p) { while (*p) p++; p++; }
        p++; /* skip the double-NUL terminator */
        count = *(unsigned int __far *)p;
        p += 2;
        if (count >= 1 && *p && cap > 12) {
            size_t n = 0;
            while (p[n] && n < cap - 12) { out[n] = p[n]; n++; }
            /* strip the file name, keep the directory */
            while (n > 0 && out[n-1] != '\\' && out[n-1] != ':' && out[n-1] != '/') n--;
            strcpy(out + n, "FFETCH.CFG");
            return;
        }
    }
    strncpy(out, fallback, cap - 1);
    out[cap - 1] = 0;
}

/* ------------------------------------------------------------------ */
/* Console probes                                                      */
/* ------------------------------------------------------------------ */

/* stdout is the screen console only when handle 1 is both a character
 * device (bit 7, ISDEV) and the standard console output device (bit 1,
 * ISCOT). Redirection to a file, pipe, or serial port (e.g. > COM1)
 * clears ISDEV or ISCOT, so colours and the side-by-side logo are
 * suppressed automatically. */
static int probe_stdout_console(void)
{
    union REGS r;
    r.x.ax = 0x4400;
    r.x.bx = 1;
    int86(0x21, &r, &r);
    if (r.x.cflag) return 0;
    return (r.x.dx & 0x82) == 0x82;
}

/* ANSI.SYS (or a compatible driver, including 4DOS's ANSI support)
 * answers INT 2Fh AX=1A00h with AL=FFh. When absent on a real console,
 * FFETCH falls back to direct BIOS INT 10h colour attributes instead
 * of emitting escape sequences. */
static int probe_ansi(void)
{
    union REGS r;
    r.x.ax = 0x1A00;
    int86(0x2F, &r, &r);
    return (r.h.al == 0xFF);
}

static void clear_screen(void)
{
    union REGS r;
    /* Scroll the full screen up by its whole height, then home the
     * cursor: the BIOS-only clear that works on any adapter DOS can
     * boot on (including 43/50-line modes), unlike a mode set, which
     * would drop a VESA or tall text mode. */
    r.h.ah = 0x0F;
    int86(0x10, &r, &r);
    {
        unsigned char cols = r.h.ah;
        unsigned char page = r.h.bh;
        unsigned char max_row = *(unsigned char __far *)MK_FP(0x40, 0x84);
        union REGS s;
        if (max_row < 19 || max_row > 99) max_row = 24;
        s.h.ah = 0x06;
        s.h.al = 0;
        s.h.bh = 0x07;
        s.x.cx = 0;
        s.h.dh = max_row;
        s.h.dl = cols ? (unsigned char)(cols - 1) : 79;
        int86(0x10, &s, &s);
        s.h.ah = 0x02;
        s.h.bh = page;
        s.x.dx = 0;
        int86(0x10, &s, &s);
    }
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    char cfg_path[96];
    const char *cfg_override = NULL;
    int i;
    int gencfg = 0;

    memset(&ff_cfg, 0, sizeof(ff_cfg));
    ff_cfg.show_logo = 1;
    ff_cfg.clear_screen = 1;
    ff_cfg.allow_color = 1;
    ff_cfg.color_key = 6;
    ff_cfg.color_logo = 3;

    /* Pass 1: handle help, config-file path override, and /GENCFG. */
    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '/' && a[0] != '-') continue;
        a++;
        if (stricmp(a, "GENCFG") == 0) gencfg = 1;
        else if (strnicmp(a, "C=", 2) == 0) cfg_override = a + 2;
        else if (stricmp(a, "?") == 0 || stricmp(a, "H") == 0 || stricmp(a, "HELP") == 0) {
            printf("FFETCH - fastfetch-style system summary for DOS\r\n"
                   "  /NOLOGO        no ASCII logo\r\n"
                   "  /NOCLS         do not clear the screen\r\n"
                   "  /NOCOLOR       disable console colours\r\n"
                   "  /JSON          machine-readable output\r\n"
                   "  /SAVE=<file>   also write plain-text output to <file>\r\n"
                   "  /LOGO=<file>   use a custom ASCII logo file\r\n"
                   "  /C=<file>      read this config file instead of FFETCH.CFG\r\n"
                   "  /MODULES=a,b   run only these modules, in this order\r\n"
                   "  /GENCFG        write a commented default FFETCH.CFG and exit\r\n");
            return 0;
        }
    }

    if (cfg_override) {
        strncpy(cfg_path, cfg_override, sizeof(cfg_path) - 1);
        cfg_path[sizeof(cfg_path) - 1] = 0;
    } else {
        cfg_default_path(cfg_path, sizeof(cfg_path));
    }

    if (gencfg) {
        FILE *f = fopen(cfg_path, "w");
        if (!f) {
            fprintf(stderr, "FFETCH: cannot write %s\r\n", cfg_path);
            return 1;
        }
        fputs(ff_default_cfg, f);
        fclose(f);
        printf("Wrote %s\r\n", cfg_path);
        return 0;
    }

    cfg_load(cfg_path);

    /* Pass 2: command-line switches override the config file. */
    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '/' && a[0] != '-') continue;
        a++;
        if (stricmp(a, "NOLOGO") == 0) ff_cfg.show_logo = 0;
        else if (stricmp(a, "NOCLS") == 0) ff_cfg.clear_screen = 0;
        else if (stricmp(a, "NOCOLOR") == 0) ff_cfg.allow_color = 0;
        else if (stricmp(a, "JSON") == 0) ff_cfg.json = 1;
        else if (strnicmp(a, "SAVE=", 5) == 0) {
            strncpy(ff_cfg.save_path, a + 5, sizeof(ff_cfg.save_path) - 1);
            ff_cfg.save_path[sizeof(ff_cfg.save_path) - 1] = 0;
        } else if (strnicmp(a, "LOGO=", 5) == 0) {
            strncpy(ff_cfg.logo_path, a + 5, sizeof(ff_cfg.logo_path) - 1);
            ff_cfg.logo_path[sizeof(ff_cfg.logo_path) - 1] = 0;
            ff_cfg.show_logo = 1;
        } else if (strnicmp(a, "MODULES=", 8) == 0) {
            strncpy(ff_cfg.modules, a + 8, sizeof(ff_cfg.modules) - 1);
            ff_cfg.modules[sizeof(ff_cfg.modules) - 1] = 0;
        }
    }

    ff_cfg.stdout_is_console = probe_stdout_console();
    if (!ff_cfg.json && ff_cfg.stdout_is_console && ff_cfg.allow_color) {
        if (probe_ansi()) ff_cfg.use_ansi = 1;
        else ff_cfg.use_bios_color = 1;
    }

    run_modules(ff_cfg.modules);

    if (ff_cfg.save_path[0]) render_save(ff_cfg.save_path);

    if (ff_cfg.json) {
        render_json(stdout);
    } else {
        if (ff_cfg.clear_screen && ff_cfg.stdout_is_console) clear_screen();
        render_pretty(stdout);
    }
    return 0;
}
