/* FroggySoundTuner: sets the SF3500's built-in equalizer from a list of profiles.
 *
 * What it does and why (measured 2026-10-29 with a line capture, see the project notes):
 * the device's sound chip honours the volume and a 10-band equalizer; mute, balance,
 * bass/treble and the equalizer presets are ignored. The equalizer keeps its setting
 * after the program that set it exits, and is cleared when the device is powered off.
 * So a profile chosen here stays on while other programs run, until the next power-off.
 *
 * Text comes from TreeFrogUI's own language setting and fonts (frogui/settings.txt,
 * frogui/fonts), with this app's strings in lang/<locale>.json next to the program (same
 * flat JSON format as the front end's packs; a missing string falls back to English).
 *
 * Buttons: L1/R1 change tab, up/down choose, A applies, X plays a test sound, Y returns
 * to flat, B leaves. Select + Start also leaves.
 *
 * Test hooks (a PC / QEMU only): ST_NOHW=1 draws no screen and plays no sound,
 * ST_SDCARD=<dir> replaces /mnt/sdcard, ST_SND_DEV=<file> replaces the sound device,
 * ST_SCRIPT="D,D,A,..." feeds buttons, ST_SHOT=<prefix> saves a PPM after each step. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <arpa/inet.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H
#include "sndapi.h"

#include "hwdisp.h"
#include "hwaudio.h"
#include "joykey.h"
#include "tfenv.h"

static int g_w = 640, g_h = 480;      /* canvas size: chosen from the panel at start */
#define W g_w
#define H g_h
#define SR 48000
#define SND_DEV "/dev/sndC0i2so"
#define ST_VERSION "1.0"
#define NBANDS 10
#define MAXPROF 24
#define RAW_LIMIT 120            /* the vendor app's own range, about +-12 dB */
#define RAW_PER_DB 10            /* measured: about 0.1 dB per raw step */

static const char *g_sdcard = "/mnt/sdcard";
static char g_appdir[512] = ".";
static const char *g_snd_dev = SND_DEV;
static int g_nohw;
static volatile int g_quit;
static uint16_t *g_canvas;

static const int g_band_hz[NBANDS] = { 32, 64, 128, 250, 500, 1000, 2000, 4000, 8000, 16000 };

static int clampi_int(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- small helpers ------------------------------------------------------- */

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void on_signal(int s) { (void)s; g_quit = 1; }

static char *read_file(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (4L << 20) || !(b = (char *)malloc((size_t)n + 1))) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    b[n] = 0;
    fclose(f);
    if (len) *len = n;
    return b;
}

/* ---- translations: a flat UTF-8 JSON object of "key": "value" ----------------- */

typedef struct { char *k, *v; } KV;
typedef struct { KV *e; int n, cap; } Pack;
static Pack g_pack, g_en;

static void put_utf8(char **o, unsigned cp)
{
    char *p = *o;
    if (cp < 0x80) *p++ = (char)cp;
    else if (cp < 0x800) { *p++ = (char)(0xC0 | (cp >> 6)); *p++ = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { *p++ = (char)(0xE0 | (cp >> 12)); *p++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *p++ = (char)(0x80 | (cp & 0x3F)); }
    else { *p++ = (char)(0xF0 | (cp >> 18)); *p++ = (char)(0x80 | ((cp >> 12) & 0x3F)); *p++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *p++ = (char)(0x80 | (cp & 0x3F)); }
    *o = p;
}

static unsigned hex4(const char *s)
{
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    }
    return v;
}

/* reads one JSON string starting after its opening quote; returns the pointer after the
 * closing quote, and a malloc'd UTF-8 copy in *out */
static const char *json_string(const char *s, char **out)
{
    size_t cap = strlen(s) + 1;
    char *buf = (char *)malloc(cap), *o = buf;
    while (*s && *s != '"') {
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
            case 'n': *o++ = '\n'; s++; break;
            case 't': *o++ = ' '; s++; break;
            case 'u': {
                unsigned cp = hex4(s + 1);
                s += 5;
                if (cp >= 0xD800 && cp < 0xDC00 && s[0] == '\\' && s[1] == 'u') {
                    unsigned lo = hex4(s + 2);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    s += 6;
                }
                put_utf8(&o, cp);
                break;
            }
            default: *o++ = *s++; break;
            }
        } else {
            *o++ = *s++;
        }
    }
    *o = 0;
    *out = buf;
    return *s == '"' ? s + 1 : s;
}

static void pack_add(Pack *p, char *k, char *v)
{
    int i;
    for (i = 0; i < p->n; i++)
        if (!strcmp(p->e[i].k, k)) { free(p->e[i].v); p->e[i].v = v; free(k); return; }
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 128;
        p->e = (KV *)realloc(p->e, (size_t)p->cap * sizeof(KV));
    }
    p->e[p->n].k = k;
    p->e[p->n].v = v;
    p->n++;
}

static int pack_load(Pack *p, const char *path)
{
    char *txt = read_file(path, NULL);
    const char *s;
    if (!txt) return -1;
    s = txt;
    while (*s) {
        char *k, *v;
        while (*s && *s != '"') s++;
        if (!*s) break;
        s = json_string(s + 1, &k);
        while (*s && *s != ':' && *s != '"') s++;
        if (*s != ':') { free(k); break; }
        s++;
        while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++;
        if (*s != '"') { free(k); continue; }       /* not a string value: skip */
        s = json_string(s + 1, &v);
        pack_add(p, k, v);
    }
    free(txt);
    return 0;
}

static const char *pack_get(const Pack *p, const char *k)
{
    int i;
    for (i = 0; i < p->n; i++)
        if (!strcmp(p->e[i].k, k)) return p->e[i].v;
    return NULL;
}

static const char *tr(const char *key)
{
    const char *v = pack_get(&g_pack, key);
    if (!v) v = pack_get(&g_en, key);
    return v ? v : key;
}

/* ---- text: FreeType, TreeFrogUI's fonts, its Unicode font when a glyph is missing -- */

static FT_Library g_ft;
static FT_Face g_face;
static int g_bold;                         /* TreeFrogUnicode.ttf is thin at these sizes: drawn emboldened */
static int g_px = 24;

static unsigned utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p++;
    int n = 0;
    if (c < 0x80) { }
    else if (c >= 0xF0) { c &= 0x07; n = 3; }
    else if (c >= 0xE0) { c &= 0x0F; n = 2; }
    else if (c >= 0xC0) { c &= 0x1F; n = 1; }
    while (n-- && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
    *s = (const char *)p;
    return c;
}

static int face_has_all(FT_Face f, const Pack *p)
{
    int i;
    for (i = 0; i < p->n; i++) {
        const char *s = p->e[i].v;
        while (*s) {
            unsigned c = utf8_next(&s);
            if (c > ' ' && !FT_Get_Char_Index(f, c)) return 0;
        }
    }
    return 1;
}

static int open_face(const char *name, FT_Face *out)
{
    char path[512];
    snprintf(path, sizeof path, "%s/frogui/fonts/%s", g_sdcard, name);
    return FT_New_Face(g_ft, path, 0, out) == 0;
}

static void font_setup(void)
{
    const char *want = pack_get(&g_pack, "_default.font_name");
    FT_Face f = NULL;
    if (FT_Init_FreeType(&g_ft)) return;
    if (!want) want = "alium_Nunito.ttf";
    if (open_face(want, &f) && face_has_all(f, &g_pack) && face_has_all(f, &g_en)) {
        g_face = f;
    } else {
        if (f) FT_Done_Face(f);
        if (open_face("TreeFrogUnicode.ttf", &g_face)) g_bold = 1;     /* the front end's own fallback policy */
    }
    if (g_face) FT_Set_Pixel_Sizes(g_face, 0, (FT_UInt)g_px);
}

static void set_size(int px)
{
    if (g_face && px != g_px) { g_px = px; FT_Set_Pixel_Sizes(g_face, 0, (FT_UInt)px); }
}

/* loads a character, emboldened when the font is the thin one; render = also draw its bitmap */
static int load_glyph(unsigned c, int render)
{
    FT_UInt idx = FT_Get_Char_Index(g_face, c);
    if (FT_Load_Glyph(g_face, idx, FT_LOAD_DEFAULT)) return -1;
    if (g_bold) FT_GlyphSlot_Embolden(g_face->glyph);
    if (render && g_face->glyph->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(g_face->glyph, FT_RENDER_MODE_NORMAL)) return -1;
    return 0;
}

static int text_width(const char *s)
{
    int w = 0;
    if (!g_face) return (int)strlen(s) * 8;
    while (*s) {
        unsigned c = utf8_next(&s);
        if (load_glyph(c, 0) == 0)
            w += (int)(g_face->glyph->advance.x >> 6);
    }
    return w;
}

static uint16_t rgb(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

static int g_cy0 = 0, g_cy1 = 1 << 20;     /* only rows in [g_cy0, g_cy1) are drawn (scrolling text) */

static void blend_px(int x, int y, uint16_t col, int a)
{
    uint16_t d;
    int dr, dg, db, sr, sg, sb;
    if (x < 0 || y < g_cy0 || y >= g_cy1 || x >= W || y >= H || a <= 0) return;
    if (a >= 250) { g_canvas[y * W + x] = col; return; }
    d = g_canvas[y * W + x];
    dr = (d >> 11) & 31; dg = (d >> 5) & 63; db = d & 31;
    sr = (col >> 11) & 31; sg = (col >> 5) & 63; sb = col & 31;
    dr += (sr - dr) * a / 255; dg += (sg - dg) * a / 255; db += (sb - db) * a / 255;
    g_canvas[y * W + x] = (uint16_t)((dr << 11) | (dg << 5) | db);
}

/* draws one line with its baseline at y; returns the width drawn */
static int draw_text(int x, int y, uint16_t col, const char *s)
{
    int pen = x;
    if (!g_face) return 0;
    while (*s) {
        unsigned c = utf8_next(&s);
        FT_GlyphSlot g;
        int bx, by;
        if (load_glyph(c, 1)) continue;
        g = g_face->glyph;
        for (by = 0; by < (int)g->bitmap.rows; by++)
            for (bx = 0; bx < (int)g->bitmap.width; bx++)
                blend_px(pen + g->bitmap_left + bx, y - g->bitmap_top + by, col,
                         g->bitmap.buffer[by * g->bitmap.pitch + bx]);
        pen += (int)(g->advance.x >> 6);
    }
    return pen - x;
}

static void fill(int x, int y, int w, int h, uint16_t col)
{
    int i, j;
    for (j = y < g_cy0 ? g_cy0 : y; j < y + h && j < H && j < g_cy1; j++)
        for (i = x < 0 ? 0 : x; i < x + w && i < W; i++) g_canvas[j * W + i] = col;
}

/* text clipped to a width (drops characters, adds "..") */
static void draw_fit(int x, int y, int maxw, uint16_t col, const char *s)
{
    char buf[200];
    size_t n = strlen(s);
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    while (n > 3 && text_width(buf) > maxw) {
        n--;
        while (n > 0 && (buf[n] & 0xC0) == 0x80) n--;       /* never cut inside a character */
        buf[n] = 0;
        if (n > 2) { buf[n - 1] = '.'; buf[n - 2] = '.'; }
    }
    draw_text(x, y, col, buf);
}


static int glyph_adv(unsigned c)
{
    if (!g_face) return 8;
    if (load_glyph(c, 0)) return 0;
    return (int)(g_face->glyph->advance.x >> 6);
}

/* punctuation that closes a phrase and so may not begin a line */
static int closing_punct(unsigned c)
{
    return c == 0x3001 || c == 0x3002 || c == 0xFF0C || c == 0xFF0E || c == 0xFF01 || c == 0xFF1F ||
           c == 0x300D || c == 0x300F || c == 0x3011 || c == 0xFF09 || c == 0x30FC || c == 0x2026 ||
           c == ',' || c == '.' || c == '!' || c == '?' || c == ')' || c == ':' || c == ';';
}

/* draws a paragraph wrapped to a width (at spaces, or anywhere for languages without
 * spaces); returns the y of the next line. Honours the clip rows. */
static int draw_wrapped(int x, int y, int w, uint16_t col, const char *s)
{
    int lh = g_px + g_px / 3;
    const char *p = s;
    char line[300];
    while (*p) {
        const char *q = p, *lastsp = NULL, *end, *next;
        int lw = 0;
        while (*q && *q != '\n') {
            const char *n = q;
            unsigned c = utf8_next(&n);
            int cw = glyph_adv(c);
            if (lw + cw > w && q > p && !closing_punct(c)) break;      /* never start a line with ")" or "。" */
            if (c == ' ') lastsp = q;
            lw += cw;
            q = n;
        }
        if (*q && *q != '\n' && lastsp && lastsp > p) { end = lastsp; next = lastsp + 1; }
        else { end = q; next = (*q == '\n') ? q + 1 : q; }
        if ((size_t)(end - p) >= sizeof line) end = p + sizeof line - 1;
        memcpy(line, p, (size_t)(end - p));
        line[end - p] = 0;
        y += lh;
        draw_text(x, y - g_px / 4, col, line);
        p = next;
        if (!*p) break;
    }
    return y;
}

/* ---- the sound chip ------------------------------------------------------ */

static int snd_ioctl(unsigned long req, void *arg)
{
    static int fake = -1;
    int fd, rc, e;
    if (fake < 0) fake = getenv("ST_FAKESND") ? 1 : 0;   /* tests: pretend every command worked */
    if (fake) return 0;
    fd = open(g_snd_dev, O_WRONLY);
    if (fd < 0) return -1;
    errno = 0;
    rc = ioctl(fd, req, arg);
    e = errno;
    close(fd);
    if (rc < 0 && e == 0) rc = -1;
    return rc;
}

/* a profile: gains per band in raw units (about 0.1 dB each); all zero = flat */
typedef struct {
    char id[64];             /* built-in: key suffix; custom: the user's name */
    int builtin;
    int gain[NBANDS];
} Profile;

static Profile g_prof[MAXPROF];
static int g_nprof;

static void add_builtin(const char *id, int g32, int g64, int g128, int g250, int g500,
                        int g1k, int g2k, int g4k, int g8k, int g16k)
{
    Profile *p = &g_prof[g_nprof++];
    memset(p, 0, sizeof *p);
    snprintf(p->id, sizeof p->id, "%s", id);
    p->builtin = 1;
    p->gain[0] = g32; p->gain[1] = g64; p->gain[2] = g128; p->gain[3] = g250; p->gain[4] = g500;
    p->gain[5] = g1k; p->gain[6] = g2k; p->gain[7] = g4k; p->gain[8] = g8k; p->gain[9] = g16k;
}

static void build_profiles(void)
{
    /* the speaker cut is exactly the one measured on a device: 2 kHz -3 dB, 4 kHz -8 dB,
     * 8 kHz -3 dB gave -8 dB at 3-4 kHz, -5 dB at 2 and 5-7 kHz */
    add_builtin("flat", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    add_builtin("speaker_natural", 0, 0, 0, 0, 0, 0, -30, -80, -30, 0);
    add_builtin("speaker_bass", 0, 40, 60, 30, 0, 0, -30, -80, -30, 0);
    add_builtin("bass_lift", 40, 60, 60, 30, 0, 0, 0, 0, 0, 0);
    add_builtin("clear_voice", 0, 0, -20, -20, 0, 20, 40, 0, -20, 0);
}

static int g_active = -1;        /* index of the applied profile, -1 = none this session */
static char g_status[160];
static int g_status_err;

static void status(int err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof g_status, fmt, ap);
    va_end(ap);
    g_status_err = err;
}

static int apply_gains(const int *gain)
{
    int i, bad = 0, any = 0;
    for (i = 0; i < NBANDS; i++) {
        struct snd_eq_band_setting b;
        int gv = gain[i];
        if (gv > RAW_LIMIT) gv = RAW_LIMIT;
        if (gv < -RAW_LIMIT) gv = -RAW_LIMIT;
        if (gv) any = 1;
        b.band = i; b.cutoff = g_band_hz[i]; b.q = 14; b.gain = gv;
        if (snd_ioctl(SND_IOCTL_SET_EQ_BAND, &b) < 0) bad++;
    }
    if (snd_ioctl(SND_IOCTL_SET_EQ_ONOFF, (void *)(long)(any ? 1 : 0)) < 0) bad++;
    return bad ? -1 : 0;
}

static const char *prof_name(const Profile *p, char *buf, size_t n)
{
    char key[64];
    if (!p->builtin) return p->id;
    snprintf(key, sizeof key, "profile.%s", p->id);
    snprintf(buf, n, "%s", tr(key));
    return buf;
}

static void apply_profile(int i)
{
    char nm[96];
    const char *name = prof_name(&g_prof[i], nm, sizeof nm);
    if (apply_gains(g_prof[i].gain) < 0) {
        status(1, "%s", tr("status.error"));
        return;
    }
    g_active = i;
    status(0, "%s: %s", tr("status.applied"), name);
}

/* ---- Speaker Fix: are earphones plugged in, and the shared setting --------------- */

/* The stock firmware drives one GPIO pin from the jack: /proc/device-tree/panel/
 * speaker-output names it, and it reads HIGH with earphones in (measured on the SF3500).
 * The bank's level register is mapped read-only; nothing is written. */
static volatile uint32_t *g_hp_level;
static int g_hp_pin = -1;

static void hp_init(void)
{
    unsigned char buf[8];
    uint32_t v;
    int fd, n;
    void *m;
    if (getenv("ST_HP")) return;                       /* tests */
    fd = open("/proc/device-tree/panel/speaker-output", O_RDONLY);
    if (fd < 0) return;
    n = (int)read(fd, buf, sizeof buf);
    close(fd);
    if (n < 4) return;
    memcpy(&v, buf, 4);
    v = ntohl(v);
    if (v > 31) return;
    fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) return;
    m = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, 0x18800000);
    close(fd);
    if (m == MAP_FAILED) return;
    g_hp_pin = (int)v;
    g_hp_level = (volatile uint32_t *)((char *)m + 0x44 + 4 * 4);
}

/* 1 earphones in, 0 speakers in use, -1 cannot tell */
static int hp_plugged(void)
{
    const char *e = getenv("ST_HP");
    if (e) return atoi(e);
    if (!g_hp_level) return -1;
    return ((*g_hp_level >> g_hp_pin) & 1u) ? 1 : 0;
}

enum { FIX_OFF, FIX_ON, FIX_AUTO };
static int g_fix = FIX_OFF;          /* the saved setting */
static int g_fixsel;                 /* the cursor on the Speaker Fix tab */

/* the one setting other programs read: /mnt/sdcard/speaker_fix.txt, "speaker_fix=auto|on|off" */
static void fix_path(char *o, size_t n) { snprintf(o, n, "%s/speaker_fix.txt", g_sdcard); }

static void fix_load(void)
{
    char path[600], *txt;
    fix_path(path, sizeof path);
    txt = read_file(path, NULL);
    if (!txt) return;
    if (strstr(txt, "speaker_fix=auto")) g_fix = FIX_AUTO;
    else if (strstr(txt, "speaker_fix=on")) g_fix = FIX_ON;
    else g_fix = FIX_OFF;
    free(txt);
}

static int fix_save(void)
{
    char path[600], tmp[620];
    static const char *names[] = { "off", "on", "auto" };
    FILE *f;
    fix_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "speaker_fix=%s\n", names[g_fix]);
    if (fclose(f) != 0 || rename(tmp, path) != 0) return -1;
    return 0;
}

/* should left and right be mixed together right now? */
static int fix_active(void)
{
    if (g_fix == FIX_ON) return 1;
    if (g_fix == FIX_AUTO) return hp_plugged() == 0;
    return 0;
}

/* ---- test sound: a slow sweep that goes through the speaker's whole range --------- */

static int g_testing;
static int g_test_mode;              /* 0 sweep (Presets, Equalizer), 1 left/right bursts (Speaker Fix) */
static int g_test_chan = -1;         /* while the bursts play: 0 left, 1 right, 2 both, -1 rest */
static double g_phase, g_tpos;

static void test_feed(void)
{
    unsigned u, d, f;
    short buf[HWAUDIO_CHUNK * 2];
    if (g_nohw || !g_testing) return;
    hwaudio_get_stats(&u, &d, &f);
    while (f < 3840) {
        int i, n;
        for (i = 0; i < HWAUDIO_CHUNK; i++) {
            short v, l, r;
            if (g_test_mode) {
                /* 1 kHz bursts: left, right, both, each 1.2 s with a short rest, level about -20 dBFS */
                double cyc = fmod(g_tpos, 4.2);
                int part = (int)(cyc / 1.4);
                double in = cyc - part * 1.4;
                g_phase += 2 * M_PI * 1000.0 / SR;
                if (g_phase > 2 * M_PI) g_phase -= 2 * M_PI;
                v = in < 1.2 ? (short)(0.10 * 32767.0 * sin(g_phase)) : 0;
                l = (part == 0 || part == 2) ? v : 0;
                r = (part == 1 || part == 2) ? v : 0;
                g_test_chan = in < 1.2 ? part : -1;
                if (fix_active()) { short m = (short)((l + r) / 2); l = r = m; }
            } else {
                /* 80 Hz .. 12 kHz in 10 s, then again; level about -20 dBFS */
                double frac = fmod(g_tpos, 10.0) / 10.0;
                double fr = 80.0 * pow(12000.0 / 80.0, frac);
                g_phase += 2 * M_PI * fr / SR;
                if (g_phase > 2 * M_PI) g_phase -= 2 * M_PI;
                v = (short)(0.10 * 32767.0 * sin(g_phase));
                l = r = v;
            }
            buf[i * 2] = l;
            buf[i * 2 + 1] = r;
            g_tpos += 1.0 / SR;
        }
        n = hwaudio_write(buf, HWAUDIO_CHUNK);
        if (n <= 0) break;
        f += (unsigned)n;
    }
}

/* ---- screen ------------------------------------------------------------- */

enum { TAB_PROFILES, TAB_CUSTOM, TAB_BLEND, TAB_ABOUT, NTABS };
static int g_tab = TAB_PROFILES;
static int g_sel;                         /* profile list cursor */
static int g_bandsel;                     /* custom tab: band cursor */
static int g_edit[NBANDS];                /* custom tab: gains being tried */

static uint16_t C_BG, C_PANEL, C_ACCENT, C_TEXT, C_DIM, C_OK, C_ERR, C_BAR;

static void colours(void)
{
    C_BG = rgb(16, 20, 30);
    C_PANEL = rgb(30, 38, 56);
    C_ACCENT = rgb(60, 190, 160);
    C_TEXT = rgb(235, 238, 245);
    C_DIM = rgb(140, 150, 170);
    C_OK = rgb(120, 220, 140);
    C_ERR = rgb(255, 120, 110);
    C_BAR = rgb(70, 82, 110);
}

/* Every size below is written for a 480-pixel-high canvas and scaled to the real one;
 * widths come from the real width, so the same layout fits 4:3, 16:9 and other panels. */
static int U(int v) { return (v * g_h + 240) / 480; }
static void font(int base)
{
    int px = U(base);
    set_size(px < 13 ? 13 : px);
}

static void draw_tabs(void)
{
    static const char *keys[NTABS] = { "tab.profiles", "tab.custom", "tab.blend", "tab.about" };
    int i, x, gap = U(8), pad = U(14), base = 22, total;
    int w[NTABS];
    fill(0, 0, W, U(56), C_PANEL);
    for (;;) {                       /* shrink the labels until all four fit (long translations) */
        font(base);
        total = U(16) * 2;
        for (i = 0; i < NTABS; i++) { w[i] = text_width(tr(keys[i])) + pad * 2; total += w[i] + gap; }
        if (total <= W || base <= 14) break;
        base--;
    }
    x = U(16);
    for (i = 0; i < NTABS; i++) {
        int ty = U(56) / 2 + g_px / 3;
        if (i == g_tab) {
            fill(x, U(8), w[i], U(40), C_ACCENT);
            draw_text(x + pad, ty, C_BG, tr(keys[i]));
        } else {
            draw_text(x + pad, ty, C_DIM, tr(keys[i]));
        }
        x += w[i] + gap;
    }
}

#define FOOTER_H U(66)

static void draw_legend(const char *a, const char *b, const char *c)
{
    char line[200];
    int base = 17;
    line[0] = 0;
    if (a[0]) snprintf(line + strlen(line), sizeof line - strlen(line), "%s    ", a);
    if (b[0]) snprintf(line + strlen(line), sizeof line - strlen(line), "%s    ", b);
    if (c[0]) snprintf(line + strlen(line), sizeof line - strlen(line), "%s", c);
    fill(0, H - FOOTER_H, W, FOOTER_H, C_PANEL);
    font(base);
    draw_fit(U(14), H - FOOTER_H * 57 / 100, W - U(28), C_TEXT, line);
    snprintf(line, sizeof line, "%s    %s    %s", tr("legend.tabs"), tr("legend.back"), tr("legend.exit"));
    draw_fit(U(14), H - FOOTER_H / 9, W - U(28), C_DIM, line);
}

static void draw_status(void)
{
    if (!g_status[0]) return;
    font(20);
    draw_fit(U(20), H - FOOTER_H - U(12), W - U(40), g_status_err ? C_ERR : C_OK, g_status);
}

static void draw_profiles(void)
{
    int i, rowh = U(56), y0 = U(66), y, vis, first = 0, stop = H - FOOTER_H - U(40);
    char nm[96], key[64];
    vis = (stop - y0) / rowh;
    if (vis < 1) vis = 1;
    if (g_sel >= vis) first = g_sel - vis + 1;       /* keep the cursor in view */
    y = y0;
    for (i = first; i < g_nprof && i < first + vis; i++) {
        const Profile *p = &g_prof[i];
        const char *name = prof_name(p, nm, sizeof nm);
        int sel = (i == g_sel);
        fill(U(12), y, W - U(30), rowh - U(4), sel ? C_PANEL : C_BG);
        if (sel) fill(U(12), y, U(6), rowh - U(4), C_ACCENT);
        font(24);
        draw_fit(U(30), y + U(26), W - U(140), C_TEXT, name);
        snprintf(key, sizeof key, "profile.%s.desc", p->builtin ? p->id : "custom");
        font(16);
        draw_fit(U(30), y + U(46), W - U(140), C_DIM, tr(key));
        if (i == g_active) {
            font(24);
            draw_text(W - U(110), y + U(34), C_OK, tr("status.on"));
        }
        y += rowh;
    }
    if (g_nprof > vis) {                              /* a thin scroll bar at the right edge */
        int th = (stop - y0), bh = th * vis / g_nprof, by = y0 + (th - bh) * first / (g_nprof - vis);
        fill(W - U(12), y0, U(4), th, C_BAR);
        fill(W - U(12), by, U(4), bh, C_ACCENT);
    }
    if (g_testing) {
        font(18);
        draw_fit(U(20), H - FOOTER_H - U(40), W - U(40), C_ACCENT, tr("test.playing"));
    }
    draw_status();
    {
        char l1[120];
        snprintf(l1, sizeof l1, "%s    %s", tr("legend.apply"), tr("legend.options"));
        draw_legend(l1, g_testing ? tr("legend.test_stop") : tr("legend.test"), tr("legend.flat"));
    }
}

/* custom tab: ten bars, one per band */
static void draw_custom(void)
{
    int i, margin = U(24), top = U(84), hgt = U(176), mid, slot, bw;
    char t[64];
    slot = (W - margin * 2) / NBANDS;
    bw = slot - U(12);
    mid = top + hgt / 2;
    font(18);
    draw_fit(U(20), U(76), W - U(40), C_DIM, tr("custom.hint"));
    fill(margin - U(6), mid, slot * NBANDS, 2, C_BAR);
    for (i = 0; i < NBANDS; i++) {
        int x = margin + i * slot + U(6);
        int v = g_edit[i], px = v * (hgt / 2) / RAW_LIMIT;
        fill(x, top, bw, hgt, i == g_bandsel ? C_PANEL : C_BG);
        if (px >= 0) fill(x + U(6), mid - px, bw - U(12), px + 2, i == g_bandsel ? C_ACCENT : C_BAR);
        else fill(x + U(6), mid, bw - U(12), -px, i == g_bandsel ? C_ACCENT : C_BAR);
        font(16);
        if (g_band_hz[i] >= 1000) snprintf(t, sizeof t, "%dk", g_band_hz[i] / 1000);
        else snprintf(t, sizeof t, "%d", g_band_hz[i]);
        draw_text(x + (bw - text_width(t)) / 2, top + hgt + U(22), i == g_bandsel ? C_TEXT : C_DIM, t);
    }
    {
        static const char *cap[3] = { "eq.bass", "eq.mid", "eq.treble" };
        static const int from[3] = { 0, 3, 7 }, to[3] = { 2, 6, 9 };
        int k;
        font(16);
        for (k = 0; k < 3; k++) {
            int x0 = margin + from[k] * slot, x1 = margin + (to[k] + 1) * slot;
            fill(x0 + U(6), top + hgt + U(30), x1 - x0 - U(12), 2, C_BAR);
            draw_text(x0 + (x1 - x0 - text_width(tr(cap[k]))) / 2, top + hgt + U(50), C_DIM, tr(cap[k]));
        }
    }
    snprintf(t, sizeof t, "%+.1f dB", g_edit[g_bandsel] / (double)RAW_PER_DB);
    font(26);
    draw_text(U(20), top + hgt + U(88), C_TEXT, t);
    draw_status();
    {
        char l1[120];
        snprintf(l1, sizeof l1, "%s    %s", tr("legend.custom_apply"), tr("legend.save"));
        draw_legend(l1, g_testing ? tr("legend.test_stop") : tr("legend.test"), tr("legend.flat"));
    }
}

static void draw_blend(void)
{
    static const char *name[] = { "fix.off", "fix.on", "fix.auto" };
    static const char *desc[] = { "fix.off.desc", "fix.on.desc", "fix.auto.desc" };
    static const int order[3] = { FIX_AUTO, FIX_ON, FIX_OFF };      /* recommended first */
    int i, y, rowh = U(46), hp = hp_plugged();
    const char *now;
    g_cy0 = U(56); g_cy1 = H - FOOTER_H;
    font(16);
    y = draw_wrapped(U(20), U(58), W - U(40), C_DIM, tr("fix.intro"));
    g_cy0 = 0; g_cy1 = 1 << 20;
    y += U(4);
    for (i = 0; i < 3; i++) {
        int m = order[i], sel = (i == g_fixsel);
        fill(U(12), y, W - U(24), rowh - U(4), sel ? C_PANEL : C_BG);
        if (sel) fill(U(12), y, U(6), rowh - U(4), C_ACCENT);
        font(22);
        draw_fit(U(30), y + U(22), W - U(130), C_TEXT, tr(name[m]));
        font(15);
        draw_fit(U(30), y + U(40), W - U(130), C_DIM, tr(desc[m]));
        if (m == g_fix) { font(22); draw_text(W - U(100), y + U(30), C_OK, tr("status.on")); }
        y += rowh;
    }
    now = hp == 1 ? tr("fix.now_phones") : hp == 0 ? tr("fix.now_speakers") : tr("fix.now_unknown");
    font(17);
    draw_fit(U(20), y + U(18), W - U(40), C_TEXT, now);
    if (g_testing && g_test_mode) {
        static const char *ch[] = { "fix.test_left", "fix.test_right", "fix.test_both" };
        char line[160];
        snprintf(line, sizeof line, "%s %s", tr("fix.test_now"), g_test_chan >= 0 ? tr(ch[g_test_chan]) : "...");
        font(18);
        draw_fit(U(20), y + U(40), W - U(40), C_ACCENT, line);
    } else {
        g_cy0 = 0; g_cy1 = H - FOOTER_H - U(30);
        font(15);
        draw_wrapped(U(20), y + U(24), W - U(40), C_DIM, tr("fix.scope"));
    }
    g_cy0 = 0; g_cy1 = 1 << 20;
    draw_status();
    {
        char l1[120];
        snprintf(l1, sizeof l1, "%s    %s", tr("legend.apply"), g_testing && g_test_mode ? tr("legend.test_stop") : tr("fix.legend_test"));
        draw_legend(l1, "", "");
    }
}

static int g_about_scroll, g_about_h;

static void draw_about(void)
{
    static const char *items[][2] = {         /* kind: h = heading, p = paragraph */
        { "t", "about.title" }, { "p", "about.tagline" },
        { "h", "about.h1" }, { "p", "about.p1" },
        { "h", "about.h2" }, { "p", "about.p2" }, { "p", "about.p3" }, { "p", "about.p4" },
        { "h", "about.h3" }, { "p", "about.p5" },
    };
    int i, top = U(60), bottom = H - FOOTER_H, y, view = bottom - top, mw = W - U(48);
    g_cy0 = top; g_cy1 = bottom;
    y = top + U(6) - g_about_scroll;
    for (i = 0; i < (int)(sizeof items / sizeof items[0]); i++) {
        char k = items[i][0][0];
        if (k == 't') {
            char title[96];
            snprintf(title, sizeof title, "%s %s", tr(items[i][1]), ST_VERSION);
            font(28);
            y = draw_wrapped(U(24), y, mw, C_ACCENT, title);
            y += U(4);
        }
        else if (k == 'h') { y += U(8); font(20); y = draw_wrapped(U(24), y, mw, C_TEXT, tr(items[i][1])); }
        else { font(17); y = draw_wrapped(U(24), y, mw, C_DIM, tr(items[i][1])); y += U(4); }
    }
    g_about_h = y + g_about_scroll - top + U(8);
    g_cy0 = 0; g_cy1 = 1 << 20;
    if (g_about_h > view) {                         /* scroll bar */
        int bh = view * view / g_about_h, by = top + (view - bh) * g_about_scroll / (g_about_h - view);
        fill(W - U(10), top, U(4), view, C_BAR);
        fill(W - U(10), by, U(4), bh, C_ACCENT);
    }
    draw_legend(g_about_h > view ? tr("legend.scroll") : "", "", tr("legend.flat"));
}

static void draw_all(void)
{
    fill(0, 0, W, H, C_BG);
    draw_tabs();
    switch (g_tab) {
    case TAB_PROFILES: draw_profiles(); break;
    case TAB_CUSTOM: draw_custom(); break;
    case TAB_BLEND: draw_blend(); break;
    default: draw_about(); break;
    }
}

/* ---- input --------------------------------------------------------------- */

enum { B_LEFT, B_RIGHT, B_UP, B_DOWN, B_A, B_B, B_X, B_Y, B_L1, B_R1, B_SELECT, B_START, B_QUIT, B_COUNT };

static unsigned map_buttons(unsigned jk)
{
    unsigned m = 0;
    if (jk & (1u << JK_LEFT)) m |= 1u << B_LEFT;
    if (jk & (1u << JK_RIGHT)) m |= 1u << B_RIGHT;
    if (jk & (1u << JK_UP)) m |= 1u << B_UP;
    if (jk & (1u << JK_DOWN)) m |= 1u << B_DOWN;
    if (jk & (1u << JK_A)) m |= 1u << B_A;
    if (jk & (1u << JK_B)) m |= 1u << B_B;
    if (jk & (1u << JK_X)) m |= 1u << B_X;
    if (jk & (1u << JK_Y)) m |= 1u << B_Y;
    if (jk & (1u << JK_L1)) m |= 1u << B_L1;
    if (jk & (1u << JK_R1)) m |= 1u << B_R1;
    if (jk & (1u << JK_SELECT)) m |= 1u << B_SELECT;
    if (jk & (1u << JK_START)) m |= 1u << B_START;
    if ((jk & (1u << JK_SELECT)) && (jk & (1u << JK_START))) m |= 1u << B_QUIT;
    return m;
}


/* ---- custom profiles: saved in profiles.txt next to the program ------------------ */

static int g_editing = -1;          /* custom tab: the custom profile being edited, -1 = a new one */

static void profiles_path(char *o, size_t n) { snprintf(o, n, "%s/profiles.txt", g_appdir); }

static void save_profiles(void)
{
    char path[600], tmp[620];
    FILE *f;
    int i, k;
    profiles_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) { status(1, "%s", tr("status.save_failed")); return; }
    for (i = 0; i < g_nprof; i++) {
        if (g_prof[i].builtin) continue;
        fprintf(f, "%s\t", g_prof[i].id);
        for (k = 0; k < NBANDS; k++) fprintf(f, "%s%d", k ? "," : "", g_prof[i].gain[k]);
        fprintf(f, "\n");
    }
    if (fclose(f) != 0 || rename(tmp, path) != 0) status(1, "%s", tr("status.save_failed"));
}

static int add_custom(const char *name, const int *gain)
{
    Profile *p;
    int k;
    if (g_nprof >= MAXPROF) return -1;
    p = &g_prof[g_nprof];
    memset(p, 0, sizeof *p);
    snprintf(p->id, sizeof p->id, "%s", name);
    for (k = 0; (size_t)k < strlen(p->id); k++) if (p->id[k] == '\t' || p->id[k] == '\n' || p->id[k] == '\r') p->id[k] = ' ';
    for (k = 0; k < NBANDS; k++) p->gain[k] = clampi_int(gain[k], -RAW_LIMIT, RAW_LIMIT);
    return g_nprof++;
}

static void load_profiles(void)
{
    char path[600], *txt, *line, *save = NULL;
    profiles_path(path, sizeof path);
    txt = read_file(path, NULL);
    if (!txt) return;
    for (line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *tab = strchr(line, '\t'), *q;
        int gain[NBANDS] = { 0 }, k = 0;
        if (!tab || tab == line) continue;
        *tab = 0;
        for (q = tab + 1; *q && k < NBANDS; k++) {
            gain[k] = atoi(q);
            while (*q && *q != ',') q++;
            if (*q == ',') q++;
        }
        add_custom(line, gain);
    }
    free(txt);
}

static void load_edit_from_active(void)
{
    int i;
    if (g_active >= 0) for (i = 0; i < NBANDS; i++) g_edit[i] = g_prof[g_active].gain[i];
    g_editing = (g_active >= 0 && !g_prof[g_active].builtin) ? g_active : -1;
}

static void flat_now(void)
{
    int zero[NBANDS] = { 0 }, i;
    if (apply_gains(zero) < 0) { status(1, "%s", tr("status.error")); return; }
    g_active = 0;                                  /* "Flat" is the first profile */
    g_editing = -1;
    for (i = 0; i < NBANDS; i++) g_edit[i] = 0;
    status(0, "%s", tr("status.flat"));
}


/* ---- remembering what is on: the equalizer outlives this program but not a power-off --- */

/* The chip keeps the last setting until the device is turned off, and cannot be asked
 * what it holds, so on exit the app notes which preset it left on, together with this
 * boot's id. On the next start the note is believed only if it is from the same boot. */
static void boot_id(char *o, size_t n)
{
    const char *e = getenv("ST_BOOTID");
    char *t;
    o[0] = 0;
    if (e) { snprintf(o, n, "%s", e); return; }
    t = read_file("/proc/sys/kernel/random/boot_id", NULL);
    if (!t) return;
    snprintf(o, n, "%s", t);
    o[strcspn(o, "\r\n")] = 0;
    free(t);
}

static void state_path(char *o, size_t n) { snprintf(o, n, "%s/state.txt", g_appdir); }

static void state_save(void)
{
    char path[600], tmp[620], id[80];
    FILE *f;
    int k;
    boot_id(id, sizeof id);
    if (!id[0]) return;
    state_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "boot=%s\nactive=%s\ngains=", id, g_active >= 0 ? g_prof[g_active].id : "");
    for (k = 0; k < NBANDS; k++) fprintf(f, "%s%d", k ? "," : "", g_edit[k]);
    fprintf(f, "\nbuiltin=%d\n", g_active >= 0 ? g_prof[g_active].builtin : 0);
    if (fclose(f) == 0) rename(tmp, path);
}

static void state_load(void)
{
    char path[600], id[80], *txt, *line, *save = NULL, act[64] = "";
    int builtin = 0, have = 0, gains[NBANDS] = { 0 }, i;
    boot_id(id, sizeof id);
    state_path(path, sizeof path);
    txt = read_file(path, NULL);
    if (!txt || !id[0]) { free(txt); return; }
    for (line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "boot=", 5)) have = !strcmp(line + 5, id);
        else if (!strncmp(line, "active=", 7)) snprintf(act, sizeof act, "%s", line + 7);
        else if (!strncmp(line, "builtin=", 8)) builtin = atoi(line + 8);
        else if (!strncmp(line, "gains=", 6)) {
            char *q = line + 6;
            int k;
            for (k = 0; *q && k < NBANDS; k++) {
                gains[k] = atoi(q);
                while (*q && *q != ',') q++;
                if (*q == ',') q++;
            }
        }
    }
    free(txt);
    if (!have) return;                                  /* from another boot: the chip is flat now */
    for (i = 0; i < NBANDS; i++) g_edit[i] = gains[i];
    if (act[0])
        for (i = 0; i < g_nprof; i++)
            if (g_prof[i].builtin == builtin && !strcmp(g_prof[i].id, act)) { g_active = i; g_sel = i; break; }
}

/* ---- the pop-ups: options for a profile, naming one, deleting one ----------------- */

enum { M_NONE, M_OPTIONS, M_NAME, M_DELETE };
enum { O_EDIT, O_COPY, O_RENAME, O_DELETE, O_CANCEL };
static int g_modal, g_opt_sel, g_opt_n, g_opt[5], g_target;      /* g_target: the profile it is about */
static char g_name[64];
static int g_name_for = -1;                                      /* profile being renamed, -1 = a new one */
static int g_gcur;
static char g_cell[240][5];
static int g_ncell, g_cols = 12;

#define NAME_MAX_CHARS 22
#define NAME_MAX_BYTES 60

static int utf8_count(const char *s) { int n = 0; while (*s) { utf8_next(&s); n++; } return n; }

static void build_cells(void)
{
    const char *s = tr("keyboard.chars");
    g_ncell = 0;
    while (*s && g_ncell < 240) {
        const char *st = s;
        size_t n;
        utf8_next(&s);
        n = (size_t)(s - st);
        if (n < sizeof g_cell[0]) { memcpy(g_cell[g_ncell], st, n); g_cell[g_ncell][n] = 0; g_ncell++; }
    }
    if (g_ncell == 0) { strcpy(g_cell[0], "A"); g_ncell = 1; }
}

static void name_add(void)
{
    const char *c = g_cell[g_gcur];
    if (utf8_count(g_name) >= NAME_MAX_CHARS || strlen(g_name) + strlen(c) > NAME_MAX_BYTES) return;
    strcat(g_name, c);
}

static void name_erase(void)
{
    size_t n = strlen(g_name);
    if (!n) return;
    n--;
    while (n > 0 && (g_name[n] & 0xC0) == 0x80) n--;
    g_name[n] = 0;
}

static void open_name(int for_profile)
{
    char buf[64];
    int count = 1, i;
    g_name_for = for_profile;
    if (for_profile >= 0) snprintf(g_name, sizeof g_name, "%s", g_prof[for_profile].id);
    else {
        for (i = 0; i < g_nprof; i++) if (!g_prof[i].builtin) count++;
        snprintf(buf, sizeof buf, "%s %d", tr("custom.default_name"), count);
        snprintf(g_name, sizeof g_name, "%s", buf);
    }
    build_cells();
    g_cols = (W - U(40)) / U(44);
    if (g_cols < 6) g_cols = 6;
    g_gcur = 0;
    g_modal = M_NAME;
}

static void open_options(int idx)
{
    g_target = idx;
    g_opt_n = 0;
    if (g_prof[idx].builtin) g_opt[g_opt_n++] = O_COPY;
    else { g_opt[g_opt_n++] = O_EDIT; g_opt[g_opt_n++] = O_RENAME; g_opt[g_opt_n++] = O_DELETE; }
    g_opt[g_opt_n++] = O_CANCEL;
    g_opt_sel = 0;
    g_modal = M_OPTIONS;
}

static void delete_profile(int idx)
{
    int i;
    if (idx < 0 || idx >= g_nprof || g_prof[idx].builtin) return;
    for (i = idx; i < g_nprof - 1; i++) g_prof[i] = g_prof[i + 1];
    g_nprof--;
    if (g_active == idx) g_active = -1; else if (g_active > idx) g_active--;
    if (g_editing == idx) g_editing = -1; else if (g_editing > idx) g_editing--;
    if (g_sel >= g_nprof) g_sel = g_nprof - 1;
    save_profiles();
    status(0, "%s", tr("status.deleted"));
}

static void save_current_edit(void)
{
    if (g_editing >= 0) {
        memcpy(g_prof[g_editing].gain, g_edit, sizeof g_edit);
        save_profiles();
        g_active = g_editing;
        status(0, "%s", tr("status.saved"));
    } else {
        open_name(-1);
    }
}

static void name_done(void)
{
    if (!g_name[0]) return;
    if (g_name_for >= 0) {
        snprintf(g_prof[g_name_for].id, sizeof g_prof[g_name_for].id, "%s", g_name);
        save_profiles();
        status(0, "%s", tr("status.saved"));
    } else {
        int idx = add_custom(g_name, g_edit);
        if (idx < 0) { status(1, "%s", tr("status.full")); }
        else {
            g_editing = idx;
            g_active = idx;
            apply_gains(g_prof[idx].gain);
            save_profiles();
            status(0, "%s", tr("status.saved"));
        }
    }
    g_modal = M_NONE;
}

static void modal_press(int b)
{
    if (g_modal == M_OPTIONS) {
        if (b == B_UP) g_opt_sel = clampi_int(g_opt_sel - 1, 0, g_opt_n - 1);
        else if (b == B_DOWN) g_opt_sel = clampi_int(g_opt_sel + 1, 0, g_opt_n - 1);
        else if (b == B_B || b == B_SELECT) g_modal = M_NONE;
        else if (b == B_A) {
            int o = g_opt[g_opt_sel], i;
            g_modal = M_NONE;
            if (o == O_EDIT || o == O_COPY) {
                for (i = 0; i < NBANDS; i++) g_edit[i] = g_prof[g_target].gain[i];
                g_editing = o == O_EDIT ? g_target : -1;
                g_tab = TAB_CUSTOM;
            } else if (o == O_RENAME) open_name(g_target);
            else if (o == O_DELETE) g_modal = M_DELETE;
        }
    } else if (g_modal == M_NAME) {
        int rows = (g_ncell + g_cols - 1) / g_cols;
        if (b == B_LEFT) g_gcur = (g_gcur + g_ncell - 1) % g_ncell;
        else if (b == B_RIGHT) g_gcur = (g_gcur + 1) % g_ncell;
        else if (b == B_UP) { g_gcur -= g_cols; if (g_gcur < 0) g_gcur = clampi_int(g_gcur + rows * g_cols, 0, g_ncell - 1); }
        else if (b == B_DOWN) { g_gcur += g_cols; if (g_gcur >= g_ncell) g_gcur %= g_cols; }
        else if (b == B_A) name_add();
        else if (b == B_X) name_erase();
        else if (b == B_START) name_done();
        else if (b == B_B) g_modal = M_NONE;
    } else if (g_modal == M_DELETE) {
        if (b == B_A) { g_modal = M_NONE; delete_profile(g_target); }
        else if (b == B_B) g_modal = M_NONE;
    }
}

static void press(int b)
{
    if (b == B_QUIT) { g_quit = 1; return; }
    if (g_modal) { modal_press(b); return; }
    if (b == B_L1 || b == B_R1) {
        g_tab = (g_tab + (b == B_L1 ? NTABS - 1 : 1)) % NTABS;
        g_testing = 0;                           /* a test belongs to the tab it was started on */
        if (g_tab == TAB_CUSTOM) load_edit_from_active();
        return;
    }
    if (b == B_B) { g_quit = 1; return; }
    if (b == B_Y) { flat_now(); return; }
    if (b == B_X && (g_tab == TAB_PROFILES || g_tab == TAB_CUSTOM || g_tab == TAB_BLEND)) {
        g_testing = !g_testing;
        g_test_mode = (g_tab == TAB_BLEND);
        return;
    }
    if (g_tab == TAB_BLEND) {
        static const int order[3] = { FIX_AUTO, FIX_ON, FIX_OFF };
        if (b == B_UP) g_fixsel = clampi_int(g_fixsel - 1, 0, 2);
        else if (b == B_DOWN) g_fixsel = clampi_int(g_fixsel + 1, 0, 2);
        else if (b == B_A) {
            g_fix = order[g_fixsel];
            if (fix_save() == 0) status(0, "%s", tr("status.saved"));
            else status(1, "%s", tr("status.save_failed"));
        }
        return;
    }
    if (g_tab == TAB_ABOUT) {
        int view = H - FOOTER_H - U(60);
        if (b == B_UP) g_about_scroll = clampi_int(g_about_scroll - U(36), 0, g_about_h > view ? g_about_h - view : 0);
        else if (b == B_DOWN) g_about_scroll = clampi_int(g_about_scroll + U(36), 0, g_about_h > view ? g_about_h - view : 0);
        return;
    }
    if (g_tab == TAB_PROFILES) {
        if (b == B_UP) g_sel = clampi_int(g_sel - 1, 0, g_nprof - 1);
        else if (b == B_DOWN) g_sel = clampi_int(g_sel + 1, 0, g_nprof - 1);
        else if (b == B_A) apply_profile(g_sel);
        else if (b == B_SELECT) open_options(g_sel);
    } else if (g_tab == TAB_CUSTOM) {
        if (b == B_LEFT) g_bandsel = clampi_int(g_bandsel - 1, 0, NBANDS - 1);
        else if (b == B_RIGHT) g_bandsel = clampi_int(g_bandsel + 1, 0, NBANDS - 1);
        else if (b == B_UP || b == B_DOWN) {
            g_edit[g_bandsel] = clampi_int(g_edit[g_bandsel] + (b == B_UP ? RAW_PER_DB : -RAW_PER_DB), -RAW_LIMIT, RAW_LIMIT);
            if (apply_gains(g_edit) < 0) status(1, "%s", tr("status.error"));
            else { g_active = -1; status(0, "%s", tr("status.custom_on")); }
        } else if (b == B_A) save_current_edit();
    }
}


static void draw_box(int x, int y, int w, int h)
{
    fill(x - 2, y - 2, w + 4, h + 4, C_ACCENT);
    fill(x, y, w, h, C_PANEL);
}

static void draw_modal(void)
{
    char nm[96], line[200];
    int i;
    if (g_modal == M_OPTIONS || g_modal == M_DELETE) {
        int bw = W - U(120), rowh = U(40), bh = U(70) + (g_modal == M_OPTIONS ? g_opt_n * rowh : U(70));
        int bx = (W - bw) / 2, by = (H - FOOTER_H - bh) / 2 + U(20);
        static const char *keys[] = { "options.edit", "options.copy", "options.rename", "options.delete", "options.cancel" };
        draw_box(bx, by, bw, bh);
        font(22);
        draw_fit(bx + U(20), by + U(34), bw - U(40), C_TEXT, prof_name(&g_prof[g_target], nm, sizeof nm));
        if (g_modal == M_OPTIONS) {
            for (i = 0; i < g_opt_n; i++) {
                int y = by + U(50) + i * rowh;
                if (i == g_opt_sel) fill(bx + U(10), y, bw - U(20), rowh - U(4), C_BG);
                font(22);
                draw_fit(bx + U(26), y + U(26), bw - U(52), i == g_opt_sel ? C_ACCENT : C_TEXT, tr(keys[g_opt[i]]));
            }
            draw_legend(tr("legend.choose"), "", "");
        } else {
            font(20);
            draw_fit(bx + U(20), by + U(80), bw - U(40), C_ERR, tr("delete.q"));
            draw_legend(tr("legend.yes"), tr("legend.no"), "");
        }
    } else if (g_modal == M_NAME) {
        int top = U(62), cell = U(44), cellh = U(34), gx = (W - g_cols * cell) / 2, rows, vis, r0, r, c;
        fill(0, U(56), W, H - U(56), C_BG);
        font(20);
        draw_fit(U(20), top + U(18), W - U(40), C_DIM, tr("name.title"));
        fill(U(16), top + U(26), W - U(32), U(40), C_PANEL);
        font(24);
        snprintf(line, sizeof line, "%s_", g_name);
        draw_fit(U(26), top + U(54), W - U(52), C_TEXT, line);
        rows = (g_ncell + g_cols - 1) / g_cols;
        vis = (H - FOOTER_H - (top + U(78))) / cellh;
        if (vis < 1) vis = 1;
        r0 = 0;
        if (g_gcur / g_cols >= vis) r0 = g_gcur / g_cols - vis + 1;
        for (r = r0; r < rows && r < r0 + vis; r++)
            for (c = 0; c < g_cols; c++) {
                int k = r * g_cols + c, x = gx + c * cell, y = top + U(78) + (r - r0) * cellh;
                if (k >= g_ncell) break;
                if (k == g_gcur) fill(x, y, cell - U(4), cellh - U(4), C_ACCENT);
                font(22);
                draw_text(x + (cell - U(4) - text_width(g_cell[k])) / 2, y + cellh - U(10), k == g_gcur ? C_BG : C_TEXT, g_cell[k]);
            }
        draw_legend(tr("legend.name_add"), tr("legend.name_erase"), tr("legend.name_done"));
    }
}

static void draw_frame(void)
{
    draw_all();
    draw_modal();
}

/* ---- startup ------------------------------------------------------------- */

static void load_language(const char *appdir)
{
    char path[600], *txt, locale[32] = "en_US";
    snprintf(path, sizeof path, "%s/frogui/settings.txt", g_sdcard);
    txt = read_file(path, NULL);
    if (txt) {
        char *l = strstr(txt, "language=");
        if (l == txt || (l && l[-1] == '\n')) {
            int n = 0;
            l += 9;
            while (*l && *l != '\n' && *l != '\r' && n < 31) locale[n++] = *l++;
            locale[n] = 0;
        }
        free(txt);
    }
    snprintf(path, sizeof path, "%s/lang/en_US.json", appdir);
    pack_load(&g_en, path);
    if (strcmp(locale, "en_US")) {
        snprintf(path, sizeof path, "%s/lang/%s.json", appdir, locale);
        if (pack_load(&g_pack, path) != 0) snprintf(g_status, sizeof g_status, "%s", "");
    }
    {
        const char *e = getenv("ST_LOCALE");        /* tests */
        if (e) { Pack n = { 0 }; g_pack = n; snprintf(path, sizeof path, "%s/lang/%s.json", appdir, e); pack_load(&g_pack, path); }
    }
}

/* The canvas has the panel's own shape (TreeFrogUI's TF_PANEL_W/H, landscape), at most 640
 * wide, so the driver's scale-to-fill does not stretch it: 854x480 gives 640x360, a 4:3
 * panel 640x480. Without the variables it is 640x480, as every other port here. */
static void choose_canvas(void)
{
    char a[32], b[32];
    int pw = 0, ph = 0, cw, ch;
    const char *e;
    if ((e = getenv("ST_PANEL"))) sscanf(e, "%dx%d", &pw, &ph);
    else if (tfenv_get("TF_PANEL_W", a, sizeof a) && tfenv_get("TF_PANEL_H", b, sizeof b)) { pw = atoi(a); ph = atoi(b); }
    if (pw < ph) { int t = pw; pw = ph; ph = t; }
    if (pw >= 160 && ph >= 120 && pw <= 4096 && ph <= 4096) {
        if (pw <= 640) { cw = pw; ch = ph; }
        else { cw = 640; ch = 640 * ph / pw; }
        if (ch > 480) { ch = 480; cw = 480 * pw / ph; }
        g_w = cw & ~1;
        g_h = ch & ~1;
    }
    g_canvas = (uint16_t *)calloc((size_t)g_w * (size_t)g_h, sizeof(uint16_t));
}

static void save_ppm(const char *prefix, int n)
{
    char path[400];
    FILE *f;
    int i;
    snprintf(path, sizeof path, "%s%02d.ppm", prefix, n);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (i = 0; i < W * H; i++) {
        uint16_t p = g_canvas[i];
        fputc((p >> 11) << 3, f); fputc(((p >> 5) & 63) << 2, f); fputc((p & 31) << 3, f);
    }
    fclose(f);
}

static int script_button(const char *tok)
{
    static const char *names[B_COUNT] = { "L", "R", "U", "D", "A", "B", "X", "Y", "L1", "R1", "SEL", "ST", "Q" };
    int i;
    for (i = 0; i < B_COUNT; i++) if (!strcmp(tok, names[i])) return i;
    return -1;
}

int main(int argc, char **argv)
{
    char appdir[512] = ".", *slash;
    const char *e;
    unsigned prev = 0;
    long long next_rep = 0, last_draw = 0;
    int dirty = 1, shot = 0, hold = -1;
    char *script = NULL, *tok = NULL;

    (void)argc;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    if ((e = getenv("ST_SDCARD"))) g_sdcard = e;
    if ((e = getenv("ST_SND_DEV"))) g_snd_dev = e;
    if ((e = getenv("ST_NOHW")) && *e == '1') g_nohw = 1;
    if (argv[0] && (slash = strrchr(argv[0], '/'))) {
        size_t n = (size_t)(slash - argv[0]);
        if (n < sizeof appdir) { memcpy(appdir, argv[0], n); appdir[n] = 0; }
    }
    if ((e = getenv("ST_APPDIR"))) snprintf(appdir, sizeof appdir, "%s", e);
    snprintf(g_appdir, sizeof g_appdir, "%s", appdir);
    if ((e = getenv("ST_PROFILES_DIR"))) snprintf(g_appdir, sizeof g_appdir, "%s", e);

    choose_canvas();
    colours();
    build_profiles();
    load_language(appdir);
    font_setup();
    load_profiles();
    hp_init();
    fix_load();
    state_load();
    g_fixsel = g_fix == FIX_AUTO ? 0 : g_fix == FIX_ON ? 1 : 2;
    if (!g_face) fprintf(stderr, "no font found under %s/frogui/fonts\n", g_sdcard);
    load_edit_from_active();

    if (!g_nohw) {
        hwdisp_init();
        hwdisp_set_target_aspect(g_w, g_h);
        joykey_init();
        if (hwaudio_init() != 0) fprintf(stderr, "audio init failed; test sound disabled\n");
    }
    if ((e = getenv("ST_SCRIPT"))) script = strdup(e);
    if (script) tok = strtok(script, ",");

    while (!g_quit) {
        long long t = now_ms();
        if (g_nohw) {
            int b;
            draw_frame();
            if (getenv("ST_SHOT")) save_ppm(getenv("ST_SHOT"), shot++);
            if (!tok) break;
            b = script_button(tok);
            if (b >= 0) press(b);
            tok = strtok(NULL, ",");
            continue;
        } else {
            unsigned cur = map_buttons(joykey_read()), edge = cur & ~prev;
            int b;
            for (b = 0; b < B_COUNT; b++)
                if (edge & (1u << b)) { press(b); dirty = 1; if (b <= B_DOWN) { hold = b; next_rep = t + 400; } }
            if (hold >= 0) {
                if (!(cur & (1u << hold))) hold = -1;
                else if (t >= next_rep) { press(hold); dirty = 1; next_rep = t + 90; }
            }
            prev = cur;
            test_feed();
            if (dirty || t - last_draw > 500) {
                draw_frame();
                hwdisp_present(g_canvas, g_w, g_h, g_w * 2);
                dirty = 0;
                last_draw = t;
            }
            usleep(15000);
        }
    }
    if (g_testing) g_testing = 0;
    state_save();
    if (!g_nohw) { hwaudio_deinit(); hwdisp_deinit(); }
    fflush(stdout);
    _exit(0);                        /* never run FreeType's atexit teardown (see the SuperTux notes) */
}
