// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ARMDECK_OSK_H
#define ARMDECK_OSK_H

#include <stdbool.h>
#include <stdint.h>
#include <cairo/cairo.h>
#include "rime_engine.h"
#include "theme.h"

#define OSK_MAX_KEYS 80
#define OSK_MAX_CANDIDATES 8
#define OSK_MAX_TEXT 128

enum {
    OSK_KEY_TEXT = 0,
    OSK_KEY_BACKSPACE,
    OSK_KEY_SHIFT,
    OSK_KEY_ENTER,
    OSK_KEY_SPACE,
    OSK_KEY_LANG,
    OSK_KEY_SYMBOLS,
    OSK_KEY_HIDE,
};

typedef struct {
    int x, y, w, h;
    char label[16];
    char text[16];
    int action;
} OskKey;

typedef struct {
    int x, y, w, h;
    char text[64];
} OskCandidate;

typedef struct {
    int width, height;
    OskKey keys[OSK_MAX_KEYS];
    int key_count;
    int focus;
    int pressed;
    bool shift;
    int lang; /* 0 = EN, 1 = 中文 */
    int layer; /* 0 = letters, 1 = symbols */
    char preedit[OSK_MAX_TEXT];
    OskCandidate candidates[OSK_MAX_CANDIDATES];
    int candidate_count;
    int candidate_page;
    OskPalette theme;
    int pad_family; /* 0 none, 1 nintendo, 2 xbox, 3 playstation */
    int layout;
    int margin;
    int gap;
    int radius;
    int split_gap;
    int font_scale;
    int show_candidates;
    int candidate_height;
    int candidate_focus;
    bool candidate_mode;
    OskRime rime;
    bool rime_ready;
} Osk;

void osk_layout(Osk *osk, int width, int height);
void osk_draw(Osk *osk, cairo_t *cr);
int osk_hit(const Osk *osk, int x, int y);
int osk_move_focus(Osk *osk, int dx, int dy);

#endif
