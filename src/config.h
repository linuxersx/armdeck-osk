// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ARMDECK_OSK_CONFIG_H
#define ARMDECK_OSK_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Keyboard layout variants. */
enum {
    OSK_LAYOUT_FULL = 0,
    OSK_LAYOUT_COMPACT,
    OSK_LAYOUT_SPLIT,
};

/* Placement mode. */
enum {
    OSK_MODE_DOCKED = 0,
    OSK_MODE_FLOATING,
};

typedef struct {
    /* Theme */
    int theme_preset;
    int lock_theme;
    uint32_t color_bg;
    uint32_t color_surface;
    uint32_t color_card;
    uint32_t color_card_soft;
    uint32_t color_elevated;
    uint32_t color_text;
    uint32_t color_text2;
    uint32_t color_text3;
    uint32_t color_accent;
    uint32_t color_accent2;
    uint32_t color_border;
    uint32_t color_border_focus;

    /* Layout */
    int layout;
    int height_pct;
    int margin;
    int gap;
    int radius;
    int split_gap;

    /* Placement */
    int mode;
    int float_x;
    int float_y;
    int float_w;
    int float_h;

    /* Input */
    int default_lang;
    int show_candidates;
    int font_scale;
    int candidate_height;
    int animate;
    int anim_ms;
} OskConfig;

void osk_config_defaults(OskConfig *cfg);
void osk_config_path(char *out, size_t n);
void osk_config_load(OskConfig *cfg);
bool osk_config_save(const OskConfig *cfg);

#endif
