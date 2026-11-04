// SPDX-License-Identifier: GPL-3.0-or-later
#include "osk.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <string.h>

#define COL_BG        0xFF0A0C12u
#define COL_SURFACE   0xFF12151Fu
#define COL_CARD      0xFF1A1E2Au
#define COL_CARD_SOFT 0xFF161A24u
#define COL_ELEVATED  0xFF232838u
#define COL_TEXT      0xFFE6EDF5u
#define COL_TEXT2     0xFF9AA6B8u
#define COL_TEXT3     0xFF64748Bu
#define COL_ACCENT    0xFF4D8BFFu
#define COL_ACCENT2   0xFF00E5FFu
#define COL_BORDER    0xFF2A3042u
#define COL_FOCUS     0xFF6EA2FFu

static void set_rgba(cairo_t *cr, uint32_t c, double a)
{
    cairo_set_source_rgba(cr,
        ((c >> 16) & 0xff) / 255.0,
        ((c >> 8) & 0xff) / 255.0,
        (c & 0xff) / 255.0,
        a);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

static void draw_text(cairo_t *cr, const char *text, double x, double y,
                      double w, double h, double px, uint32_t color,
                      bool center)
{
    if (!text || !*text) return;
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, "Sans");
    pango_font_description_set_absolute_size(desc, px * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, text, -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(layout, (int)(w * PANGO_SCALE));
    if (center) pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);
    set_rgba(cr, color, 1.0);
    cairo_move_to(cr, x, y + (h - th) / 2.0);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(desc);
    g_object_unref(layout);
}

static const uint32_t FOCUS_COLORS[] = {
    0xFF00E5FF, 0xFF5DD39E, 0xFFFFB84C, 0xFFFF6B9A,
    0xFFB48CFF, 0xFF4D8BFF, 0xFFE8C468, 0xFFFF6B6B,
};

static uint32_t focus_color(int index)
{
    int n = (int)(sizeof(FOCUS_COLORS) / sizeof(FOCUS_COLORS[0]));
    if (index < 0) index = 0;
    return FOCUS_COLORS[index % n];
}

static const char *pad_hint(int family, int action)
{
    if (family == 1) { /* Nintendo */
        if (action == OSK_KEY_HIDE) return "B";
        if (action == OSK_KEY_BACKSPACE) return "Y";
        if (action == OSK_KEY_SPACE) return "X";
        if (action == OSK_KEY_ENTER) return "ZR";
    } else if (family == 2) { /* Xbox */
        if (action == OSK_KEY_HIDE) return "A";
        if (action == OSK_KEY_BACKSPACE) return "X";
        if (action == OSK_KEY_SPACE) return "Y";
        if (action == OSK_KEY_ENTER) return "RT";
    } else if (family == 3) { /* PlayStation */
        if (action == OSK_KEY_HIDE) return "\u00d7";
        if (action == OSK_KEY_BACKSPACE) return "\u25a1";
        if (action == OSK_KEY_SPACE) return "\u25b3";
        if (action == OSK_KEY_ENTER) return "R2";
    }
    return NULL;
}

static void add_key(Osk *osk, int x, int y, int w, int h,
                    const char *label, const char *text, int action)
{
    if (osk->key_count >= OSK_MAX_KEYS) return;
    OskKey *k = &osk->keys[osk->key_count++];
    memset(k, 0, sizeof(*k));
    k->x = x; k->y = y; k->w = w; k->h = h; k->action = action;
    snprintf(k->label, sizeof(k->label), "%s", label ? label : "");
    snprintf(k->text, sizeof(k->text), "%s", text ? text : "");
}

static void add_text_row(Osk *osk, int x, int y, int unit, int gap,
                         int row_h, const char *letters)
{
    for (int i = 0; letters[i]; i++) {
        char s[2] = {letters[i], 0};
        add_key(osk, x + i * (unit + gap), y, unit, row_h, s, s, OSK_KEY_TEXT);
    }
}

static void add_str_row(Osk *osk, int x, int y, int unit, int gap,
                        int row_h, const char *const *items, int count)
{
    for (int i = 0; i < count; i++)
        add_key(osk, x + i * (unit + gap), y, unit, row_h,
                items[i], items[i], OSK_KEY_TEXT);
}

void osk_layout(Osk *osk, int width, int height)
{
    memset(osk->keys, 0, sizeof(osk->keys));
    osk->key_count = 0;
    osk->width = width;
    osk->height = height;
    if (width <= 0 || height <= 0) return;

    int margin = osk->margin > 0 ? osk->margin : 8;
    int gap = osk->gap >= 0 ? osk->gap : 4;
    if (margin * 2 + gap * 9 + 12 * 10 > width) { margin = 4; gap = 2; }
    int candidate_h = osk->show_candidates
                          ? (osk->candidate_height > 0 ? osk->candidate_height : 42)
                          : 0;
    int total_h = height - margin * 2 - gap * 4 - candidate_h;
    int row_h = total_h / 5;
    if (row_h < 18) row_h = 18;
    int y = margin + candidate_h;

    if (osk->layer == 1) {
        int unit = (width - margin * 2 - gap * 9) / 10;
        if (unit < 12) unit = 12;
        int used_w = unit * 10 + gap * 9;
        int left = (width - used_w) / 2;
        int u = unit, g = gap, h = row_h;
        static const char *sym1[] = {
            "\uff0c", "\u3002", "\uff01", "\uff1f", "\uff1a",
            "\uff1b", "\u201c", "\u201d", "\u2018", "\u2019",
        };
        static const char *sym2[] = {
            "\uff08", "\uff09", "\u3010", "\u3011", "\u300a",
            "\u300b", "\u3001", "\u2026", "\u2014",
        };
        static const char *sym3[] = {
            "@", "#", "\uffe5", "%", "&", "*", "+", "=",
        };
        add_text_row(osk, left, y, u, g, h, "1234567890");
        y += h + g;
        add_str_row(osk, left, y, u, g, h, sym1, 10);
        y += h + g;
        add_str_row(osk, left, y, u, g, h, sym2, 9);
        add_key(osk, left + 9 * (u + g), y, u, h,
                "\u232b", "", OSK_KEY_BACKSPACE);
        y += h + g;
        add_str_row(osk, left, y, u, g, h, sym3, 8);
        add_key(osk, left + 8 * (u + g), y, 2 * u + g, h,
                "Enter", "", OSK_KEY_ENTER);
        y += h + g;
        int x = left;
        add_key(osk, x, y, u * 3 / 2, h, "ABC", "", OSK_KEY_SYMBOLS);
        x += u * 3 / 2 + g;
        add_key(osk, x, y, u, h, ",", ",", OSK_KEY_TEXT);
        x += u + g;
        add_key(osk, x, y, u * 4 + g * 3, h, "\u7a7a\u683c", " ",
                OSK_KEY_SPACE);
        x += u * 4 + g * 4;
        add_key(osk, x, y, u, h, ".", ".", OSK_KEY_TEXT);
        x += u + g;
        add_key(osk, x, y, u * 3 / 2, h, "\u6536\u8d77", "", OSK_KEY_HIDE);
        return;
    }

    if (osk->layout == OSK_LAYOUT_SPLIT) {
        int center = width * (osk->split_gap > 0 ? osk->split_gap : 12) / 100;
        if (center < 0) center = 0;
        int pane_w = (width - margin * 2 - center) / 2;
        int unit = (pane_w - gap * 4) / 5;
        if (unit < 12) unit = 12;
        int lx = margin;
        int rx = margin + pane_w + center;
        int u = unit, g = gap, h = row_h;

        add_text_row(osk, lx, y, u, g, h, "12345");
        add_text_row(osk, rx, y, u, g, h, "67890");
        y += h + g;
        add_text_row(osk, lx, y, u, g, h, "qwert");
        add_text_row(osk, rx, y, u, g, h, "yuiop");
        y += h + g;
        add_text_row(osk, lx, y, u, g, h, "asdfg");
        add_text_row(osk, rx, y, u, g, h, "hjkl");
        add_key(osk, rx + 4 * (u + g), y, u, h, "\u232b", "", OSK_KEY_BACKSPACE);
        y += h + g;
        add_key(osk, lx, y, u, h, "Shift", "", OSK_KEY_SHIFT);
        add_text_row(osk, lx + u + g, y, u, g, h, "zxcv");
        add_text_row(osk, rx, y, u, g, h, "bnm");
        add_key(osk, rx + 3 * (u + g), y, 2 * u + g, h, "Enter", "", OSK_KEY_ENTER);
        y += h + g;
        add_key(osk, lx, y, u, h, osk->lang == 1 ? "\u4e2d" : "EN", "", OSK_KEY_LANG);
        add_key(osk, lx + u + g, y, u, h, "\u7b26\u53f7", "", OSK_KEY_SYMBOLS);
        add_key(osk, lx + 2 * (u + g), y, u, h, ",", ",", OSK_KEY_TEXT);
        add_key(osk, lx + 3 * (u + g), y, 2 * u + g, h, "\u7a7a\u683c",
                " ", OSK_KEY_SPACE);
        add_key(osk, rx, y, u, h, ".", ".", OSK_KEY_TEXT);
        add_key(osk, rx + u + g, y, 2 * u + g, h, "\u7a7a\u683c", " ",
                OSK_KEY_SPACE);
        add_key(osk, rx + 3 * (u + g), y, 2 * u + g, h, "\u6536\u8d77", "",
                OSK_KEY_HIDE);
        return;
    }

    double frac = osk->layout == OSK_LAYOUT_COMPACT ? 0.82 : 1.0;
    int avail = (int)((width - margin * 2) * frac);
    int unit = (avail - gap * 9) / 10;
    if (unit < 12) unit = 12;
    int used_w = unit * 10 + gap * 9;
    int left = (width - used_w) / 2;

    add_text_row(osk, left, y, unit, gap, row_h, "1234567890");
    y += row_h + gap;
    add_text_row(osk, left, y, unit, gap, row_h, "qwertyuiop");
    y += row_h + gap;
    add_text_row(osk, left, y, unit, gap, row_h, "asdfghjkl");
    add_key(osk, left + 9 * (unit + gap), y, unit, row_h,
            "\u232b", "", OSK_KEY_BACKSPACE);
    y += row_h + gap;
    add_key(osk, left, y, unit * 3 / 2, row_h, "Shift", "", OSK_KEY_SHIFT);
    int x3 = left + unit * 3 / 2 + gap;
    add_text_row(osk, x3, y, unit, gap, row_h, "zxcvbnm");
    add_key(osk, x3 + 7 * (unit + gap), y, unit * 3 / 2, row_h,
            "Enter", "", OSK_KEY_ENTER);
    y += row_h + gap;
    int x = left;
    add_key(osk, x, y, unit * 3 / 2, row_h,
            osk->lang == 1 ? "\u4e2d" : "EN", "", OSK_KEY_LANG);
    x += unit * 3 / 2 + gap;
    add_key(osk, x, y, unit, row_h, "\u7b26\u53f7", "", OSK_KEY_SYMBOLS);
    x += unit + gap;
    add_key(osk, x, y, unit, row_h, ",", ",", OSK_KEY_TEXT);
    x += unit + gap;
    add_key(osk, x, y, unit * 4 + gap * 3, row_h, "\u7a7a\u683c", " ",
            OSK_KEY_SPACE);
    x += unit * 4 + gap * 4;
    add_key(osk, x, y, unit, row_h, ".", ".", OSK_KEY_TEXT);
    x += unit + gap;
    add_key(osk, x, y, unit * 3 / 2, row_h, "\u6536\u8d77", "", OSK_KEY_HIDE);
}

int osk_hit(const Osk *osk, int x, int y)
{
    for (int i = 0; i < osk->key_count; i++) {
        const OskKey *k = &osk->keys[i];
        if (x >= k->x && x < k->x + k->w && y >= k->y && y < k->y + k->h)
            return i;
    }
    return -1;
}

static int center_x(const OskKey *k) { return k->x + k->w / 2; }
static int center_y(const OskKey *k) { return k->y + k->h / 2; }

int osk_move_focus(Osk *osk, int dx, int dy)
{
    if (osk->key_count <= 0) return -1;
    if (osk->focus < 0 || osk->focus >= osk->key_count) osk->focus = 0;
    const OskKey *cur = &osk->keys[osk->focus];
    int best = -1;
    long best_score = 0;
    for (int i = 0; i < osk->key_count; i++) {
        if (i == osk->focus) continue;
        const OskKey *k = &osk->keys[i];
        int vx = center_x(k) - center_x(cur);
        int vy = center_y(k) - center_y(cur);
        if (dx && ((dx > 0 && vx <= 0) || (dx < 0 && vx >= 0))) continue;
        if (dy && ((dy > 0 && vy <= 0) || (dy < 0 && vy >= 0))) continue;
        /* Left/right must stay on the same row.  Without this the nearest
         * key in the direction could be in another row when the edge is
         * reached. */
        if (dx && (k->y >= cur->y + cur->h || k->y + k->h <= cur->y)) continue;
        long primary = dx ? labs(vx) : labs(vy);
        long secondary = dx ? labs(vy) : labs(vx);
        long score = primary * 100 + secondary;
        if (best < 0 || score < best_score) { best = i; best_score = score; }
    }
    if (best >= 0) osk->focus = best;
    return osk->focus;
}

void osk_draw(Osk *osk, cairo_t *cr)
{
    double scale = osk->font_scale > 0 ? osk->font_scale / 100.0 : 1.0;
    int margin = osk->margin > 0 ? osk->margin : 8;
    double radius = osk->radius > 0 ? osk->radius : 8;
    int top = osk->show_candidates
                  ? (osk->candidate_height > 0 ? osk->candidate_height : 42)
                  : 0;
    set_rgba(cr, osk->theme.bg, 1.0);
    cairo_rectangle(cr, 0, top, osk->width, osk->height - top);
    cairo_fill(cr);
    if (osk->lang == 1 && osk->show_candidates && osk->layer == 0) {
        int bar_y = margin;
        int bar_h = (osk->candidate_height > 0 ? osk->candidate_height : 42) - 8;
        if (bar_h < 26) bar_h = 26;
        int bar_x = margin;
        int bar_w = osk->width - margin * 2;
        rounded_rect(cr, bar_x, bar_y, bar_w, bar_h, radius);
        set_rgba(cr, osk->theme.card_soft, 1.0);
        cairo_fill(cr);

        double fpx = bar_h * 0.38 * scale;
        if (fpx < 14) fpx = 14;
        if (fpx > 22) fpx = 22;
        int x = bar_x + 12;
        int right = bar_x + bar_w - 12;
        if (osk->preedit[0]) {
            int pre_w = (int)(fpx * (strlen(osk->preedit) * 0.65 + 1));
            if (pre_w > bar_w / 3) pre_w = bar_w / 3;
            draw_text(cr, osk->preedit, x, bar_y, pre_w, bar_h, fpx,
                      osk->theme.text2, false);
            x += pre_w + 10;
        }
        if (osk->candidate_count <= 0) {
            draw_text(cr, "\u62fc\u97f3", x, bar_y, right - x, bar_h,
                      fpx * 0.8, osk->theme.text3, false);
        }
        for (int i = 0; i < osk->candidate_count && i < 8; i++) {
            char label[80];
            snprintf(label, sizeof(label), "%d.%s", i + 1,
                     osk->candidates[i].text);
            int cw = (int)(fpx * (strlen(label) / 3.0 + 1.2)) + 18;
            if (x + cw > right) break;
            bool selected = osk->candidate_mode && i == osk->candidate_focus;
            if (selected) {
                rounded_rect(cr, x, bar_y + 4, cw, bar_h - 8, 8);
                set_rgba(cr, osk->theme.accent, 1.0);
                cairo_fill(cr);
            }
            draw_text(cr, label, x + 6, bar_y, cw - 12, bar_h, fpx,
                      selected ? osk_contrast_text(osk->theme.accent)
                               : osk->theme.text,
                      true);
            x += cw + 4;
        }
    }

    for (int i = 0; i < osk->key_count; i++) {
        OskKey *k = &osk->keys[i];
        bool focused = i == osk->focus && !osk->candidate_mode;
        bool pressed = i == osk->pressed;
        double r = radius;
        rounded_rect(cr, k->x, k->y, k->w, k->h, r);
        set_rgba(cr, pressed ? osk->theme.accent
                             : (focused ? osk->theme.elevated : osk->theme.card), 1.0);
        cairo_fill(cr);
        if (focused) {
            rounded_rect(cr, k->x + 1.0, k->y + 1.0, k->w - 2.0, k->h - 2.0, r - 1);
            set_rgba(cr, focus_color(i), 1.0);
            cairo_set_line_width(cr, 2.0);
            cairo_stroke(cr);
        }
        char label[32];
        if (k->action == OSK_KEY_TEXT && osk->shift &&
            k->label[0] >= 'a' && k->label[0] <= 'z') {
            label[0] = (char)(k->label[0] - 'a' + 'A');
            label[1] = 0;
        } else {
            snprintf(label, sizeof(label), "%s", k->label);
        }
        double fs = k->h * 0.34 * scale;
        if (fs < 12) fs = 12;
        if (strlen(label) > 2) fs *= 0.72;
        draw_text(cr, label, k->x + 4, k->y, k->w - 8, k->h, fs,
                  pressed ? osk_contrast_text(osk->theme.accent) : osk->theme.text,
                  true);

        const char *hint = pad_hint(osk->pad_family, k->action);
        if (hint) {
            double bw = 24, bh = 20;
            double bx = k->x + k->w - bw - 4;
            double by = k->y + 4;
            if (bx < k->x + 2) bx = k->x + 2;
            uint32_t bg = k->action == OSK_KEY_SPACE
                              ? osk->theme.accent2 : osk->theme.accent;
            rounded_rect(cr, bx, by, bw, bh, 6);
            set_rgba(cr, bg, 0.92);
            cairo_fill(cr);
            draw_text(cr, hint, bx, by, bw, bh, 13,
                      osk_contrast_text(bg), true);
        }
    }
}
