#include "core.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

float core_bezier(float x1, float y1, float x2, float y2, float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    /* Solve x(s) = t for s with Newton's method, then return y(s). */
    float s = t;
    for (int i = 0; i < 8; i++) {
        float inv = 1.0f - s;
        float x = 3 * inv * inv * s * x1 + 3 * inv * s * s * x2 + s * s * s;
        float dx = 3 * inv * inv * x1 + 6 * inv * s * (x2 - x1) + 3 * s * s * (1 - x2);
        if (fabsf(dx) < 1e-6f) break;
        s = clampf(s - (x - t) / dx, 0.0f, 1.0f);
    }
    float inv = 1.0f - s;
    return 3 * inv * inv * s * y1 + 3 * inv * s * s * y2 + s * s * s;
}

/* The same geometry at any pitch: a list with two lines in a row needs taller
 * rows than the menu's single line, and the item in the pill still has to sit
 * on the middle of the screen (Darren, 28 Oct 2026). */
static int top_for(int pitch) { return CORE_SCREEN / 2 - pitch / 2; }

int core_menu_y_at(int i, int pitch) { return top_for(pitch) - i * pitch; }
int core_menu_y(int i) { return core_menu_y_at(i, CORE_MENU_PITCH); }

static float menu_raw(int sel, float dy, int count, int pitch)
{
    float raw = (float)core_menu_y_at(sel, pitch) + dy;
    return clampf(raw, (float)(core_menu_y_at(count - 1, pitch) - CORE_MENU_OVERSCROLL),
                  (float)(core_menu_y_at(0, pitch) + CORE_MENU_OVERSCROLL));
}

static int menu_nearest(float raw, int count, int pitch)
{
    return clampi((int)lroundf((top_for(pitch) - raw) / pitch), 0, count - 1);
}

/* Detent: within each step between two items, the list lags the finger near
 * an item (it has to be pushed out) and catches up fast past halfway (the next
 * one snaps in). x - k sin(2 pi x) / 2 pi: slope 1 - k at each item, 1 + k at
 * halfway, continuous and always moving the same way as the finger. */
static float detent(float x, float k)
{
    return x - k * sinf(6.2831853f * x) / 6.2831853f;
}

float core_menu_drag(int sel, float dy, int count, int *highlight)
{
    return core_list_drag(sel, dy, count, CORE_MENU_DETENT, highlight);
}

float core_list_drag(int sel, float dy, int count, float k, int *highlight)
{
    return core_list_drag_at(sel, dy, count, k, highlight, CORE_MENU_PITCH);
}

float core_list_drag_at(int sel, float dy, int count, float k, int *highlight, int pitch)
{
    float raw = menu_raw(sel, dy, count, pitch);
    float f = (top_for(pitch) - raw) / pitch;  /* finger position, in items */
    float last = (float)(count - 1), d;
    if (f < 0) d = f * CORE_MENU_EDGE;                   /* past either end: stiff */
    else if (f > last) d = last + (f - last) * CORE_MENU_EDGE;
    else d = floorf(f) + detent(f - floorf(f), k);
    if (highlight) {
        int h = (int)lroundf(d);
        *highlight = h < 0 ? 0 : h > count - 1 ? count - 1 : h;
    }
    return top_for(pitch) - d * pitch;
}

int core_menu_release(int sel, float dy, int count)
{
    return core_menu_release_at(sel, dy, count, CORE_MENU_PITCH);
}

int core_menu_release_at(int sel, float dy, int count, int pitch)
{
    return menu_nearest(menu_raw(sel, dy, count, pitch), count, pitch);
}

float core_fisheye_scale(float d)
{
    float k = 1.0f - CORE_FISH_SHRINK * fabsf(d);
    return k < CORE_FISH_MIN ? CORE_FISH_MIN : k;
}

int core_fisheye_grey(float d)
{
    d = fabsf(d);
    int g = d <= 1 ? CORE_FISH_GREY : (int)lroundf(CORE_FISH_GREY - CORE_FISH_GREY_STEP * (d - 1));
    return g < CORE_FISH_GREY_MIN ? CORE_FISH_GREY_MIN : g;
}

/* The sum of the scale from the pill out to d: each stretch of the list takes
 * as much room as the items in it are tall. */
float core_fisheye_offset(float d)
{
    float a = fabsf(d), xm = (1.0f - CORE_FISH_MIN) / CORE_FISH_SHRINK;  /* where shrinking stops */
    float f = a <= xm ? a - CORE_FISH_SHRINK / 2 * a * a
                      : xm - CORE_FISH_SHRINK / 2 * xm * xm + CORE_FISH_MIN * (a - xm);
    f += CORE_FISH_GAP * (a < 1 ? a : 1);
    return d < 0 ? -f : f;
}

int core_skip_hold_step(int total)
{
    total = total < 0 ? -total : total;
    return total < 120 ? 30 : total < 300 ? 60 : 300;
}

void core_skip_label(int seconds, char *dst, size_t n)
{
    const char *sign = seconds < 0 ? "\xE2\x88\x92" : "+";
    int a = seconds < 0 ? -seconds : seconds;
    if (!a) snprintf(dst, n, "0s");  /* at the start already: nothing skipped */
    else if (a < 60) snprintf(dst, n, "%s%ds", sign, a);
    else snprintf(dst, n, "%s%d:%02d", sign, a / 60, a % 60);
}

void core_time_label(int seconds, char *dst, size_t n)
{
    if (seconds < 0) seconds = 0;
    int h = seconds / 3600, m = seconds / 60 % 60, sec = seconds % 60;
    if (h) snprintf(dst, n, "%02d:%02d:%02d", h, m, sec);
    else snprintf(dst, n, "%02d:%02d", m, sec);
}

void core_minutes_label(int seconds, char *dst, size_t n)
{
    if (seconds < 0) seconds = 0;
    snprintf(dst, n, "%02d:%02d", seconds / 60, seconds % 60);
}

bool core_is_drag(float dx, float dy)
{
    return fabsf(dx) > CORE_DRAG_START_PX || fabsf(dy) > CORE_DRAG_START_PX;
}

core_album_action_t core_album_release(float dx, float dy)
{
    if (fabsf(dy) > CORE_SWIPE_V_ALBUMS_PX && fabsf(dy) > fabsf(dx) && dy > 0) return CORE_ALBUM_TO_MENU;
    if (fabsf(dx) > CORE_SWIPE_H_PX) return dx < 0 ? CORE_ALBUM_NEXT : CORE_ALBUM_PREV;
    return CORE_ALBUM_STAY;
}

/* A slide at the scroll speed whose last CORE_MARQUEE_EASE_PCT of the distance
 * slows evenly to a stop, which takes twice as long as at full speed. */
static uint32_t marquee_travel_ms(int over)
{
    return (uint32_t)over * CORE_MARQUEE_MS_PER_PX * (100u + CORE_MARQUEE_EASE_PCT) / 100u;
}

/* Distance covered (0..1) at fraction u (0..1) of the slide's time. */
static float marquee_ease_out(float u)
{
    const float f = CORE_MARQUEE_EASE_PCT / 100.0f;
    float tau = clampf(u, 0, 1) * (1 + f);  /* time in units of the whole distance at full speed */
    if (tau <= 1 - f) return tau;
    float s = tau - (1 - f);
    return (1 - f) + s - s * s / (4 * f);
}

uint32_t core_marquee_cycle_ms(int over)
{
    if (over <= 0) return 0u;
    return CORE_MARQUEE_HOLD_START_MS + 2u * marquee_travel_ms(over) + CORE_MARQUEE_HOLD_END_MS;
}

float core_marquee_offset(int over, uint32_t t_ms)
{
    if (over <= 0) return 0.0f;
    uint32_t travel = marquee_travel_ms(over);
    uint32_t t = t_ms % core_marquee_cycle_ms(over);
    if (t < CORE_MARQUEE_HOLD_START_MS) return 0.0f;
    t -= CORE_MARQUEE_HOLD_START_MS;
    if (t < travel) return -over * marquee_ease_out((float)t / travel);
    t -= travel;
    if (t < CORE_MARQUEE_HOLD_END_MS) return (float)-over;
    t -= CORE_MARQUEE_HOLD_END_MS;
    return -over * (1.0f - marquee_ease_out((float)t / travel));  /* back, easing into the start */
}

char core_letter(const char *s)
{
    if (!s) return '#';
    while (*s == ' ' || *s == '"' || *s == '\'' || *s == '(' || *s == '[') s++;
    unsigned char c = (unsigned char)*s;
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    if (c >= 'A' && c <= 'Z') return (char)c;
    if (c == 0xC3) {  /* UTF-8 Latin-1 letters, U+00C0..U+00FF */
        static const char base[65] = "AAAAAAACEEEEIIIIDNOOOOO#OUUUUYTSAAAAAAACEEEEIIIIDNOOOOO#OUUUUYTY";
        unsigned char n = (unsigned char)s[1];
        if (n >= 0x80 && n <= 0xBF) return base[n - 0x80];
    }
    return '#';
}

float core_fling_albums(float v)
{
    /* 1.5 px/ms (a gentle flick) -> ~4 albums, 4 px/ms -> ~29, 6 px/ms -> ~65 */
    float a = 1.8f * v * v;
    return v > 0 ? -a : a;  /* finger right = back through the list */
}

int core_flick_albums(float v)
{
    int n = (int)lroundf(2.0f + (fabsf(v) - 0.9f) * 4.0f);
    return n < 2 ? 2 : n > 8 ? 8 : n;
}

void core_fold(const char *src, char *dst, size_t n)
{
    static const char base[65] = "aaaaaaaceeeeiiiidnooooo#ouuuuytsaaaaaaaceeeeiiiidnooooo#ouuuuyty";
    size_t o = 0;
    if (!n) return;
    for (const unsigned char *s = (const unsigned char *)(src ? src : ""); *s && o + 1 < n; s++) {
        if (*s == 0xC3 && s[1] >= 0x80 && s[1] <= 0xBF) {
            dst[o++] = base[s[1] - 0x80];
            s++;
        } else {
            dst[o++] = (char)(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s);
        }
    }
    dst[o] = 0;
}

bool core_is_compilation(const char *artist, const char *album)
{
    if (!artist || !*artist) return true;
    if (!strcasecmp(artist, "various artists") || !strcasecmp(artist, "various")) return true;
    return album && !strcasecmp(artist, album);
}

void core_album_key(const char *artist_sort, const char *album_sort, bool compilation, char *dst, size_t n)
{
    char a[96], b[96];
    core_fold(compilation ? album_sort : artist_sort, a, sizeof a);
    core_fold(compilation ? "" : album_sort, b, sizeof b);
    snprintf(dst, n, "%s\x01%s", a, b);
}

/* ---- Voice search ------------------------------------------------------------ */
static bool is_vowel(char c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y'; }
static bool is_word_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

void core_sound(const char *w, char *dst, size_t n)
{
    char t[64];
    size_t o = 0, len = strlen(w);
    if (!n) return;
    /* spell it as it sounds */
    for (size_t i = 0; i < len && o + 2 < sizeof t; i++) {
        char c = w[i], d = i + 1 < len ? w[i + 1] : 0, e = i + 2 < len ? w[i + 2] : 0;
        if (i == 0 && ((c == 'k' && d == 'n') || (c == 'g' && d == 'n') || (c == 'w' && d == 'r'))) continue;
        if (c == 'r' && i > 0 && is_vowel(w[i - 1]) && !is_vowel(d)) continue;  /* British: "ar" sounds "ah" */
        if (c == 'p' && d == 'h') { t[o++] = 'f'; i++; }
        else if (c == 'g' && d == 'h') i++;                        /* night, though */
        else if (c == 'g' && d == 'n') { t[o++] = 'n'; i++; }       /* reign, sign */
        else if (c == 'c' && d == 'k') { t[o++] = 'k'; i++; }
        else if (c == 's' && d == 'c' && e == 'h') { t[o++] = 's'; t[o++] = 'k'; i += 2; }
        else if ((c == 's' || c == 'c') && d == 'h') { t[o++] = 'x'; i++; }
        else if (c == 't' && d == 'h') { t[o++] = '0'; i++; }
        else if (c == 'm' && d == 'b' && !e) { t[o++] = 'm'; i++; }
        else if (c == 'd' && d == 'g') { t[o++] = 'j'; i++; }
        else if (c == 'c') t[o++] = (d == 'e' || d == 'i' || d == 'y') ? 's' : 'k';
        else if (c == 'q') t[o++] = 'k';
        else if (c == 'x') { t[o++] = 'k'; t[o++] = 's'; }
        else if (c == 'z') t[o++] = 's';
        else if (c == 'v') t[o++] = 'f';
        else t[o++] = c;
    }
    t[o] = 0;
    /* keep the first sound, then only consonants, no doubles */
    size_t k = 0;
    for (size_t i = 0; i < o && k + 1 < n; i++) {
        char c = t[i];
        if (i == 0) c = is_vowel(c) ? 'a' : c;
        else if (is_vowel(c) || c == 'h' || c == 'w') continue;
        if (k && dst[k - 1] == c) continue;
        dst[k++] = c;
    }
    dst[k] = 0;
}

/* The number a word says, or -1: digits ("5"), the number itself ("five") or a
 * Roman numeral ("IV"). Titles and speech-to-text disagree about which of the
 * three to use - "To The 5 Boroughs", "Chapter II", "The 3 Tenors" - and a
 * number written the other way counted as a word the album didn't have
 * (Darren, 29 Oct 2026). Single letters are left alone: "I Hate Models" is not
 * "1 Hate Models", and "X" is a letter far more often than it is ten. */
static int core_number(const char *w)
{
    static const char *const NAMES[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight",
                                        "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen",
                                        "sixteen", "seventeen", "eighteen", "nineteen", "twenty"};
    static const char *const TENS[] = {"thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};
    size_t l = strlen(w);
    if (!l) return -1;
    if (w[0] >= '0' && w[0] <= '9') {
        int v = 0;
        for (size_t i = 0; i < l; i++) {
            if (w[i] < '0' || w[i] > '9') return -1;
            v = v * 10 + (w[i] - '0');
            if (v > 999) return -1;  /* a year is said as two numbers, not one */
        }
        return v;
    }
    for (int i = 0; i <= 20; i++)
        if (!strcmp(w, NAMES[i])) return i;
    for (int i = 0; i < 7; i++)
        if (!strcmp(w, TENS[i])) return 30 + i * 10;
    if (l < 2) return -1;  /* "i", "v" and "x" are letters */
    int v = 0, last = 0;
    for (size_t i = l; i-- > 0;) {
        int d = w[i] == 'i' ? 1 : w[i] == 'v' ? 5 : w[i] == 'x' ? 10 : w[i] == 'l' ? 50 : w[i] == 'c' ? 100 : 0;
        if (!d) return -1;
        v += d < last ? -d : d;
        if (d > last) last = d;
    }
    return v;
}

/* Two spellings of the same thing. 215 of Darren's albums end "- CD 1" and 15
 * end "- Disc 1", and whichever he says he means the same thing (30 Oct 2026). */
static int say_group(const char *w)
{
    static const char *const CD[] = {"cd", "disc", "disk"};
    for (int i = 0; i < 3; i++)
        if (!strcmp(w, CD[i])) return 1;
    return 0;
}

int core_word_match(const char *word, const char *text, char *matched, size_t n)
{
    char ws[64], tok[64], ts[64];
    size_t wl = strlen(word);
    bool digits = wl && word[0] >= '0' && word[0] <= '9';
    core_sound(word, ws, sizeof ws);
    int best = 0;
    for (const char *p = text; *p;) {
        while (*p && !is_word_char(*p)) p++;
        size_t l = 0;
        while (p[l] && is_word_char(p[l])) l++;
        if (!l) break;
        if (l < sizeof tok) {
            memcpy(tok, p, l);
            tok[l] = 0;
            int m = 0, num = core_number(word);
            if (l == wl && !memcmp(tok, word, l)) m = 3;
            else if (num >= 0 && core_number(tok) == num) m = 3;  /* "5" is "five" is "V" */
            else if (say_group(word) && say_group(word) == say_group(tok)) m = 3;  /* "disc 2" is "CD 2" */
            else if (!digits && wl >= 3 && l + 3 >= wl && wl + 3 >= l) {
                core_sound(tok, ts, sizeof ts);
                /* two sounds at least: "verve" wears down to a single "f"
                 * (the r goes the way it does in English speech, the v's
                 * double up), and on one sound alone it was the same word as
                 * "Five" (Darren, 30 Oct 2026). */
                if (ws[0] && ws[1] && !strcmp(ws, ts)) m = 2;
            }
            if (!m && wl >= 3 && l > wl && !memcmp(tok, word, wl)) m = 1;
            /* ...and the short way round: a title's "Vol 6" against a spoken
             * "volume six" (29 Oct 2026). Only worth a point, as above. */
            if (!m && l >= 3 && wl > l && !memcmp(tok, word, l)) m = 1;
            if (m > best) {
                best = m;
                if (matched) snprintf(matched, n, "%s", tok);
            }
        }
        p += l;
    }
    return best;
}

static const char *const LETTER_NAMES[26] = {"ay", "bee", "see", "dee", "ee", "ef", "jee", "aitch", "eye",
                                             "jay", "kay", "el", "em", "en", "oh", "pee", "cue", "ar",
                                             "es", "tee", "you", "vee", "doubleyou", "ex", "why", "zed"};
static const char *const DIGIT_NAMES[10] = {"zero", "one", "two", "three", "four",
                                            "five", "six", "seven", "eight", "nine"};

void core_phrase_key(const char *text, char *dst, size_t n)
{
    size_t o = 0;
    if (!n) return;
    dst[0] = 0;
    for (const char *p = text; *p;) {
        while (*p && !is_word_char(*p)) p++;
        /* a run of letters or a run of digits: "u2" is "u" then "2" */
        bool digits = *p >= '0' && *p <= '9';
        size_t l = 0;
        while (p[l] && is_word_char(p[l]) && ((p[l] >= '0' && p[l] <= '9') == digits)) l++;
        if (!l) break;
        char word[64], snd[64];
        for (size_t d = 0; d < (digits ? l : 1); d++) {
            if (digits) snprintf(word, sizeof word, "%s", DIGIT_NAMES[p[d] - '0']);
            else if (l == 1) snprintf(word, sizeof word, "%s", LETTER_NAMES[p[0] - 'a']);
            else snprintf(word, sizeof word, "%.*s", (int)(l < sizeof word ? l : sizeof word - 1), p);
            core_sound(word, snd, sizeof snd);
            for (const char *s = snd; *s && o + 1 < n; s++)
                if (!o || dst[o - 1] != *s) dst[o++] = *s;  /* no doubles across words either */
            dst[o] = 0;
        }
        p += l;
    }
}

float core_similarity(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (!la || !lb) return 0;
    if (la > 40 || lb > 40) return strcmp(a, b) ? 0 : 1;
    if (la <= 3 || lb <= 3) return strcmp(a, b) ? 0.0f : 1.0f;
    /* edit distance, with two neighbouring sounds swapped counting as one edit;
     * three rows, not the whole table: this runs on small task stacks */
    int r0[41], r1[41], r2[41];
    int *pp = r0, *prev = r1, *cur = r2;
    for (size_t j = 0; j <= lb; j++) prev[j] = (int)j;
    for (size_t i = 1; i <= la; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= lb; j++) {
            int sub = prev[j - 1] + (a[i - 1] != b[j - 1]);
            int del = prev[j] + 1, ins = cur[j - 1] + 1;
            int best = sub < del ? (sub < ins ? sub : ins) : (del < ins ? del : ins);
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] && pp[j - 2] + 1 < best)
                best = pp[j - 2] + 1;
            cur[j] = best;
        }
        int *t = pp;
        pp = prev;
        prev = cur;
        cur = t;
    }
    size_t longer = la > lb ? la : lb;
    return 1.0f - (float)prev[lb] / (float)longer;
}


/* ---- Reads as -------------------------------------------------------------- */
typedef struct {
    const char *sym;  /* UTF-8 */
    char letter;
    bool inside;      /* only between letters */
} read_alike_t;

static const read_alike_t READ_ALIKE[] = {
    {"$", 's', false}, {"@", 'a', false}, {"\xC2\xA5", 'y', false} /* ¥ */, {"\xE2\x82\xAC", 'e', false} /* € */,
    {"\xC2\xA2", 'c', false} /* ¢ */, {"!", 'i', true},
    {"0", 'o', true}, {"1", 'i', true}, {"3", 'e', true}, {"4", 'a', true}, {"5", 's', true}, {"7", 't', true},
};

/* One unit of text at p: its length, and the letter it reads as (0 if none);
 * *letter_like says whether it's a letter or a symbol that can stand for one. */
static size_t read_unit(const char *p, char *as, bool *inside, bool *letter_like)
{
    *as = 0;
    *inside = false;
    for (size_t i = 0; i < sizeof READ_ALIKE / sizeof READ_ALIKE[0]; i++) {
        size_t l = strlen(READ_ALIKE[i].sym);
        if (!strncmp(p, READ_ALIKE[i].sym, l)) {
            *as = READ_ALIKE[i].letter;
            *inside = READ_ALIKE[i].inside;
            *letter_like = !READ_ALIKE[i].inside;
            return l;
        }
    }
    unsigned char c = (unsigned char)*p;
    *letter_like = (c >= 'a' && c <= 'z');
    if (c < 0x80) return 1;
    size_t l = 1;
    while ((p[l] & 0xC0) == 0x80) l++;  /* one UTF-8 character */
    return l;
}

/* What a symbol is said as, when no letter is leaning on it. */
static const char *spoken_symbol(char c)
{
    switch (c) {
        case '@': return "at";
        case '&': return "and";
        case '+': return "and";
        case '%': return "percent";
        default: return NULL;
    }
}

void core_read_alike(const char *src, char *dst, size_t n)
{
    enum { MAX_UNITS = 160 };
    size_t start[MAX_UNITS], len[MAX_UNITS];
    char as[MAX_UNITS];
    bool inside[MAX_UNITS], like[MAX_UNITS];
    int units = 0;
    for (const char *p = src; *p && units < MAX_UNITS; units++) {
        start[units] = (size_t)(p - src);
        len[units] = read_unit(p, &as[units], &inside[units], &like[units]);
        p += len[units];
    }
    size_t o = 0;
    if (!n) return;
    for (int i = 0; i < units; i++) {
        bool before = i > 0 && like[i - 1], after = i + 1 < units && like[i + 1];
        bool letter = as[i] && (inside[i] ? before && after : before || after);
        if (letter) {
            if (o + 1 < n) dst[o++] = as[i];
        } else if (src[start[i]] == '.' && i > 0 && like[i - 1] && len[i - 1] == 1 && (i < 2 || !like[i - 2]) &&
                   i + 1 < units && like[i + 1] && (i + 2 >= units || !like[i + 2])) {
            /* Initials run together, the way they are said and the way
             * speech-to-text writes them: "L.A. Woman" is "la woman",
             * "R.E.M." is "rem", "Run D.M.C." is "run dmc" (29 Oct 2026).
             * Single letters either side only, so "R.Kelly" stays two words. */
        } else if (len[i] == 1 && spoken_symbol(src[start[i]])) {
            /* Symbols that are said out loud: "PAN-POT Live @ Fabric London" is
             * "live at fabric london", "Black & Blue" is "black and blue",
             * "Bits + Pieces" is "bits and pieces", "100% Colombian" is "one
             * hundred percent colombian" (Darren, 29 Oct 2026). Spaced, so
             * "R&B" reads as three words and matches "r and b". */
            const char *w = spoken_symbol(src[start[i]]);
            if (o + 1 < n && o && dst[o - 1] != ' ') dst[o++] = ' ';
            for (const char *c = w; *c && o + 1 < n; c++) dst[o++] = *c;
            if (i + 1 < units && like[i + 1] && o + 1 < n) dst[o++] = ' ';
        } else {
            for (size_t k = 0; k < len[i] && o + 1 < n; k++) dst[o++] = src[start[i] + k];
        }
    }
    dst[o] = 0;
}

static bool is_article(const char *w)
{
    return !strcmp(w, "the") || !strcmp(w, "a") || !strcmp(w, "an");
}

/* Fold and split into words, dropping "play", "search for", "by" and the like
 * - and the articles too, unless `keep_articles`. */
static int split_words(const char *text, char words[][32], int max, bool keep_articles)
{
    /* "the" is in 530 of Darren's 1,839 titles, so it is nearly free: a
     * misheard "the verve" came back as "the nerve" and scored as a complete
     * two-word match on The Prodigy, because one of its two words was "the"
     * (Darren, 30 Oct 2026). Dropped, along with "a" and "an", so only the
     * words that say something count. "Featuring" goes too: the titles write
     * it "ft.", so saying it in full cost a word and dropped the set to an
     * also-ran. No title in the library is made only of these. */
    static const char *const FILLER[] = {"play", "search", "find", "for", "me", "some", "by", "please", "album",
                                         "song", "track", "the", "a", "an", "feat", "ft", "featuring", "vs",
                                         "versus"};
    char folded[128];
    core_fold(text, folded, sizeof folded);
    int n = 0;
    for (char *p = folded; *p && n < max;) {
        while (*p && !(isalnum((unsigned char)*p) || (unsigned char)*p >= 0x80)) p++;
        char *start = p;
        while (*p && (isalnum((unsigned char)*p) || (unsigned char)*p >= 0x80)) p++;
        int len = (int)(p - start);
        if (!len || len >= 32) continue;
        memcpy(words[n], start, len);
        words[n][len] = 0;
        bool filler = false;
        for (size_t f = 0; f < sizeof FILLER / sizeof FILLER[0]; f++) filler |= !strcmp(words[n], FILLER[f]);
        if (keep_articles && is_article(words[n])) filler = false;
        /* an article belongs to the word after it: "a song by u2" keeps none */
        else if (filler && keep_articles && n && is_article(words[n - 1])) n--;
        if (!filler) n++;
    }
    while (keep_articles && n && is_article(words[n - 1])) n--;
    return n;
}

int core_search_words(const char *text, char words[][32], int max)
{
    return split_words(text, words, max, false);
}

/* The articles go back in for Music Assistant, which matches the phrase as a
 * whole: "in name of love" found nothing, "in the name of love" found U2
 * twice (Darren, 20 Nov 2026). Dropping them is right for ranking albums on
 * the knob and wrong for this. The other words come from `fixed`, so the
 * knob's spelling corrections still reach the request. */
int core_search_phrase(const char *text, char fixed[][32], int nfixed, char out[][32], int max)
{
    int n = split_words(text, out, max, true), k = 0;
    for (int i = 0; i < n; i++)
        if (!is_article(out[i]) && k < nfixed) snprintf(out[i], 32, "%s", fixed[k++]);
    return n;
}

static void append(char *dst, size_t n, const char *s)
{
    size_t l = strlen(dst);
    if (l < n) snprintf(dst + l, n - l, "%s", s);
}

/* Score every album against the words, out of 1000.
 * Word by word: each word scores 2 per point of core_word_match (same word 3,
 * sounds alike 2, start of a word 1) plus 1 for the same word in the artist's
 * name; all words present gives 500-1000.
 * The whole phrase by sound: how alike it is to the artist, the title, or
 * both (core_similarity, 0.75 or more counts): catches what speech-to-text
 * splits or respells, "Ah Kelly" for R. Kelly, "C6 Steve" for Seasick Steve.
 * The better of the two counts, but the phrase only rescues albums when no
 * album has all the words: "prince purple rain" is Purple Rain, not Prince's
 * Platinum or Parade, which sound close only because "Prince" is most of the
 * phrase (Darren, 27 Oct 2026). Albums missing one word of three or more are
 * kept as "all but one". No training: the rules and the library do it. */
int core_rank_albums(char words[][32], int nwords, int total, const char *(*key_of)(int album),
                     core_candidate_t *cand, char fixed[][32])
{
    char phrase[160] = "", qkey[64];
    if (nwords > CORE_SEARCH_WORDS) nwords = CORE_SEARCH_WORDS;
    for (int w = 0; w < nwords; w++) {
        append(phrase, sizeof phrase, words[w]);
        append(phrase, sizeof phrase, " ");
    }
    core_phrase_key(phrase, qkey, sizeof qkey);
    int n = 0;
    for (int i = 0; i < total; i++) {
        const char *key = key_of(i);
        const char *sep = strchr(key, '\x01');
        char artist[96], title[96], both[192], k1[64], k2[64], k3[64];
        size_t al = sep ? (size_t)(sep - key) : strlen(key);
        char raw[192];
        snprintf(raw, sizeof raw, "%.*s", (int)(al < sizeof raw ? al : sizeof raw - 1), key);
        core_read_alike(raw, artist, sizeof artist);  /* "ke$ha" reads as "kesha" */
        core_read_alike(sep ? sep + 1 : "", title, sizeof title);
        int mw[CORE_SEARCH_WORDS], bonus[CORE_SEARCH_WORDS];
        int gaps = 0;
        for (int w = 0; w < nwords; w++) {
            int ma = core_word_match(words[w], artist, NULL, 0), mt = core_word_match(words[w], title, NULL, 0);
            mw[w] = ma > mt ? ma : mt;
            bonus[w] = ma == 3;
            gaps += !mw[w];
        }
        /* One word said as two: speech-to-text wrote "back street boys", and the
         * Backstreet Boys were two words short of being found at all, where the
         * other way round ("hiphop" for "Hip Hop") had always half-worked
         * (Darren, 30 Oct 2026). Only where a word was missed, and only for the
         * same word or one that sounds like it: on a prefix any two short words
         * would join onto anything. Only albums already within two words of
         * the phrase are tried, which is nearly none of the library: joining
         * every pair against all 1,839 of them took the search from 0.9 to
         * 1.4 seconds on the knob, and this brings it back. */
        for (int w = 0; gaps && gaps <= 2 && w + 1 < nwords; w++) {
            if (mw[w] && mw[w + 1]) continue;
            char joined[64];
            snprintf(joined, sizeof joined, "%s%s", words[w], words[w + 1]);
            int ja = core_word_match(joined, artist, NULL, 0), jt = core_word_match(joined, title, NULL, 0);
            int j = ja > jt ? ja : jt;
            if (j < 2) continue;
            for (int k = w; k <= w + 1; k++)
                if (j > mw[k]) { mw[k] = j; bonus[k] = ja == 3; }
        }
        int score = 0, missing = 0;
        for (int w = 0; w < nwords; w++) {
            if (!mw[w]) missing++;
            score += mw[w] * 2 + bonus[w];
        }
        int value = missing ? 0 : 500 + 500 * score / (nwords * 7);
        snprintf(both, sizeof both, "%s %s", artist, title);
        core_phrase_key(artist, k1, sizeof k1);
        core_phrase_key(title, k2, sizeof k2);
        core_phrase_key(both, k3, sizeof k3);
        float sim = fmaxf(core_similarity(qkey, k1), fmaxf(core_similarity(qkey, k2), core_similarity(qkey, k3)));
        /* at most 950: the same words (up to 1000) always beat a sound-alike
         * phrase; plain "kylie" is Kylie, not an artist called Carl */
        /* the phrase rescues albums the words alone would drop ("ah kelly" has
         * one word of two), but an album missing one word of three or more stays
         * "all but one": "new model army 225" wants the track 225 first */
        bool partial = missing == 1 && nwords >= 3, by_sound = false;
        if (sim >= 0.75f && !partial && (int)(sim * 950) > value) {
            value = (int)(sim * 950);
            by_sound = missing > 0;
            missing = 0;
        }
        if (missing > 1 || (missing == 1 && nwords < 3)) continue;
        core_candidate_t c = {i, missing ? score : value, missing, by_sound};
        /* keep the best: fewer missing words first, then score, then browse order */
        int at = n;
        while (at > 0 && (cand[at - 1].missing > c.missing ||
                          (cand[at - 1].missing == c.missing && cand[at - 1].score < c.score)))
            at--;
        if (at >= CORE_SEARCH_CANDIDATES) continue;
        if (n < CORE_SEARCH_CANDIDATES) n++;
        memmove(&cand[at + 1], &cand[at], (n - 1 - at) * sizeof(core_candidate_t));
        cand[at] = c;
    }
    /* an album with every word: drop the ones only the phrase's sound found */
    bool complete = false;
    for (int i = 0; i < n; i++) complete |= !cand[i].missing && !cand[i].by_sound;
    if (complete) {
        int k = 0;
        for (int i = 0; i < n; i++)
            if (!cand[i].by_sound) cand[k++] = cand[i];
        n = k;
    }
    /* the words as the best album spells them: "reign" -> "rain" */
    for (int w = 0; w < nwords; w++) {
        snprintf(fixed[w], 32, "%s", words[w]);
        if (n && !cand[0].missing) {
            char tok[32];
            if (core_word_match(words[w], key_of(cand[0].album), tok, sizeof tok) == 2) snprintf(fixed[w], 32, "%s", tok);
        }
    }
    return n;
}
