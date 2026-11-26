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
 * names, but the shell writes CONFIG (the FreeDOS menu selection)
 * into the master environment after boot, and the direct read is the
 * one guaranteed to see the same block the shell itself uses. */
const char *ff_getenv(const char *name)
{
    static char value[128];
    unsigned int envseg = *(unsigned int __far *)MK_FP(_psp, 0x2C);
    const char __far *env;
    size_t nlen = strlen(name);
    if (envseg == 0) return NULL;
    env = (const char __far *)MK_FP(envseg, 0);
    while (*env) {
        const char __far *eq = env;
        while (*eq && *eq != '=') eq++;
        if ((size_t)(eq - env) == nlen && _fmemcmp(env, name, nlen) == 0 && *eq == '=') {
            size_t i = 0;
            eq++;
            while (*eq && i < sizeof(value) - 1) value[i++] = *eq++;
            value[i] = 0;
            return value;
        }
        while (*env) env++;
        env++;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Built-in logo. Lines are kept under 24 columns so the summary text
 * still fits an 80-column screen with room to spare, and the art uses
 * plain ASCII so no codepage can mangle it.                          */
/* ------------------------------------------------------------------ */

static const char *ff_builtin_logo[] = {
    "  ___________________  ",
    " |   _______________ | ",
    " |  |  F E R R I C   | |",
    " |  |   D O S        | |",
    " |  |_______________| |",
    " |   _____   _____     |",
    " |  |_____| |_____|    |",
    " |___________________| ",
    NULL
};

#define FF_MAX_LOGO_LINES 24
#define FF_MAX_LOGO_WIDTH 40

static char ff_logo_store[FF_MAX_LOGO_LINES][FF_MAX_LOGO_WIDTH + 1];
static int ff_logo_n = 0;
static int ff_logo_w = 0;

static void logo_load(void)
{
    int i;
    ff_logo_n = 0;
    ff_logo_w = 0;
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
            while (*tok == ' ') tok++;
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

static void emit_ansi_color(FILE *out, int color)
{
    fprintf(out, "\x1b[3%dm", color);
}

static void render_pretty(FILE *out)
{
    int rows, i;
    int use_logo = ff_cfg.show_logo && ff_cfg.stdout_is_console && !ff_cfg.json;
    if (use_logo) logo_load(); else ff_logo_n = 0;
    rows = ff_nlines > ff_logo_n ? ff_nlines : ff_logo_n;
    for (i = 0; i < rows; i++) {
        if (ff_logo_n > 0) {
            const char *art = i < ff_logo_n ? ff_logo_store[i] : "";
            if (ff_cfg.use_ansi) emit_ansi_color(out, ff_cfg.color_logo);
            fprintf(out, "%-*s", ff_logo_w, art);
            if (ff_cfg.use_ansi) fprintf(out, "\x1b[0m");
            fprintf(out, "  ");
        }
        if (i < ff_nlines) {
            if (ff_cfg.use_ansi) emit_ansi_color(out, ff_cfg.color_key);
            fprintf(out, "%s", ff_lines[i].key);
            if (ff_cfg.use_ansi) fprintf(out, "\x1b[0m");
            fprintf(out, ": %s", ff_lines[i].value);
        }
        fprintf(out, "\r\n");
    }
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
        else if (c < 0x20) fprintf(out, "\\u%04x", c);
        else fputc(c, out);
    }
}

static void render_json(FILE *out)
{
    int i;
    fprintf(out, "{\r\n  \"ffetch\": \"1.0\",\r\n");
    for (i = 0; i < ff_nlines; i++) {
        char key[FF_KEY_MAX];
        int j;
        /* JSON keys are the module-style lowercase label with spaces
         * folded to underscores, so "Physical Disk" stays usable as
         * an identifier for whatever parses the output. */
        strncpy(key, ff_lines[i].key, sizeof(key) - 1);
        key[sizeof(key) - 1] = 0;
        for (j = 0; key[j]; j++) {
            if (key[j] == ' ') key[j] = '_';
            else key[j] = (char)tolower((unsigned char)key[j]);
        }
        fprintf(out, "  \"%s\": \"", key);
        json_escape(out, ff_lines[i].value);
        fprintf(out, "\"%s\r\n", i + 1 < ff_nlines ? "," : "");
    }
    fprintf(out, "}\r\n");
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
    "; color_key=6   label colour, ANSI 0-7 (default 6, cyan)\r\n"
    "; color_logo=3  logo colour, ANSI 0-7 (default 3, yellow)\r\n"
    "; modules=os,cpu,memory,disk,uptime\r\n"
    ";               comma list in display order; omit for all modules\r\n"
    "cls=1\r\n"
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
        else { strncpy(ff_cfg.logo_path, val, sizeof(ff_cfg.logo_path) - 1); ff_cfg.show_logo = 1; }
    } else if (stricmp(key, "nologo") == 0) {
        if (atoi(val)) ff_cfg.show_logo = 0;
    } else if (stricmp(key, "cls") == 0) {
        ff_cfg.clear_screen = atoi(val) != 0;
    } else if (stricmp(key, "color_key") == 0) {
        ff_cfg.color_key = (unsigned char)(atoi(val) & 7);
    } else if (stricmp(key, "color_logo") == 0) {
        ff_cfg.color_logo = (unsigned char)(atoi(val) & 7);
    } else if (stricmp(key, "modules") == 0) {
        strncpy(ff_cfg.modules, val, sizeof(ff_cfg.modules) - 1);
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
    const char __far *env = (const char __far *)MK_FP(envseg, 0);
    const char __far *p = env;
    const char *fallback = "FFETCH.CFG";
    size_t i;
    while (*p) { while (*p) p++; p++; }
    p++; /* skip the word-sized count that follows the strings */
    {
        unsigned int count = *(unsigned int __far *)p;
        p += 2;
        if (count >= 1 && *p) {
            size_t n = 0;
            while (p[n] && n < cap - 12) { out[n] = p[n]; n++; }
            /* strip the file name, keep the directory */
            while (n > 0 && out[n-1] != '\\' && out[n-1] != ':') n--;
            strcpy(out + n, "FFETCH.CFG");
            return;
        }
    }
    (void)i;
    strncpy(out, fallback, cap - 1);
    out[cap - 1] = 0;
}

/* ------------------------------------------------------------------ */
/* Console probes                                                      */
/* ------------------------------------------------------------------ */

/* stdout is the console only when handle 1 is a character device.
 * When it has been redirected to a file or a pipe the device bit is
 * clear, and both colour and the side-by-side logo would corrupt the
 * captured text, so the pretty renderer is disabled there. */
static int probe_stdout_console(void)
{
    union REGS r;
    r.x.ax = 0x4400;
    r.x.bx = 1;
    int86(0x21, &r, &r);
    if (r.x.cflag) return 1;    /* cannot tell: assume a console */
    return (r.x.dx & 0x80) != 0;
}

/* ANSI.SYS (or a compatible driver) answers INT 2Fh AX=1A00h with
 * AL=FFh. Without it, escape sequences would print as garbage on the
 * stock DOS console, so colour is strictly opt-in by this probe. */
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
     * boot on, unlike a mode set, which would drop a VESA mode. */
    r.h.ah = 0x0F;
    int86(0x10, &r, &r);
    {
        unsigned char cols = r.h.ah;
        unsigned char page = r.h.bh;
        union REGS s;
        s.h.ah = 0x06;
        s.h.al = 0;
        s.h.bh = 0x07;
        s.x.cx = 0;
        s.h.dh = 24;
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
    ff_cfg.color_key = 6;
    ff_cfg.color_logo = 3;

    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '/' && a[0] != '-') continue;
        a++;
        if (stricmp(a, "NOLOGO") == 0) ff_cfg.show_logo = 0;
        else if (stricmp(a, "NOCLS") == 0) ff_cfg.clear_screen = 0;
        else if (stricmp(a, "JSON") == 0) ff_cfg.json = 1;
        else if (stricmp(a, "GENCFG") == 0) gencfg = 1;
        else if (strnicmp(a, "SAVE=", 5) == 0) strncpy(ff_cfg.save_path, a + 5, sizeof(ff_cfg.save_path) - 1);
        else if (strnicmp(a, "LOGO=", 5) == 0) { strncpy(ff_cfg.logo_path, a + 5, sizeof(ff_cfg.logo_path) - 1); ff_cfg.show_logo = 1; }
        else if (strnicmp(a, "C=", 2) == 0) cfg_override = a + 2;
        else if (strnicmp(a, "MODULES=", 8) == 0) strncpy(ff_cfg.modules, a + 8, sizeof(ff_cfg.modules) - 1);
        else if (stricmp(a, "?") == 0 || stricmp(a, "H") == 0 || stricmp(a, "HELP") == 0) {
            printf("FFETCH - fastfetch-style system summary for DOS\r\n"
                   "  /NOLOGO        no ASCII logo\r\n"
                   "  /NOCLS         do not clear the screen\r\n"
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
    /* Command-line switches were parsed before the config load so a
     * config file cannot silently undo an explicit switch; re-apply
     * the two switches the config also knows about. */
    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '/' && a[0] != '-') continue;
        a++;
        if (stricmp(a, "NOLOGO") == 0) ff_cfg.show_logo = 0;
        else if (stricmp(a, "NOCLS") == 0) ff_cfg.clear_screen = 0;
        else if (strnicmp(a, "LOGO=", 5) == 0) { strncpy(ff_cfg.logo_path, a + 5, sizeof(ff_cfg.logo_path) - 1); ff_cfg.show_logo = 1; }
        else if (strnicmp(a, "MODULES=", 8) == 0) strncpy(ff_cfg.modules, a + 8, sizeof(ff_cfg.modules) - 1);
        else if (strnicmp(a, "SAVE=", 5) == 0) strncpy(ff_cfg.save_path, a + 5, sizeof(ff_cfg.save_path) - 1);
    }

    ff_cfg.stdout_is_console = probe_stdout_console();
    ff_cfg.use_ansi = !ff_cfg.json && ff_cfg.stdout_is_console && probe_ansi();

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
