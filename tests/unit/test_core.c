/* Unit tests for core/. Run with: cmake -S tests -B tests/build && cmake --build tests/build && ctest --test-dir tests/build */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core.h"

static int failures;
static int checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                        \
    } while (0)
#define NEAR(a, b, tol) CHECK(fabsf((float)(a) - (float)(b)) <= (tol))

static void test_bezier(void)
{
    NEAR(core_bezier(0.42f, 0, 0.58f, 1, 0.0f), 0.0f, 1e-6f);
    NEAR(core_bezier(0.42f, 0, 0.58f, 1, 1.0f), 1.0f, 1e-6f);
    NEAR(core_bezier(0.42f, 0, 0.58f, 1, 0.5f), 0.5f, 1e-3f);  /* ease-in-out is symmetric */
    NEAR(core_bezier(0, 0, 1, 1, 0.3f), 0.3f, 1e-3f);           /* linear */
    /* the album slide curve (.2,.9,.3,1) is front-loaded: well past halfway at t=0.25 */
    CHECK(core_bezier(0.2f, 0.9f, 0.3f, 1.0f, 0.25f) > 0.6f);
}

static float snap_ease(float t) { return core_bezier(CORE_MENU_SNAP_EASE, t); }

static void test_menu_snap(void)
{
    /* straight in: never past the pill, never backwards */
    float prev = 0;
    for (int i = 0; i <= 100; i++) {
        float p = snap_ease(i / 100.0f);
        CHECK(p <= 1.0f + 1e-5f && p >= prev - 1e-5f);
        prev = p;
    }
}

static void test_menu_positions(void)
{
    /* Albums (index 2) centres on the pill at y=180: list top 30, item centre 30+2*60+30 */
    CHECK(core_menu_y(2) == 30);
    CHECK(core_menu_y(2) + 2 * CORE_MENU_PITCH + CORE_MENU_PITCH / 2 == 180);
    CHECK(core_menu_y(0) == 150);
}

static void test_menu_drag(void)
{
    int hl = -1;
    /* No movement: sits exactly on the slot, highlighting the selected item */
    NEAR(core_menu_drag(2, 0, 6, &hl), core_menu_y(2), 1e-4f);
    CHECK(hl == 2);
    /* Near an item it resists: 10px of finger moves the list much less */
    float y = core_menu_drag(2, 10, 6, &hl) - core_menu_y(2);
    CHECK(y > 0.0f && y < 4.0f);
    CHECK(hl == 2);
    /* Halfway, finger and list agree, and past it the next item snaps in fast */
    NEAR(core_menu_drag(2, 30, 6, &hl), core_menu_y(2) + 30, 1e-3f);
    float a = core_menu_drag(2, 26, 6, &hl), b = core_menu_drag(2, 34, 6, &hl);
    CHECK(b - a > 12.0f);  /* 8px of finger moves the list well over 8px */
    /* A full pitch lands exactly on the next item and highlights it */
    NEAR(core_menu_drag(2, 60, 6, &hl), core_menu_y(1), 1e-3f);
    CHECK(hl == 1);
    /* Always moves the same way as the finger, never jumps */
    float prev = core_menu_drag(2, -200, 6, &hl);
    for (float dy = -199; dy <= 200; dy += 1) {
        float cur = core_menu_drag(2, dy, 6, &hl);
        CHECK(cur >= prev - 1e-3f);
        CHECK(cur - prev < 3.0f);
        prev = cur;
    }
    /* Past the ends it gives only a little (stiff), within the 26px rubber band */
    float e = core_menu_drag(0, 500, 6, &hl) - core_menu_y(0);
    CHECK(e > 0.0f && e < 26.0f * 0.4f);
    CHECK(hl == 0);
    e = core_menu_y(5) - core_menu_drag(5, -500, 6, &hl);
    CHECK(e > 0.0f && e < 26.0f * 0.4f);
    CHECK(hl == 5);
}

static void test_fisheye(void)
{
    NEAR(core_fisheye_scale(0), 1.0f, 1e-6f);
    NEAR(core_fisheye_scale(1), 0.85f, 1e-6f);
    NEAR(core_fisheye_scale(-2), 0.70f, 1e-6f);
    NEAR(core_fisheye_scale(4), CORE_FISH_MIN, 1e-6f);
    CHECK(core_fisheye_grey(0.5f) == 0x80 && core_fisheye_grey(1) == 0x80);
    CHECK(core_fisheye_grey(2) == 0x80 - 24);
    CHECK(core_fisheye_grey(9) == 0x60);
    /* spacing: on the pill, nothing moves; next to it a little more room;
     * further out each gap smaller than the one before; the same both ways */
    NEAR(core_fisheye_offset(0), 0, 1e-6f);
    NEAR(core_fisheye_offset(1) * CORE_MENU_PITCH, 64.5f, 0.01f);
    NEAR(core_fisheye_offset(-1), -core_fisheye_offset(1), 1e-6f);
    float prev = core_fisheye_offset(1);
    for (int i = 2; i <= 5; i++) {
        float gap = core_fisheye_offset((float)i) - core_fisheye_offset((float)(i - 1));
        CHECK(gap < prev + 1e-6f && gap >= CORE_FISH_MIN - 1e-6f);
        prev = gap;
    }
    for (float x = -4; x < 4; x += 0.05f) CHECK(core_fisheye_offset(x + 0.05f) > core_fisheye_offset(x));  /* order kept */
}

static void test_list_drag_follow(void)
{
    /* detent 0: the list follows the finger exactly */
    int hl;
    for (int dy = -50; dy <= 50; dy += 5) NEAR(core_list_drag(3, (float)dy, 10, 0, &hl), core_menu_y(3) + dy, 1e-3f);
}

static void test_skip_hold(void)
{
    /* the steps from a standing start: 30 s up to 2:00, a minute up to 5:00, then 5 minutes */
    static const int want[] = {30, 60, 90, 120, 180, 240, 300, 600, 900, 1200};
    int total = 0;
    for (int i = 0; i < 10; i++) {
        total += core_skip_hold_step(total);
        CHECK(total == want[i]);
    }
    CHECK(core_skip_hold_step(-150) == 60);  /* backwards the same */
    char b[16];
    core_skip_label(5, b, sizeof b);
    CHECK(!strcmp(b, "+5s"));
    core_skip_label(-25, b, sizeof b);
    CHECK(!strcmp(b, "\xE2\x88\x92" "25s"));
    core_skip_label(90, b, sizeof b);
    CHECK(!strcmp(b, "+1:30"));
    core_skip_label(900, b, sizeof b);
    CHECK(!strcmp(b, "+15:00"));
    core_skip_label(0, b, sizeof b);
    CHECK(!strcmp(b, "0s"));

    /* the play time in the same slot */
    core_time_label(0, b, sizeof b);
    CHECK(!strcmp(b, "00:00"));
    core_time_label(7, b, sizeof b);
    CHECK(!strcmp(b, "00:07"));
    core_time_label(222, b, sizeof b);
    CHECK(!strcmp(b, "03:42"));
    core_time_label(3600, b, sizeof b);
    CHECK(!strcmp(b, "01:00:00"));
    core_time_label(3723, b, sizeof b);
    CHECK(!strcmp(b, "01:02:03"));
    core_time_label(-5, b, sizeof b);
    CHECK(!strcmp(b, "00:00"));

    /* the scrubber's time, which has no room for an hours field */
    core_minutes_label(0, b, sizeof b);
    CHECK(!strcmp(b, "00:00"));
    core_minutes_label(222, b, sizeof b);
    CHECK(!strcmp(b, "03:42"));
    core_minutes_label(3852, b, sizeof b);  /* an hour-long mix: 64:12, not 01:04:12 */
    CHECK(!strcmp(b, "64:12"));
    core_minutes_label(-5, b, sizeof b);
    CHECK(!strcmp(b, "00:00"));
}

static void test_menu_release(void)
{
    CHECK(core_menu_release(2, 0, 6) == 2);
    CHECK(core_menu_release(2, 29, 6) == 2);
    CHECK(core_menu_release(2, 31, 6) == 1);
    CHECK(core_menu_release(2, -95, 6) == 4);
    CHECK(core_menu_release(0, 1000, 6) == 0);
    CHECK(core_menu_release(0, -1000, 6) == 5);
}

static void test_album_gestures(void)
{
    CHECK(!core_is_drag(5, 5));
    CHECK(core_is_drag(6, 0));
    CHECK(core_album_release(-65, 0) == CORE_ALBUM_NEXT);
    CHECK(core_album_release(65, 0) == CORE_ALBUM_PREV);
    CHECK(core_album_release(-64, 0) == CORE_ALBUM_STAY);
    CHECK(core_album_release(10, 61) == CORE_ALBUM_TO_MENU);
    CHECK(core_album_release(10, 60) == CORE_ALBUM_STAY);   /* threshold is exclusive */
    CHECK(core_album_release(10, -100) == CORE_ALBUM_STAY); /* swipe up does nothing here */
    CHECK(core_album_release(90, 70) == CORE_ALBUM_PREV);   /* mostly horizontal wins */
}

static void test_marquee(void)
{
    const uint32_t H = CORE_MARQUEE_HOLD_START_MS;
    CHECK(H == 1500);
    CHECK(core_marquee_cycle_ms(0) == 0);
    /* pause + 2 x (100 px at 20 ms/px, the last 15 px taking twice as long) + 1.5 s hold */
    CHECK(core_marquee_cycle_ms(100) == H + 2 * 2300 + 1500);
    /* steady speed from the start (no ease in): 50 px/s, so 10 px after 0.2 s */
    NEAR(core_marquee_offset(100, H + 200), -10, 0.01f);
    NEAR(core_marquee_offset(100, H + 1500), -75, 0.01f);
    /* then it slows to a stop: the last 15 px take 600 ms */
    float full = core_marquee_offset(100, H + 500) - core_marquee_offset(100, H + 510);   /* 10 ms at speed */
    float last = core_marquee_offset(100, H + 2280) - core_marquee_offset(100, H + 2290); /* the last 10 ms */
    NEAR(full, 0.5f, 0.01f);
    CHECK(last >= 0.0f && last < full / 10);
    CHECK(core_marquee_offset(100, H + 2290) < -99.9f);
    NEAR(core_marquee_offset(0, 1234), 0, 1e-6f);
    NEAR(core_marquee_offset(80, 0), 0, 1e-4f);
    NEAR(core_marquee_offset(80, H - 1), 0, 1e-4f);            /* still in the pause */
    CHECK(core_marquee_offset(80, H + 200) < 0.0f);             /* moving within 0.2 s of the pause */
    NEAR(core_marquee_offset(80, H + 1840 + 10), -80, 1e-3f);   /* end hold, exactly the overflow */
    uint32_t c = core_marquee_cycle_ms(80);
    NEAR(core_marquee_offset(80, c - 1), 0, 0.5f);             /* back at the start */
    NEAR(core_marquee_offset(80, c + H - 1), 0, 1e-4f);        /* and the whole pause again before the next */
    for (uint32_t t = 0; t < c; t += 7) {                     /* never past the overflow */
        float o = core_marquee_offset(80, t);
        CHECK(o <= 0.0f && o >= -80.0f);
    }
    /* the long Club Azuli title (610 px over) starts moving after 1.5 s, not the prototype's 6.5 s */
    NEAR(core_marquee_offset(610, H - 1), 0, 1e-4f);
    CHECK(core_marquee_offset(610, H + 500) < -0.5f);
}

static void test_letters(void)
{
    CHECK(core_letter("altogether, the") == 'A');
    CHECK(core_letter("Zebra") == 'Z');
    CHECK(core_letter("100% colombian") == '#');
    CHECK(core_letter("1-800-new-funk") == '#');
    CHECK(core_letter("\"heroes\"") == 'H');
    CHECK(core_letter("\xC3\xA9" "coute") == 'E');   /* é */
    CHECK(core_letter("\xC3\x96" "sterreich") == 'O'); /* Ö */
    CHECK(core_letter("") == '#');
    CHECK(core_letter(NULL) == '#');
}

static void test_flick_albums(void)
{
    CHECK(core_flick_albums(0.5f) == 2);   /* any flick moves at least two */
    CHECK(core_flick_albums(1.08f) == 3);  /* Darren's "a bit harder" flick: 81 px in 75 ms */
    CHECK(core_flick_albums(2.0f) == 6);
    CHECK(core_flick_albums(2.87f) == 8);  /* his hardest single flick */
    CHECK(core_flick_albums(-2.87f) == 8); /* direction doesn't matter */
    CHECK(core_flick_albums(9.0f) == 8);   /* capped */
    for (float v = 0.5f; v < 4.0f; v += 0.1f) CHECK(core_flick_albums(v + 0.1f) >= core_flick_albums(v));
}

static void key(char *dst, const char *artist, const char *album)
{
    core_album_key(artist, album, core_is_compilation(artist, album), dst, 200);
}

static void test_sort_keys(void)
{
    char f[64], a[200], b[200];
    core_fold("G\xC3\xA1" "bor Szab\xC3\xB3", f, sizeof f);  /* Gábor Szabó */
    CHECK(!strcmp(f, "gabor szabo"));
    core_fold("ABC", f, 3);
    CHECK(!strcmp(f, "ab"));                       /* never overruns */
    CHECK(core_is_compilation("Various Artists", "Hed Kandi: Disco Heaven"));
    CHECK(core_is_compilation("101 80's Hits", "101 80's Hits"));
    CHECK(!core_is_compilation("Prince", "Purple Rain"));
    /* artist first, then album A-Z */
    key(a, "prince", "1999"); key(b, "prince", "purple rain");
    CHECK(strcmp(a, b) < 0);
    key(a, "prince", "the vault"); key(b, "prodigy, the", "fat of the land, the");
    CHECK(strcmp(a, b) < 0);                       /* all of Prince before The Prodigy */
    /* a compilation files under its title among the artists */
    key(a, "Various Artists", "hed kandi: disco heaven"); key(b, "led zeppelin", "iv");
    CHECK(strcmp(a, b) < 0);                       /* H before L */
    key(b, "happy mondays", "pills 'n' thrills");
    CHECK(strcmp(a, b) > 0);                       /* "hed..." after "happy..." */
    CHECK(core_letter(a) == 'H');
    /* the album title doesn't leak into an artist's position */
    key(a, "abba", "zzz"); key(b, "abc", "aaa");
    CHECK(strcmp(a, b) < 0);
}

static void test_fling(void)
{
    NEAR(core_fling_albums(0), 0, 1e-6f);
    CHECK(core_fling_albums(-1.5f) > 3.5f && core_fling_albums(-1.5f) < 4.5f);  /* finger left: forwards */
    CHECK(core_fling_albums(1.5f) < -3.5f);                                       /* finger right: back */
    CHECK(core_fling_albums(-4.0f) > 25.0f);
    CHECK(core_fling_albums(-6.0f) > 60.0f);
}

static void test_voice_match(void)
{
    char a[16], b[16], m[32];
    core_sound("reign", a, sizeof a);
    core_sound("rain", b, sizeof b);
    CHECK(!strcmp(a, b));                       /* the speech-to-text spelling still finds Purple Rain */
    core_sound("phil", a, sizeof a);
    core_sound("fill", b, sizeof b);
    CHECK(!strcmp(a, b));
    core_sound("prince", a, sizeof a);
    core_sound("purple", b, sizeof b);
    CHECK(strcmp(a, b) != 0);
    CHECK(core_word_match("prince", "prince\x01purple rain", m, sizeof m) == 3);
    CHECK(core_word_match("reign", "prince\x01purple rain", m, sizeof m) == 2 && !strcmp(m, "rain"));
    CHECK(core_word_match("prince", "kylie\x01impossible princess", m, sizeof m) == 2);  /* sounds alike: ranked below */
    CHECK(core_word_match("purp", "prince\x01purple rain", m, sizeof m) == 1);
    CHECK(core_word_match("225", "new model army\x01" "225", NULL, 0) == 3);
    CHECK(core_word_match("22", "new model army\x01" "225", NULL, 0) == 0);     /* too short to count as the start of a word */
    CHECK(core_word_match("orb", "orb, the\x01ultraworld", NULL, 0) == 3);
    CHECK(core_word_match("zz", "prince\x01purple rain", NULL, 0) == 0);
}

static float phrase_sim(const char *heard, const char *name)
{
    char a[64], b[64], fa[96], fb[96];
    core_fold(heard, fa, sizeof fa);
    core_fold(name, fb, sizeof fb);
    core_phrase_key(fa, a, sizeof a);
    core_phrase_key(fb, b, sizeof b);
    return core_similarity(a, b);
}

static void test_phrase_snap(void)
{
    /* what the speech-to-text wrote (26 Oct 2026 benchmark) against the name */
    CHECK(phrase_sim("Ah Kelly", "R.Kelly") >= 0.75f);
    CHECK(phrase_sim("Akelli", "R.Kelly") >= 0.75f);
    CHECK(phrase_sim("Ocarly", "R.Kelly") >= 0.75f);
    CHECK(phrase_sim("C6 Steve", "Seasick Steve") >= 0.75f);
    CHECK(phrase_sim("C-60", "Seasick Steve") < 0.75f);       /* too little left to go on */
    CHECK(phrase_sim("Mochiba", "Morcheeba") >= 0.75f);
    CHECK(phrase_sim("Head candy", "Hed Kandi") >= 0.75f);
    CHECK(phrase_sim("Banco Dagaya", "Banco de Gaia") >= 0.75f);
    CHECK(phrase_sim("Gabor Sabbo", "Gábor Szabó") >= 0.75f);
    CHECK(phrase_sim("you two", "U2") >= 0.99f);
    /* and things that shouldn't match */
    CHECK(phrase_sim("Ah Kelly", "Kylie") < 0.75f);
    CHECK(phrase_sim("Prince", "Pink Floyd") < 0.75f);
    CHECK(phrase_sim("Madonna", "Moby") < 0.75f);
    CHECK(phrase_sim("Oasis", "Orbital") < 0.75f);
}

/* A small library in browse-key form, for ranking */
static const char *const RANK_KEYS[] = {
    "minogue, kylie\x01impossible princess", "new model army\x01thunder and consolation",
    "new model army\x01vengeance", ("pink floyd\x01" "delicate sound of thunder"), "prince\x01parade",
    "prince\x01platinum", "prince\x01purple rain", "prince\x01sign o the times", "r.kelly\x01tp-2.com",
    ("backstreet boys\x01" "black & blue"), ("various artists\x01" "essential r&b the love collection - cd 1"),
};
static const char *rank_key(int i) { return RANK_KEYS[i]; }

static void read_as(const char *in, const char *want)
{
    char out[96];
    core_read_alike(in, out, sizeof out);
    if (strcmp(out, want)) printf("  read_alike(\"%s\") = \"%s\", want \"%s\"\n", in, out, want);
    CHECK(!strcmp(out, want));
}

/* Digits, number words and Roman numerals are the same word (29 Oct 2026). */
static void test_numbers(void)
{
    CHECK(core_word_match("five", "to the 5 boroughs", NULL, 0) == 3);
    CHECK(core_word_match("three", "the 3 tenors in concert 1994", NULL, 0) == 3);
    CHECK(core_word_match("two", "chapter ii", NULL, 0) == 3);
    CHECK(core_word_match("four", "elements series iv: agua", NULL, 0) == 3);
    CHECK(core_word_match("six", "10 days off - vol 6", NULL, 0) == 3);
    CHECK(core_word_match("volume", "10 days off - vol 6", NULL, 0) == 1);
    /* and single letters stay letters */
    CHECK(core_word_match("one", "i hate models", NULL, 0) == 0);
    CHECK(core_word_match("ten", "x-ray spex", NULL, 0) == 0);
}

static void test_read_alike(void)
{
    read_as("ke$ha", "kesha");
    read_as("big$hot", "bigshot");
    read_as("p!nk", "pink");
    /* a lone "@" is the word "at": "PAN-POT Live @ Fabric London" (29 Oct 2026) */
    read_as("pan-pot live @ fabric london", "pan-pot live at fabric london");
    read_as("boys noize @thelotradio", "boys noize athelotradio");
    /* symbols that are said out loud (29 Oct 2026) */
    read_as("black & blue", "black and blue");
    read_as("essential r&b", "essential r and b");
    read_as("bits + pieces", "bits and pieces");
    read_as("100% colombian", "100 percent colombian");
    /* initials run together, as they are said and as whisper writes them */
    read_as("l.a. woman", "la. woman");
    read_as("r.e.m. - around the sun", "rem. - around the sun");
    read_as("run d.m.c.", "run dmc.");
    read_as("\xC2\xA5" "ou$uk" "\xE2\x82\xAC" " " "\xC2\xA5" "uk1mat$u", "yousuke yukimatsu");  /* ¥ØU$UK€ ¥UK1MAT$U, Ø folded */
    /* left alone */
    read_as("$100 bill", "$100 bill");
    read_as("1999", "1999");
    read_as("u2", "u2");
    read_as("blink-182", "blink-182");
    read_as("freedom! '90", "freedom! '90");
    read_as("gábor", "gábor");
    /* found by how it's said: Yusuke Yukimatsu (Darren, 27 Oct 2026) */
    char folded[96], words[8][32];
    core_fold("\xC2\xA5\xC3\x98U$UK\xE2\x82\xAC \xC2\xA5UK1MAT$U", folded, sizeof folded);
    char read[96];
    core_read_alike(folded, read, sizeof read);
    int nw = core_search_words("yusuke yukimatsu", words, 8);
    CHECK(nw == 2 && core_word_match(words[0], read, NULL, 0) >= 2 && core_word_match(words[1], read, NULL, 0) == 3);
}

static void test_rank_albums(void)
{
    char words[8][32], fixed[8][32];
    core_candidate_t cand[CORE_SEARCH_CANDIDATES];
    int total = (int)(sizeof RANK_KEYS / sizeof RANK_KEYS[0]);
    /* filler words go; "reign" finds Purple Rain first and is respelled */
    int nw = core_search_words("Play Prince, Purple Reign.", words, 8);
    CHECK(nw == 3 && !strcmp(words[0], "prince") && !strcmp(words[2], "reign"));
    int n = core_rank_albums(words, nw, total, rank_key, cand, fixed);
    CHECK(n >= 1 && cand[0].album == 6 && cand[0].missing == 0);
    CHECK(!strcmp(fixed[2], "rain"));
    /* and only Purple Rain: Parade and Platinum sound close as phrases ("Prince"
     * is most of it) but don't have the words (Darren's search, 27 Oct 2026) */
    nw = core_search_words("prince purple rain", words, 8);
    n = core_rank_albums(words, nw, total, rank_key, cand, fixed);
    CHECK(n == 1 && cand[0].album == 6);
    /* "prince" alone: his albums, not Kylie's "Impossible Princess" first */
    nw = core_search_words("prince", words, 8);
    n = core_rank_albums(words, nw, total, rank_key, cand, fixed);
    CHECK(n >= 4);
    for (int i = 0; i < 4 && i < n; i++) CHECK(cand[i].album >= 4 && cand[i].album <= 7);  /* his four before Kylie's */
    /* "new model army 225": both albums are all-but-one, none complete */
    nw = core_search_words("new model army 225", words, 8);
    n = core_rank_albums(words, nw, total, rank_key, cand, fixed);
    CHECK(n == 2 && cand[0].missing == 1 && cand[1].missing == 1);
    /* the phrase by sound: "ah kelly" is R. Kelly */
    nw = core_search_words("ah kelly", words, 8);
    n = core_rank_albums(words, nw, total, rank_key, cand, fixed);
    CHECK(n >= 1 && cand[0].album == 8 && cand[0].missing == 0);
    /* one word said as two: "back street boys" is the Backstreet Boys, who
     * before this were two words short of being found at all (30 Oct 2026) */
    nw = core_search_words("back street boys black and blue", words, 8);
    n = core_rank_albums(words, nw, total, rank_key, cand, fixed);
    CHECK(n >= 1 && cand[0].album == 9 && cand[0].missing == 0);
    /* "the" says nothing: a misheard "the nerve" used to score as a complete
     * two-word match because one of its two words was "the" */
    nw = core_search_words("the verve", words, 8);
    CHECK(nw == 1 && !strcmp(words[0], "verve"));
    nw = core_search_words("prince featuring the revolution", words, 8);
    CHECK(nw == 2 && !strcmp(words[0], "prince") && !strcmp(words[1], "revolution"));
    /* "disc 1" and "CD 1" are the same thing, whichever he says */
    nw = core_search_words("essential r and b love collection disc one", words, 8);
    n = core_rank_albums(words, nw, total, rank_key, cand, fixed);
    CHECK(n >= 1 && cand[0].album == 10 && cand[0].missing == 0);
}

int main(void)
{
    test_skip_hold();
    test_read_alike();
    test_numbers();
    test_list_drag_follow();
    test_menu_snap();
    test_fisheye();
    test_rank_albums();
    test_phrase_snap();
    test_voice_match();
    test_letters();
    test_fling();
    test_sort_keys();
    test_flick_albums();
    test_bezier();
    test_menu_positions();
    test_menu_drag();
    test_menu_release();
    test_album_gestures();
    test_marquee();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
