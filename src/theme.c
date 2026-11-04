// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme.h"

#include <string.h>

static const OskPalette PRESETS[] = {
    {"深空蓝 / Deep Space", 0xFF0A0C12, 0xFF12151F, 0xFF1A1E2A, 0xFF161A24,
     0xFF232838, 0xFFE6EDF5, 0xFF9AA6B8, 0xFF64748B, 0xFF4D8BFF, 0xFF00E5FF,
     0xFF2A3042, 0xFF6EA2FF, false},
    {"纸白 / Paper White", 0xFFF2F4F7, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFF7F9FC,
     0xFFFFFFFF, 0xFF1A1C22, 0xFF5E6777, 0xFF9AA3B2, 0xFF3A76F0, 0xFF00B4D8,
     0xFFD8DEE8, 0xFF3A76F0, true},
    {"落日霓虹 / Sunset", 0xFF0E0812, 0xFF1A101F, 0xFF241526, 0xFF1C1121,
     0xFF32203A, 0xFFF4ECF7, 0xFFB9A2C4, 0xFF7D6689, 0xFFFF6B9A, 0xFFFFB84C,
     0xFF3A2442, 0xFFFF8FB2, false},
    {"森林复古 / Forest", 0xFF0B120E, 0xFF121A15, 0xFF18231C, 0xFF141E18,
     0xFF24352A, 0xFFE8F1E9, 0xFF9DB3A2, 0xFF647A6B, 0xFF5DD39E, 0xFFE8C468,
     0xFF2A3B30, 0xFF7BE5B4, false},
};

int osk_palette_count(void)
{
    return (int)(sizeof(PRESETS) / sizeof(PRESETS[0]));
}

const OskPalette *osk_palette_preset(int index)
{
    if (index < 0 || index >= osk_palette_count()) index = 0;
    return &PRESETS[index];
}

void osk_palette_resolve(OskPalette *out, const OskConfig *cfg,
                         const OskPalette *override_palette)
{
    if (!out) return;
    if (override_palette) *out = *override_palette;
    else *out = *osk_palette_preset(cfg ? cfg->theme_preset : 0);

    if (!cfg) return;
    if (cfg->color_bg) out->bg = cfg->color_bg;
    if (cfg->color_surface) out->surface = cfg->color_surface;
    if (cfg->color_card) out->card = cfg->color_card;
    if (cfg->color_card_soft) out->card_soft = cfg->color_card_soft;
    if (cfg->color_elevated) out->elevated = cfg->color_elevated;
    if (cfg->color_text) out->text = cfg->color_text;
    if (cfg->color_text2) out->text2 = cfg->color_text2;
    if (cfg->color_text3) out->text3 = cfg->color_text3;
    if (cfg->color_accent) out->accent = cfg->color_accent;
    if (cfg->color_accent2) out->accent2 = cfg->color_accent2;
    if (cfg->color_border) out->border = cfg->color_border;
    if (cfg->color_border_focus) out->border_focus = cfg->color_border_focus;
}

uint32_t osk_contrast_text(uint32_t bg)
{
    int r = (int)((bg >> 16) & 0xFF);
    int g = (int)((bg >> 8) & 0xFF);
    int b = (int)(bg & 0xFF);
    int luma = (r * 299 + g * 587 + b * 114) / 1000;
    return luma > 150 ? 0xFF10131A : 0xFFFFFFFF;
}
