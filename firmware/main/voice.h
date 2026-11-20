#pragma once

#include "ui.h"

/* Voice search: set up the microphone and the worker. NULL if the microphone
 * isn't there. voice_poll() hands results to the UI on the LVGL thread. */
void voice_mic_init(void);  /* before Wi-Fi starts */
const ui_voice_t *voice_init(void);
void voice_poll(void);
void voice_test_heard(const char *text);  /* test: search these words as if heard */
void voice_test_stt(void);                /* test: send the last recording again */
void voice_log(void);                     /* log the last attempts */
void voice_clear(void);                   /* forget them */
