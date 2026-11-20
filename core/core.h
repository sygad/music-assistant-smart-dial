/*
 * Bathroom Music Knob: core logic.
 *
 * Plain C with no ESP-IDF or LVGL dependencies, so it builds and is unit-tested
 * on the Mac. Every number here comes from the reference design (docs/SPEC.md,
 * DESIGN-TOKENS.md, TIMINGS.md and the prototype's behaviour). Don't adjust by eye.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Design space ---------------------------------------------------------- */
#define CORE_SCREEN 360

/* ---- Gesture thresholds (TIMINGS.md) ---------------------------------------- */
#define CORE_DRAG_START_PX      5   /* movement before a touch counts as a drag     */
#define CORE_SWIPE_H_PX         64  /* horizontal travel to change album or track   */
#define CORE_SWIPE_V_PX         40  /* vertical travel to trigger up/down           */
#define CORE_SWIPE_V_ALBUMS_PX  60  /* vertical travel from album browse            */

/* A finger coming in over the bezel onto the glass, as against one put down on
 * the glass and then dragged (Darren, 30 Oct 2026). All three numbers are
 * measured off his own fingers on the knob, from the INPUT line app_main logs
 * with every touch.
 *
 * Where it lands. Forty-odd swipes in over the top and left bezels were first
 * seen between 0 and 27 px in, while the nearest thing started on the glass
 * was 54 px in. 32 covers every one of them and is still 22 px clear of the
 * closest press on the glass, with the dwell behind it.
 *
 * Which way it is going. Not "mostly straight in", which is what this asked
 * for first and why coming in over the top corners kept being missed: the
 * screen is round, so a finger entering away from the middle of an edge
 * travels diagonally towards the centre, and five of his ten top swipes came
 * in at an angle steep enough to be thrown out. What matters is that it heads
 * for the middle of the screen, so that is what is asked now, with 0.35 being
 * the cosine of 70 degrees: anywhere within 70 degrees of straight inward
 * counts. A finger sliding along the rim instead of into the screen does not,
 * which is the only thing this has to rule out. 60 degrees was tried first and
 * left one of his swipes sitting on the line at 0.489; over the 76 touches
 * logged, opening it to 70 takes not one touch that started away from an edge,
 * so the margin is free.
 *
 * When it is asked matters as much as what is asked, and getting that wrong
 * cost another nine of twenty-two: a finger rolling in over the top right is
 * still travelling sideways along the rim at the 5 px where a touch first
 * counts as a drag, and only turns in afterwards. Over the 20 px below those
 * nine were heading 0.94 to 1.00 straight at the middle. So the direction is
 * judged once, when the swipe has gone CORE_EDGE_TRAVEL_PX, not at the first
 * flicker of movement (ui_touch_move).
 *
 * How soon it moves. The panel does see a bezel swipe sitting still for a
 * sample or two, 25 to 75 ms of it, which the first guess of 30 ms threw away.
 * A finger put down on the glass and dragged rested 65 to 155 ms. 120 clears
 * the slowest bezel swipe by 45 ms, which 80 did not: one of his was measured
 * at 75 and only just got through. It is the band above that keeps the glass
 * presses out, not this - not one touch that started more than 32 px in is
 * taken even with the dwell opened right up - so this is the backstop for a
 * finger put down inside the band itself and dragged slowly inward, and it
 * still turns away the 125 and 155 ms of a deliberate drag.
 *
 * How far it goes. This was the biggest cause of the swipe being missed, and
 * the least expected: asking for the 40 px of a normal swipe threw out more
 * than half of them, because a swipe whose only job is to leave the screen is
 * a flick, not a drag. His ranged from 14 to 88 px and half were under 40. The
 * bezel, the direction and the dwell have already made the gesture specific by
 * the time this is asked, so it only has to tell a swipe from a tap: 20 px, or
 * four times the threshold at which a touch counts as a drag at all.
 *
 * Over the 81 touches logged on 30 Oct 2026, these four take 49 of his 50
 * bezel swipes, the 50th being a 14 px flick, and send none of the 30 touches
 * that started away from an edge anywhere. The rule they replaced took 18. */
#define CORE_EDGE_PX            32  /* first contact no further in than this       */
#define CORE_EDGE_INWARD        0.4f /* with this much of its travel away from that edge */
#define CORE_EDGE_DWELL_MS     120  /* and past the drag threshold within this     */
#define CORE_EDGE_TRAVEL_PX     20  /* and carried at least this far in to be meant */

/* ---- Menu (screen 02) ------------------------------------------------------- */
#define CORE_MENU_PITCH      60     /* item pitch                                   */
#define CORE_MENU_TOP        150    /* list offset when item 0 is selected          */
#define CORE_MENU_OVERSCROLL 26     /* rubber-band allowance past either end        */
/* A list's rows hold a title over a detail line, both at the menu's weight and
 * close to its size, so they need more room than a menu item (Darren, 28 Oct
 * 2026: at the menu's 60 the lines and the rows were squashed together).
 * 43 px of title plus 23 of detail, with 18 left over, as the menu has. */
#define CORE_LIST_PITCH      84
#define CORE_MENU_SNAP_MS    220
#define CORE_MENU_COLOUR_MS  180
/* Detent feel (Darren, 26 Oct 2026: the design's light magnet felt like glass on
 * glass). 0 = follows the finger exactly, 1 = the list stops dead at each item. */
#define CORE_MENU_DETENT     0.85f
#define CORE_MENU_EDGE       0.35f  /* how far past the first or last item it gives */
/* Snapping into place, after a drag or a knob click: ease out only, straight in
 * with no overshoot (Darren, 27 Oct 2026: with the fisheye, the magnet's 3%
 * overshoot showed as a bounce through every item). The album slide's curve.
 * CSS cubic-bezier. */
#define CORE_MENU_SNAP_EASE  0.2f, 0.9f, 0.3f, 1.0f

/* ---- Animation timings (prototype) ------------------------------------------ */
#define CORE_ALBUM_SLIDE_MS  260    /* album browse strip (.album-track.snap)       */
#define CORE_TRACK_SLIDE_MS  280    /* now playing strip (.np-strip.snap)           */
#define CORE_LABEL_HOLD_MS   1100   /* volume / skip label                          */
/* While a track is playing the same slot carries the play time, so a volume or
 * skip label is standing in front of something rather than on its own: it
 * hands the slot back instead of fading to nothing (Darren, 31 Oct 2026).
 * It hands it back quickly: five seconds, and then three, both read as the
 * screen being slow to come back, because the label isn't telling him
 * anything he is waiting for - the dial is under his hand and the room is
 * already louder, so the number has done its job by the time he has read it
 * (Darren, 2 Nov 2026). Short enough that it is gone on its own before he
 * looks for the time, and still long enough to read a two-digit number. */
#define CORE_CHIP_BACK_MS    1500
#define CORE_LABEL_FADE_MS   200
#define CORE_ARC_MS          100

/* Darren, 26 Oct 2026: swiping tracks while playing borrows the rim for the
 * track marker, which stays this long after landing before the volume returns;
 * skips this close together add up in the label (+5s, +10s, +15s...). */
#define CORE_TRACK_MARK_HOLD_MS 600
#define CORE_SKIP_RUN_MS     1000

/* Holding back or forward skips faster and faster (Darren, 27 Oct 2026: an
 * hour-long DJ set can't be crossed 5 s at a time). After CORE_SKIP_HOLD_MS
 * held, a step every CORE_SKIP_REPEAT_MS: 30 s at a time up to 2 minutes
 * skipped, then a minute at a time up to 5, then 5 minutes at a time. */
#define CORE_SKIP_HOLD_MS    500
#define CORE_SKIP_REPEAT_MS  450
/* A tap on back or forward moves five seconds, and so does one detent while
 * the scrubber is up. The two ways of nudging the position derive from here
 * rather than agreeing by coincidence. */
#define CORE_SKIP_TAP_S      5

/* ---- Scrubbing (Darren, 1 Nov 2026) --------------------------------------------
 * Tapping the play time opens the track full screen - the time big, a slider
 * under it - and a detent or a swipe moves the position. The way in is the
 * thing that tells you where you are, as the letter badge is in Albums.
 *
 * Scrubbing is hunting, not addressing (Darren, 2 Nov 2026): you are looking
 * for a part of a song, so you throw the handle somewhere, listen, and throw
 * it again. So the music goes where the hand left it the moment the finger
 * lifts, and the scrubber stays up for the next throw - the screen closing is
 * what waits, not the seek.
 *
 * Nothing goes out while the position is still moving: crossing a four minute
 * track is fifty detents, and fifty seeks would be fifty track restarts as far
 * as the stream is concerned. A finger has a release to commit on; the knob
 * has none, so a detent commits CORE_SCRUB_SETTLE_MS after the last one - long
 * enough to swallow a spin, short enough to pass for letting go.
 *
 * CORE_SCRUB_REST_MS of no scrubbing at all closes the screen, by which time
 * the music is already where he put it. It is generous because listening to
 * judge the landing is part of the hunt, and because closing early costs him
 * another tap on the play time. Longer again before the first move, because a
 * tap has to be followed by a reach for the knob: the same bargain
 * CORE_LETTER_OPEN_MS makes in Albums. */
/* A seek has been asked for and the player has yet to carry it out. Until it
 * does - within about 140 ms, measured - it goes on answering with the
 * position from before the seek, and adopting that drags the clock back over a
 * move that has already happened. So a reported position more than
 * CORE_SEEK_SLACK_MS from where the seek says the music should be by now is
 * passed over, for at most CORE_SEEK_WAIT_MS, after which the player is
 * believed whatever it says: a seek it quietly refused must not leave the
 * clock wrong for good. Only the position is doubted - never whether the music
 * is playing, nor which track it is on (Darren, 2 Nov 2026: holding the whole
 * report back starved the knob of both, and a hunt restarted that blackout
 * with every throw). */
#define CORE_SEEK_WAIT_MS    4000
#define CORE_SEEK_SLACK_MS   2500

/* How long after a seek before the room hears it. Measured on the real chain
 * (Darren, 2 Nov 2026), from the WiiM's own reported state: the ask goes out,
 * and 1.18 s later the streamer is idle with a *new* stream URL, buffers for
 * 0.60 s, and plays - 1.78 s in all. Music Assistant does not seek within the
 * stream it is already serving; it tears that one down and serves the rest of
 * the track as a new one, which is why the knob cannot make this quick. Music
 * Assistant reports the player as "playing" throughout, so there is nothing to
 * wait on - only this, which is why it is a measured number and not a signal.
 *
 * It exists so the scrubber does not close over the top of the sound it asked
 * for. The dwell before closing is counted from here, not from the finger
 * leaving the glass: the hunt is throw, listen, throw again, and he had a
 * fraction of a second of listening before the screen took the next throw
 * away. Re-measure it if the player changes. */
#define CORE_SEEK_SOUND_MS   1800

#define CORE_SCRUB_SETTLE_MS 350
#define CORE_SCRUB_REST_MS   3000
#define CORE_SCRUB_OPEN_MS   5000

/* The next step of a held skip, in seconds, after `total` seconds so far. */
int core_skip_hold_step(int total);

/* The skip label: "+5s", "-25s", "+1:30", "+15:00", "0s" (seconds, signed;
 * the minus is a real minus sign). */
void core_skip_label(int seconds, char *dst, size_t n);

/* The play time, every unit two digits: "00:07", "03:42", "01:02:03" past the
 * hour. A single leading digit read as unfinished (Darren, 1 Nov 2026). */
void core_time_label(int seconds, char *dst, size_t n);

/* The same time with no hours field: "64:12" for an hour-long DJ set. The
 * scrubber's time is 96px extralight, the clock's own size, and "01:04:12" at
 * that size is wider than a 360px round screen has room for at that height -
 * it would clip against the rim. Five characters always fit. */
void core_minutes_label(int seconds, char *dst, size_t n);

/* ---- Voice search ------------------------------------------------------------ */

/* A rough sound-alike key for one folded word, so speech-to-text spellings
 * still match: "reign" and "rain" both give "rn", "phil" and "fill" "fl".
 * Digits are kept as they are. */
void core_sound(const char *word, char *dst, size_t n);

/* How well one spoken (folded) word matches the words of `text` (folded):
 * 3 the same word, 2 a word that sounds alike, 1 the start of a longer word,
 * 0 none. The best-matching word of `text` goes to `matched` (may be NULL). */
int core_word_match(const char *word, const char *text, char *matched, size_t n);

/* The sound of a whole (folded) phrase, for comparing with core_similarity():
 * single letters and digits are read out as words ("r kelly" -> "ar kelly",
 * "u2" -> "you two") and every word goes through core_sound(). */
void core_phrase_key(const char *text, char *dst, size_t n);

/* How alike two phrase keys are: 1 the same, 0 nothing alike (edit distance
 * over the longer length). Keys of 3 or fewer sounds only count if equal. */
float core_similarity(const char *a, const char *b);

/* Names styled with symbols for letters, read as the letters (folded text in,
 * folded text out; Darren, 27 Oct 2026): "ke$ha" -> "kesha", "p!nk" -> "pink",
 * "¥øu$uk€ ¥uk1mat$u" -> "yousuke yukimatsu". A symbol counts next to a
 * letter, "!" only inside a word, a digit only between letters, so "$100",
 * "1999", "u2" and "blink-182" stay as they are. */
void core_read_alike(const char *src, char *dst, size_t n);

/* Fold a heard phrase and split it into words (up to `max`, each under 32
 * characters), dropping "play", "search for", "by" and the like. */
int core_search_words(const char *text, char words[][32], int max);

/* The same phrase for Music Assistant's search, which matches it whole: the
 * words of core_search_words as `fixed` (spelling corrected), with "the", "a"
 * and "an" put back where they were said. Up to `max` words. */
#define CORE_PHRASE_WORDS 16
int core_search_phrase(const char *text, char fixed[][32], int nfixed, char out[][32], int max);

/* An album found for the heard words: `missing` words it doesn't have (0 or 1),
 * `score` its rank (higher first), `by_sound` found only by the sound of the
 * whole phrase, not by its words. */
typedef struct {
    int album, score, missing;
    bool by_sound;
} core_candidate_t;
#define CORE_SEARCH_CANDIDATES 48
/* The most spoken words a search looks at; one value, so the caller's array and
 * the ranking's own working room can never disagree. */
#define CORE_SEARCH_WORDS 8

/* Rank every album (`key(i)` its browse key, see core_album_key) against the
 * words, best first, into `cand` (CORE_SEARCH_CANDIDATES long); returns how
 * many. `fixed` gets the words as the best album spells them ("reign" ->
 * "rain"), for searching tracks. */
int core_rank_albums(char words[][32], int nwords, int total, const char *(*key)(int album),
                     core_candidate_t *cand, char fixed[][32]);

/* ---- Text overflow ---------------------------------------------------------- */
#define CORE_TITLE_SPACE_PX  220    /* usable width at the title band's narrow edge */
#define CORE_TITLE_LEFT_PX   70     /* left inset when the title scrolls            */

/* Scrolling titles. Changed from the prototype on 26 Oct 2026 at Darren's request:
 * the prototype held for 18% of a cycle that grows with the title's length (6.5 s
 * before a long title moved). Now: a short pause, then scroll at the prototype's
 * speed. */
/* 1.5 s before each slide out, on landing and again after each return: a
 * moment to take the title in (Darren, 27 Oct 2026; was 1 s) */
#define CORE_MARQUEE_HOLD_START_MS 1500
#define CORE_MARQUEE_HOLD_END_MS   1500
#define CORE_MARQUEE_MS_PER_PX     20   /* 50 px/s, the prototype's speed */
/* Each slide runs at that speed, then slows to a stop over its last 15%: an ease
 * out only (Darren, 27 Oct 2026; the prototype's ease-in-out felt unnatural). */
#define CORE_MARQUEE_EASE_PCT      15

/* Cubic-bezier easing, same definition as CSS cubic-bezier(x1, y1, x2, y2).
 * t in 0..1, returns progress 0..1. */
float core_bezier(float x1, float y1, float x2, float y2, float t);

/* ---- Menu ------------------------------------------------------------------- */

/* List offset (y of the list's top edge) that centres item i under the pill. */
int core_menu_y(int i);
/* The same, for a list whose rows are taller than the menu's (CORE_LIST_PITCH). */
int core_menu_y_at(int i, int pitch);
float core_list_drag_at(int sel, float dy, int count, float detent, int *highlight, int pitch);
int core_menu_release_at(int sel, float dy, int count, int pitch);

/* While dragging: the list offset to draw for a drag of dy pixels that started
 * with `sel` selected, with a detent at each item (see CORE_MENU_DETENT).
 * *highlight receives the item currently under the pill. */
float core_menu_drag(int sel, float dy, int count, int *highlight);
/* The same with its own detent strength (0 follows the finger exactly). */
float core_list_drag(int sel, float dy, int count, float detent, int *highlight);

/* On release after a drag: the item that ends up selected. */
int core_menu_release(int sel, float dy, int count);

/* Fisheye, for every list (Darren, 27 Oct 2026): an item `d` items from the
 * pill (fractional while the list moves) is drawn this much smaller, 15% per
 * item down to 62%, and in this grey (0-255) when not selected: the menu's grey
 * one item away, slightly darker beyond, never below 0x60. */
#define CORE_FISH_SHRINK    0.15f
#define CORE_FISH_MIN       0.62f
#define CORE_FISH_GREY      0x80
#define CORE_FISH_GREY_STEP 24
#define CORE_FISH_GREY_MIN  0x60
float core_fisheye_scale(float d);
int core_fisheye_grey(float d);

/* ...and spaced to match (Darren, 27 Oct 2026): where an item `d` items from
 * the pill (signed) is drawn, in items from the pill. The gap to each next item
 * shrinks with its size, and the items next to the pill get CORE_FISH_GAP more
 * room: 60 px apart becomes about 64, then 47, then 39. */
#define CORE_FISH_GAP 0.15f
float core_fisheye_offset(float d);

/* ---- Album browse ----------------------------------------------------------- */

typedef enum {
    CORE_ALBUM_STAY,     /* snap back to the current album  */
    CORE_ALBUM_NEXT,
    CORE_ALBUM_PREV,
    CORE_ALBUM_TO_MENU,  /* swipe down                       */
} core_album_action_t;

/* What a released drag (dx, dy) does in album browse. */
core_album_action_t core_album_release(float dx, float dy);

/* True once a touch has moved far enough to count as a drag. */
bool core_is_drag(float dx, float dy);

/* ---- Letters -------------------------------------------------------------------
 * Replaced 28 Oct 2026. Letters used to be entered by how fast you turned the
 * knob or flicked the screen, on the idea that a fast gesture means "get me
 * out of here". Three thresholds were measured and tried against Darren's own
 * recorded gestures, and none of them worked: the opening detents of a long
 * fast spin are identical to a flick, so it jumped letters when he only meant
 * to scroll, and waiting for the turn to end to tell them apart made it late.
 *
 * Darren's answer: stop inferring intent. Album browse shows the letter you
 * are on in a badge at the top - which he had no way of seeing before - and
 * tapping that badge is the switch into letters. Turning or swiping then
 * steps one letter at a time, and it drops back to albums CORE_KNOB_REST_MS
 * after the input stops. Speed decides nothing, so the knob can be spun as
 * hard as he likes and the albums just come as fast as they can be loaded.
 *
 * How long it waits depends on what has to happen next. Freshly opened it
 * allows CORE_LETTER_OPEN_MS, because the tap has to be followed by a reach
 * for the knob. A finger stepping the letters gets CORE_LETTER_SWIPE_MS: a
 * swipe has to be lifted and set down again, so it repeats more slowly than a
 * detent, and the same 600 ms that suits the knob kept throwing him out
 * halfway through swiping (Darren, 28 Oct 2026). The knob itself keeps
 * CORE_KNOB_REST_MS, which is right for how quickly detents arrive. */
/* Spinning hard enough to be travelling, not browsing, is a second way into
 * letters - the badge being the first. It only decides the way in: from there
 * it is the same mode, a letter per detent, ending when the turning stops.
 * Darren's own three paces, measured 29 Oct 2026: browsing 6.3 albums a
 * second, hunting through a letter 12.5, and spinning to cross the alphabet
 * 19.3. Judged over a moment and a half rather than a few detents, because the
 * hunting pace comes in bursts that touch 22 a second while the spin is simply
 * sustained - over a shorter window the two cannot be told apart, which is how
 * an earlier attempt at this kept interrupting him. His fastest sustained
 * hunting was 14.0, so 17 leaves room for a more energetic day. */
#define CORE_FLY_WINDOW_MS  1300
/* 15 over 1.3 s: twenty detents. Tapping the badge is the way into letters;
 * this is the extra one, for when he is spinning in earnest, so it is set to
 * err towards not firing. Of every setting tried against six recorded runs
 * across two sessions, this is the fewest detents that never once trips while
 * he is only browsing - 14 over 1.2 s needed three fewer, and he found it
 * caught him mid-hunt (29 Oct 2026).
 *
 * It still takes about a second, and that is not the threshold. A sustained
 * speed needs a slice of the spin to establish, and his fast spin only lasts
 * 2.2 s, so it cannot be told from a burst much sooner however low the bar. */
#define CORE_FLY_IN         15.0f  /* detents a second, sustained, and it is letters */
/* Letters have a ceiling of their own, whatever the knob is doing. Arriving at
 * a hard spin used to hand every remaining detent straight to the alphabet:
 * replaying Darren's own ludicrous run, it crossed over with 94 detents still
 * to come, which is three and a half times round a 27-letter alphabet before
 * his hand could stop (29 Oct 2026). At this ceiling the whole alphabet still
 * takes about three seconds and overshooting costs a letter or two. */
#define CORE_LETTER_STEP_MS 125
#define CORE_LETTER_OPEN_MS 1000   /* ...longer again before the first step         */
#define CORE_LETTER_SWIPE_MS 900   /* ...and longer between swipes than detents     */
#define CORE_KNOB_REST_MS   600    /* no detent for this long ends letter mode      */
/* Swipe or flick, by how long the finger is down (Darren's recorded gestures,
 * 26 Oct 2026: flicks 35-55 ms, swipes 90-180 ms; speed overlapped). */
#define CORE_FLICK_MS       90     /* down for less than this: a flick              */
#define CORE_FLICK_MIN_PX   12     /* ...that moved at least this far               */
#define CORE_FLICK_REST_MS  700    /* letters end this long after a console flick   */

/* Albums a single flick glides: more the harder (finger speed in px/ms over the
 * flick). Darren's flicks: 1.1 px/ms gentle -> 3 albums, 2-2.9 hard -> 6-8. */
int core_flick_albums(float speed_px_ms);
/* The hand timer: this long since anybody touched the glass or turned the
 * knob, and the dial puts back one screen - a tone screen to the tone
 * overview, the overview to Now Playing, a list to the music it is playing.
 * Not to be confused with CORE_CLOCK_IDLE_MS below, which is the idle timer
 * and a different thing: that one takes the screen away when nothing at all
 * is happening, this one only unwinds something that was opened and left. */
#define CORE_IDLE_RETURN_MS 8000
#define CORE_ASK_CANCEL_MS  8000  /* a question ("Music off?") cancels itself after this untouched */
/* A one-answer question ("Louder than 80%?") goes sooner: it is in the way of
 * the volume rather than waiting on a choice, so a hand that has stopped
 * turning wants it gone (Darren, 31 Oct 2026). */
#define CORE_ASK_ONE_MS     3000

/* Volume limit (PLANNING.md): the knob stops at 80% (Denon 78); one more click
 * up asks "Louder than 80%?", and Continue allows up to 100% until the music
 * is next switched off - or until the volume has sat back at the limit or
 * below for CORE_VOLUME_RELOCK_MS. */
#define CORE_VOLUME_LIMIT   80
/* ...and the lift is only lent, not given (Darren, 2 Nov 2026): a yes given
 * twenty minutes ago shouldn't still be answering for the volume now. While
 * the volume is still past the limit the door stays open - the next click up
 * has nothing new to ask. Retreat to the limit or below and sit there this
 * long and the question comes back. Half a minute is long enough that easing
 * off and going up again is one movement, short enough that the lift doesn't
 * outlive the moment it was given in. */
#define CORE_VOLUME_RELOCK_MS 30000
/* The volume the music starts at, at switch-on and again after Music off
 * (Darren, 27 Oct 2026: a reasonably low start). */
#define CORE_VOLUME_START   30
/* ...and the bottom of the dial, not just its starting point (Darren, 5 Nov
 * 2026). Below 30 the bathroom is inaudible, so the first third of the arc was
 * always drawn full and meant nothing, and everything that can be heard was
 * crammed into the rest. The arc's empty position is this volume now, its full
 * sweep to CORE_VOLUME_LIMIT, and the knob will not go under it. The dial's
 * own 0-100 scale is unchanged - what the amp is told is the same number as
 * before - so nothing in Home Assistant moves with this. */
#define CORE_VOLUME_FLOOR   CORE_VOLUME_START
/* Music off hands the amp to Darren's own "Music Fade Off", which takes about
 * ten seconds to fade the volume down before the switches go off. The music
 * carries on through that, so what you hear is the fade and not a track cut
 * dead, and the player is only stopped once it is over (31 Oct 2026). */
#define CORE_FADE_OFF_MS    12000
/* ...and not at all while the dial is still waiting for the amp. Nothing
 * playing is not the same as nothing happening: a dial that has just been
 * switched on and cannot play anything yet is not idle, it is working.
 *
 * There is no cap on that any more. There was one - three minutes, after which
 * the clock was allowed through to protect the battery - and it was exactly
 * backwards. Darren, 4 Nov 2026: "It's not even connected to the amp at this
 * point. I want it to do nothing... If I've not even connected, you're killing
 * it before I've even managed to connect it."
 *
 * Three minutes still means something, but it now means the connection has
 * failed rather than that the wait may be abandoned. At that point the dial
 * says so, power cycles the amps and restarts itself - see
 * CORE_AMP_GIVE_UP_MS. The battery is protected by fixing the fault, not by
 * going dark in front of somebody who is waiting. */
#define CORE_AMP_GIVE_UP_MS 180000
#define CORE_CLOCK_IDLE_MS  60000  /* no music: the clock after this (a minute, to read a screen, 28 Oct) */
/* Music off: how long the screen stays where it is before the clock comes up.
 * It used to be instant, and answering "Turn off" threw you straight to the
 * clock with no moment to see that anything had happened (Darren, 2 Nov 2026).
 * Three seconds is long enough to register and short enough not to feel stuck;
 * touching the dial inside that window cancels it, and the ordinary
 * CORE_CLOCK_IDLE_MS minute then applies as it would anywhere else. */
#define CORE_MUSIC_OFF_CLOCK_MS 3000
/* ...but only when the music went off somewhere else (Home Assistant's own
 * rule, the dashboard). The dial's own Music off now counts down first
 * (Darren, 5 Nov 2026): "Turn off" turns the question into five seconds of
 * the rim winding back to 6 o'clock with the seconds in the middle, and a tap
 * or a turn in that time takes it back - nothing has been sent yet. That is
 * the moment to see it happen, so when the count runs out the clock comes
 * straight up rather than after another three seconds of menu. */
#define CORE_MUSIC_OFF_COUNT_MS 5000
/* Two minutes after the last touch or detent with the clock up, the screen goes
 * off and the dial drops into its low-power state.
 *
 * It was twenty minutes (Darren, 30 Oct 2026), and twenty minutes turned out to
 * be the single most expensive thing the dial did. Measured against how it is
 * actually used - three spells of about a quarter of an hour a day - twenty
 * minutes of fully lit screen after each of them is an hour a day of a bright
 * screen in an empty bathroom, which costs more than all the use does. Two
 * minutes is long enough to glance at the time, walk to the shower and come
 * back to a screen that is still up, and a touch or a turn brings it straight
 * back anyway (4 Nov 2026). */
#define CORE_SLEEP_MS       120000
/* The amps are let go much later than the screen: a quarter of an hour after
 * the last touch, with nothing playing. Their own countdown, because they cost
 * the dial's battery nothing and cost Darren two minutes every time they come
 * up from cold. Darren, 5 Nov 2026: he switches the dial on, goes to get his
 * clothes ready, and is back five to ten minutes later to choose the music -
 * and the amp had been switched off with the screen, two minutes after it
 * answered, so the whole wait started again. Fifteen covers that with room.
 *
 * This only ever runs with nothing playing. Music that is still playing is
 * never stopped by the dial at all - that is Home Assistant's Music Auto Off,
 * which kills it, playing or not, once the bathroom has been empty for twenty
 * minutes: the belt and braces for somebody who walked out of the room, or
 * out of the house, with a record on (Darren, 5 Nov 2026). The two do not
 * overlap and neither stands in for the other. Must stay longer than
 * CORE_SLEEP_MS and shorter than CORE_DEEP_SLEEP_MS. */
#define CORE_AMP_LET_GO_MS  900000
/* ...and after two hours of nobody touching it at all, the dial stops properly:
 * deep sleep, which is the only state where the processor, the panel, the touch
 * chip and the radio are all genuinely off.
 *
 * Two hours rather than something short, on purpose. Light sleep above is
 * already cheap enough that going deeper only buys a few hours over a day, and
 * deep sleep is not free to come back from: it takes a turn of the knob (a tap
 * cannot wake it - nothing is running to read the glass) and several seconds to
 * get Wi-Fi, Music Assistant and the covers back. Two hours means the gaps
 * between Darren's three daily spells and the whole night are covered, while a
 * dial walked up to during the day is nearly always only in light sleep and
 * comes back instantly. */
#define CORE_DEEP_SLEEP_MS  7200000
/* Deep sleep takes the dial off the network, so Home Assistant cannot reach it
 * and an update could never land unattended. It wakes itself this often behind
 * a dark screen, says what its charge is, asks whether a new firmware is
 * waiting, and goes straight back down. Six hours; a few seconds each time. */
#define CORE_DEEP_SLEEP_CHECK_MS 21600000
/* How often the idle check looks: twice a second awake, and once every two
 * seconds asleep, where the only thing it is waiting for is music starting
 * somewhere else. */
#define CORE_IDLE_CHECK_MS  500
#define CORE_SLEEP_CHECK_MS 2000
/* ...but not while a title is still scrolling: it waits until the title on
 * screen has scrolled this many times (Darren, 27 Oct 2026) */
#define CORE_CLOCK_READ_PASSES 2
#define CORE_SEARCH_GIVE_UP_MS 8000  /* "Didn't catch that": back to Now Playing or the menu after this (27 Oct) */

/* The letter an album files under: A-Z, or '#' for digits and symbols. Accented
 * Latin letters file under their base letter (É under E). */
char core_letter(const char *sort_name);

/* ---- Browse order (decided 26 Oct 2026) -------------------------------------
 * By artist, then A-Z by album title within the artist. Compilations (artist
 * "Various Artists", or tagged with the album's own name as the artist) file
 * under their own title among the artists: a Hed Kandi compilation sits under
 * H as "Hed Kandi". Names come from Music Assistant's sort names, so "The"
 * has already moved to the end ("doors, the"). */

/* Copy `src` lowercased, with accented Latin letters folded to their base
 * letter, so names compare as they read (Gábor sorts as gabor). */
void core_fold(const char *src, char *dst, size_t n);

/* True for a compilation: "Various Artists", or the artist is just the album's own name. */
bool core_is_compilation(const char *artist, const char *album);

/* The key an album sorts by: "<artist>\x01<album>", or "<album>\x01" for a
 * compilation, both folded. Compare keys with strcmp. */
void core_album_key(const char *artist_sort, const char *album_sort, bool compilation, char *dst, size_t n);

/* How many albums a flick travels, from the finger's speed at release in px/ms
 * (signed: positive = finger moving right = towards earlier albums). Grows with
 * the square of the speed, so a hard flick goes much further than a soft one. */
float core_fling_albums(float speed_px_ms);

/* ---- Text overflow ---------------------------------------------------------- */

/* Length of one scroll cycle for a title that overflows by `over` px. */
uint32_t core_marquee_cycle_ms(int over);

/* Horizontal offset (0 or negative) of a scrolling title at time t_ms into its
 * cycle. Pauses, slides left by exactly `over`, holds, slides back. */
float core_marquee_offset(int over, uint32_t t_ms);

#ifdef __cplusplus
}
#endif
