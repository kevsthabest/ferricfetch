/*
 * FFETCH - a fastfetch-style system summary for DOS.
 *
 * The design rule for every module in this program: a line is printed
 * only when its data source actually answered. DOS machines vary too
 * much (an 8088 with no XMS, a laptop with APM, a Ferric machine with
 * a resident core) for any field to be filled in by assumption, so
 * each probe returns "no answer" and its line simply never appears.
 */
#ifndef FFETCH_H
#define FFETCH_H

#include <stddef.h>

/* One collected result line. Keys are short labels ("CPU"), values are
 * the finished display string. The table is filled by the probes in
 * module order, then rendered three possible ways (screen, JSON,
 * save-file) from the same data so the modes can never disagree. */
#define FF_MAX_LINES 56
#define FF_KEY_MAX 16
#define FF_VAL_MAX 108

struct ff_line {
    char key[FF_KEY_MAX];
    char value[FF_VAL_MAX];
};

extern struct ff_line ff_lines[FF_MAX_LINES];
extern int ff_nlines;

/* Append one line; silently drops the line when the table is full,
 * which is the correct behaviour for a summary: the important modules
 * run first. */
void ff_add(const char *key, const char *value);

/* Configuration gathered from FFETCH.CFG and the command line. */
struct ff_config {
    int show_logo;          /* draw the ASCII logo beside the lines */
    int clear_screen;       /* clear before drawing (console only) */
    int json;               /* machine-readable output instead */
    int allow_color;        /* 1 = allow colour on console (default) */
    int use_ansi;           /* filled in after the ANSI probe */
    int use_bios_color;     /* filled in when CON is active without ANSI */
    int stdout_is_console;  /* filled in after the IOCTL CON probe */
    unsigned char color_key;   /* colour number 0-7 for labels */
    unsigned char color_logo;  /* colour number 0-7 for the logo */
    char logo_path[80];     /* empty = built-in logo */
    char save_path[80];     /* empty = no save file */
    /* Module selection: an ordered list of module names chosen by the
     * config file or /MODULES. Empty = the built-in default order. */
    char modules[640];
};

extern struct ff_config ff_cfg;

/* Module entry points. Each returns nothing; a module that cannot get
 * an answer simply adds no line. */
void mod_os(void);
void mod_kernel(void);
void mod_cpu(void);
void mod_memory(void);
void mod_disk(void);
void mod_physdisk(void);
void mod_gpu(void);
void mod_display(void);
void mod_bios(void);
void mod_host(void);
void mod_board(void);
void mod_physmem(void);
void mod_battery(void);
void mod_power(void);
void mod_sound(void);
void mod_network(void);
void mod_shell(void);
void mod_title(void);
void mod_editor(void);
void mod_uptime(void);
void mod_datetime(void);
void mod_locale(void);
void mod_terminal(void);
void mod_processes(void);
void mod_gamepad(void);
void mod_bootmgr(void);
void mod_ferric(void);
void mod_wifi(void);
void mod_modules(void);

/* Environment lookup that reads the DOS environment block directly
 * (the C runtime copy is fine too, but this one is guaranteed to be
 * the shell's live block, which is where FreeCOM puts CONFIG).
 * ff_getenv_buf writes into a caller-supplied buffer (safe across
 * multiple lookups); ff_getenv is a convenience wrapper for a single
 * immediate read. Name matching is case-insensitive. */
int ff_getenv_buf(const char *name, char *out, size_t cap);
const char *ff_getenv(const char *name);

/* Format a KiB count into a short human string ("640 KB", "255.9 MB",
 * "16.0 GB"). Buffer must hold at least 16 bytes. */
void ff_fmt_kb(char *out, unsigned long kb);

/* Format a byte count the same way. */
void ff_fmt_bytes(char *out, unsigned long bytes);

/* The Ferric status verb (V86 API AX=0x0049) fills this structure on
 * cores that implement it; ferric.c owns the probe and the parsing. */
struct ff_ferric_status {
    int answered;           /* the verb returned a well-formed block */
    int wifi_present;
    int wifi_connected;
    int ip_valid;
    char ssid[33];
    unsigned char bssid[6];
    unsigned char channel;
    unsigned char ip[4];
    unsigned char mac[6];
    int jlm_count;
    struct {
        unsigned int ddb_id;
        char name[13];
    } jlm[8];
};

extern struct ff_ferric_status ff_fst;

/* Probe the Ferric core (INT 2Fh AX=1684h, device 0xFD01) and, when it
 * answers, the status verb. Returns 1 when the core is present.
 * Safe to call once; the result is cached in globals. */
int ff_ferric_probe(void);
extern int ff_ferric_present;
extern unsigned int ff_ferric_abi;    /* AX from the version call */

#endif
