// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ARMDECK_OSK_THEME_H
#define ARMDECK_OSK_THEME_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

typedef struct {
    const char *name;
    uint32_t bg;
    uint32_t surface;
    uint32_t card;
    uint32_t card_soft;
    uint32_t elevated;
    uint32_t text;
    uint32_t text2;
    uint32_t text3;
    uint32_t accent;
    uint32_t accent2;
    uint32_t border;
    uint32_t border_focus;
    bool light;
} OskPalette;

int osk_palette_count(void);
const OskPalette *osk_palette_preset(int index);

/* Resolve the effective palette: built-in preset, optionally overridden by a
 * palette sent by the desktop, then by explicit per-color config overrides. */
void osk_palette_resolve(OskPalette *out, const OskConfig *cfg,
                         const OskPalette *override_palette);

/* Text/icon color that stays readable on top of `bg`. */
uint32_t osk_contrast_text(uint32_t bg);

#endif
