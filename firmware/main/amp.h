/* Is the amp actually listening? (2 Nov 2026)
 *
 * The Denon is deaf for about two minutes after its mains comes on. Measured
 * from cold: it answers ping at 11 seconds and does not serve HTTP until 120.
 * Audio works long before that, so the room sounds alive while every control
 * command is silently thrown away - and because Music off powers the amp down,
 * every Music on is a cold start and opens a fresh two-minute hole.
 *
 * Darren met that hole repeatedly on 2 Nov: the screen was up, Home Assistant
 * answered, the arc moved, and the volume did nothing, with nothing anywhere
 * saying why. His own reading of it was the right one - the missing piece is
 * not another automation, it is telling him.
 *
 * Home Assistant cannot tell him. Its own view of the amp is not trustworthy
 * here: through a 31 second power cut the integration never once marked the
 * amp unavailable, and it reported "on" four seconds after the mains came
 * back, while the amp was still a minute and a half from answering anything.
 *
 * So the knob asks the amp itself. It is on the same network as the amp and
 * one small GET answers the only question that matters - can this amp be told
 * anything right now - with no opinion in the way. The same probe catches the
 * amp locking up later in the day, because a locked amp stops answering too,
 * and that is the same answer as far as the screen is concerned. */
#pragma once

#include <stdbool.h>

typedef enum {
    AMP_UNKNOWN,     /* not looked yet, or not looking (music off, or screen off) */
    AMP_CONNECTING,  /* asked and got nothing: it is booting, or it has hung */
    AMP_READY,       /* it answered: anything sent now will land */
} amp_state_t;

void amp_start(void);  /* once Wi-Fi is up */

/* Whether to ask at all: true only while the music is meant to be on and the
 * screen is on with it. Nothing is probed otherwise - with the screen off
 * there is nobody to tell, and with the music off the amp has been powered
 * down on purpose, so "it is not answering" is the asked-for state and not
 * news. */
void amp_awake(bool awake);

amp_state_t amp_get_state(void);
bool amp_ready(void);  /* AMP_READY - shorthand for the volume path */

/* How long the knob has been waiting for it, in seconds, for the screen to
 * show progress rather than a word that never changes. */
int amp_connecting_secs(void);
