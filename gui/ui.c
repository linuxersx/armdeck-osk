// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "ui.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum {
    F_LAYOUT = 1,
    F_HEIGHT,
    F_MARGIN,
    F_GAP,
    F_RADIUS,
    F_SPLIT_GAP,
    F_MODE,
    F_FLOAT_X,
    F_FLOAT_Y,
    F_FLOAT_W,
    F_FLOAT_H,
    F_LANG,
    F_CANDIDATES,
    F_FONT_SCALE,
    F_CANDIDATE_H,
    F_ANIMATE,
    F_ANIM_MS,
    F_LOCK_THEME,
};

typedef struct {
    SettingsUi *ui;
    int field;
    int value;
    lv_obj_t *value_label;
} Ctx;

static lv_color_t c(uint32_t argb)
{
    return lv_color_hex(argb & 0xFFFFFFu);
}

static const uint32_t FOCUS_COLORS[] = {
    0xFF00E5FF, 0xFF5DD39E, 0xFFFFB84C, 0xFFFF6B9A,
    0xFFB48CFF, 0xFF4D8BFF, 0xFFE8C468, 0xFFFF6B6B,
};
static int g_focus_color;

static uint32_t next_focus_color(void)
{
    int n = (int)(sizeof(FOCUS_COLORS) / sizeof(FOCUS_COLORS[0]));
    return FOCUS_COLORS[g_focus_color++ % n];
}

static const lv_font_t *font_sm(SettingsUi *ui)
{
    return ui->font_sm ? ui->font_sm : &lv_font_montserrat_16;
}
static const lv_font_t *font_md(SettingsUi *ui)
{
    return ui->font_md ? ui->font_md : &lv_font_montserrat_20;
}
static const lv_font_t *font_lg(SettingsUi *ui)
{
    return ui->font_lg ? ui->font_lg : &lv_font_montserrat_28;
}
static const lv_font_t *font_xl(SettingsUi *ui)
{
    return ui->font_xl ? ui->font_xl : &lv_font_montserrat_32;
}

static void ctx_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void style_focus(lv_obj_t *obj, SettingsUi *ui, int accent)
{
    lv_obj_set_style_border_color(obj, c(ui->palette.border_focus),
                                  LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(obj, accent ? 3 : 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(obj, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(obj, c(ui->palette.accent2), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(obj, 2, LV_STATE_FOCUSED);
}

static void group_add(SettingsUi *ui, lv_obj_t *obj)
{
    if (!obj || !ui->building_content) return;
    if (ui->focus_count >= (int)(sizeof(ui->focus_items) / sizeof(ui->focus_items[0])))
        return;
    ui->focus_items[ui->focus_count] = obj;
    ui->focus_item_row[ui->focus_count] = ui->build_row;
    ui->focus_item_col[ui->focus_count] = ui->build_col++;
    ui->focus_count++;
}

static void focus_new_row(SettingsUi *ui)
{
    if (!ui->building_content) return;
    ui->build_row = ui->content_rows++;
    ui->build_col = 0;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *text,
                          const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, c(color), 0);
    return l;
}

static lv_obj_t *mk_panel(SettingsUi *ui, lv_obj_t *parent, lv_opa_t opa)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(p, c(ui->palette.surface), 0);
    lv_obj_set_style_bg_opa(p, opa, 0);
    lv_obj_set_style_border_color(p, c(ui->palette.border), 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_radius(p, 16, 0);
    lv_obj_set_style_pad_all(p, 16, 0);
    return p;
}

static lv_obj_t *mk_button(SettingsUi *ui, lv_obj_t *parent, const char *text,
                           int width, int height, lv_event_cb_t cb, void *data,
                           int accent)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, width, height);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, c(accent ? ui->palette.accent : ui->palette.card), 0);
    lv_obj_set_style_bg_opa(b, accent ? 210 : 255, 0);
    lv_obj_set_style_border_color(b, c(accent ? ui->palette.accent : ui->palette.border), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 6, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    style_focus(b, ui, accent);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font_md(ui), 0);
    lv_obj_set_style_text_color(l, c(accent ? 0xFFFFFF : ui->palette.text), 0);
    lv_obj_center(l);
    if (cb)
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    group_add(ui, b);
    return b;
}

static lv_obj_t *mk_row(SettingsUi *ui, lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    (void)ui;
    return row;
}

static void slider_cb(lv_event_t *e)
{
    Ctx *ctx = lv_event_get_user_data(e);
    SettingsUi *ui = ctx->ui;
    int v = (int)lv_slider_get_value(lv_event_get_target_obj(e));
    switch (ctx->field) {
        case F_HEIGHT: ui->cfg.height_pct = v; break;
        case F_MARGIN: ui->cfg.margin = v; break;
        case F_GAP: ui->cfg.gap = v; break;
        case F_RADIUS: ui->cfg.radius = v; break;
        case F_SPLIT_GAP: ui->cfg.split_gap = v; break;
        case F_FLOAT_X: ui->cfg.float_x = v; break;
        case F_FLOAT_Y: ui->cfg.float_y = v; break;
        case F_FLOAT_W: ui->cfg.float_w = v; break;
        case F_FLOAT_H: ui->cfg.float_h = v; break;
        case F_FONT_SCALE: ui->cfg.font_scale = v; break;
        case F_CANDIDATE_H: ui->cfg.candidate_height = v; break;
        case F_ANIM_MS: ui->cfg.anim_ms = v; break;
    }
    if (ctx->value_label) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", v);
        lv_label_set_text(ctx->value_label, buf);
    }
    ui_mark_dirty(ui);
    if (ui->preview) {
        extern void ui_update_preview(SettingsUi *ui);
        ui_update_preview(ui);
    }
}

static lv_obj_t *mk_slider_row(SettingsUi *ui, lv_obj_t *parent, const char *title,
                               int field, int min, int max, int value,
                               const char *suffix)
{
    focus_new_row(ui);
    lv_obj_t *row = mk_row(ui, parent);
    lv_obj_t *l = mk_label(row, title, font_sm(ui), ui->palette.text2);
    lv_obj_set_width(l, 190);

    lv_obj_t *s = lv_slider_create(row);
    lv_obj_set_flex_grow(s, 1);
    lv_obj_set_height(s, 12);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, c(ui->palette.border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, c(ui->palette.accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, c(ui->palette.accent2), LV_PART_KNOB);
    lv_obj_set_style_border_width(s, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 4, LV_PART_KNOB);
    lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    style_focus(s, ui, 1);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d%s", value, suffix ? suffix : "");
    lv_obj_t *vl = mk_label(row, buf, font_sm(ui), ui->palette.text);
    lv_obj_set_width(vl, 70);
    lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_RIGHT, 0);

    Ctx *ctx = calloc(1, sizeof(*ctx));
    ctx->ui = ui;
    ctx->field = field;
    ctx->value_label = vl;
    lv_obj_add_event_cb(s, slider_cb, LV_EVENT_VALUE_CHANGED, ctx);
    lv_obj_add_event_cb(s, ctx_free, LV_EVENT_DELETE, ctx);
    group_add(ui, s);
    return s;
}

static void switch_cb(lv_event_t *e)
{
    Ctx *ctx = lv_event_get_user_data(e);
    SettingsUi *ui = ctx->ui;
    lv_obj_t *sw = lv_event_get_target_obj(e);
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    if (ctx->field == F_CANDIDATES) ui->cfg.show_candidates = on;
    else if (ctx->field == F_ANIMATE) ui->cfg.animate = on;
    else if (ctx->field == F_LOCK_THEME) ui->cfg.lock_theme = on;
    ui_mark_dirty(ui);
    if (ctx->field == F_LOCK_THEME) {
        ui_refresh_palette(ui);
        ui_rebuild(ui);
    }
}

static lv_obj_t *mk_switch_row(SettingsUi *ui, lv_obj_t *parent, const char *title,
                               int field, int value)
{
    focus_new_row(ui);
    lv_obj_t *row = mk_row(ui, parent);
    mk_label(row, title, font_sm(ui), ui->palette.text);
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 64, 32);
    lv_obj_set_style_bg_color(sw, c(ui->palette.border), 0);
    lv_obj_set_style_bg_color(sw, c(ui->palette.accent), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, c(ui->palette.accent2), LV_PART_KNOB);
    lv_obj_add_flag(sw, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    if (value) lv_obj_add_state(sw, LV_STATE_CHECKED);
    style_focus(sw, ui, 0);
    Ctx *ctx = calloc(1, sizeof(*ctx));
    ctx->ui = ui;
    ctx->field = field;
    lv_obj_add_event_cb(sw, switch_cb, LV_EVENT_VALUE_CHANGED, ctx);
    lv_obj_add_event_cb(sw, ctx_free, LV_EVENT_DELETE, ctx);
    group_add(ui, sw);
    return sw;
}

static void segment_cb(lv_event_t *e)
{
    Ctx *ctx = lv_event_get_user_data(e);
    SettingsUi *ui = ctx->ui;
    switch (ctx->field) {
        case F_LAYOUT: ui->cfg.layout = ctx->value; break;
        case F_MODE: ui->cfg.mode = ctx->value; break;
        case F_LANG: ui->cfg.default_lang = ctx->value; break;
        case F_FONT_SCALE: break;
    }
    ui_mark_dirty(ui);
    ui_rebuild(ui);
}

static lv_obj_t *mk_segment(SettingsUi *ui, lv_obj_t *parent, const char *title,
                            int field, const char *const *labels, int count,
                            int selected)
{
    focus_new_row(ui);
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(box, 0, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_pad_bottom(box, 8, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 6, 0);
    mk_label(box, title, font_sm(ui), ui->palette.text2);

    lv_obj_t *row = lv_obj_create(box);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

    for (int i = 0; i < count; i++) {
        Ctx *ctx = calloc(1, sizeof(*ctx));
        ctx->ui = ui;
        ctx->field = field;
        ctx->value = i;
        lv_obj_t *b = mk_button(ui, row, labels[i], 0, 56, segment_cb, ctx,
                                i == selected);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_add_event_cb(b, ctx_free, LV_EVENT_DELETE, ctx);
    }
    return box;
}

static void page_title(SettingsUi *ui, lv_obj_t *page, const char *title,
                       const char *sub)
{
    mk_label(page, title, font_xl(ui), ui->palette.text);
    if (sub) mk_label(page, sub, font_sm(ui), ui->palette.text3);
}

static lv_obj_t *mk_page(SettingsUi *ui, lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_width(p, lv_pct(100));
    lv_obj_set_height(p, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(p, 0, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 4, 0);
    lv_obj_set_style_pad_row(p, 14, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    return p;
}

/* ── Preview ─────────────────────────────────────────────────────────────── */

static void preview_row(SettingsUi *ui, lv_obj_t *parent, int split)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 3, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

    int half = split ? 5 : 10;
    for (int i = 0; i < half; i++) {
        lv_obj_t *k = lv_obj_create(row);
        lv_obj_remove_flag(k, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_height(k, lv_pct(100));
        lv_obj_set_flex_grow(k, 1);
        lv_obj_set_style_bg_color(k, c(ui->palette.card), 0);
        lv_obj_set_style_bg_opa(k, 255, 0);
        lv_obj_set_style_border_color(k, c(ui->palette.border), 0);
        lv_obj_set_style_border_width(k, 1, 0);
        lv_obj_set_style_radius(k, ui->cfg.radius > 10 ? 6 : ui->cfg.radius, 0);
        lv_obj_set_style_pad_all(k, 0, 0);
    }
    if (split) {
        lv_obj_t *gap = lv_obj_create(row);
        lv_obj_remove_flag(gap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_height(gap, lv_pct(100));
        lv_obj_set_flex_grow(gap, 2);
        lv_obj_set_style_bg_opa(gap, 0, 0);
        lv_obj_set_style_border_width(gap, 0, 0);
        lv_obj_set_style_pad_all(gap, 0, 0);
        for (int i = 0; i < half; i++) {
            lv_obj_t *k = lv_obj_create(row);
            lv_obj_remove_flag(k, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_height(k, lv_pct(100));
            lv_obj_set_flex_grow(k, 1);
            lv_obj_set_style_bg_color(k, c(ui->palette.card), 0);
            lv_obj_set_style_bg_opa(k, 255, 0);
            lv_obj_set_style_border_color(k, c(ui->palette.border), 0);
            lv_obj_set_style_border_width(k, 1, 0);
            lv_obj_set_style_radius(k, ui->cfg.radius > 10 ? 6 : ui->cfg.radius, 0);
            lv_obj_set_style_pad_all(k, 0, 0);
        }
    }
}

void ui_update_preview(SettingsUi *ui)
{
    if (!ui->preview) return;
    lv_obj_clean(ui->preview);
    lv_obj_set_style_bg_color(ui->preview, c(ui->palette.bg), 0);
    lv_obj_set_style_border_color(ui->preview, c(ui->palette.border), 0);
    lv_obj_set_style_pad_all(ui->preview, 10, 0);
    lv_obj_set_style_pad_row(ui->preview, 4, 0);
    lv_obj_set_flex_flow(ui->preview, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui->preview, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    lv_obj_t *bar = lv_obj_create(ui->preview);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(bar, lv_pct(100));
    int bar_h = ui->cfg.candidate_height / 3;
    if (bar_h < 12) bar_h = 12;
    if (bar_h > 36) bar_h = 36;
    lv_obj_set_height(bar, bar_h);
    lv_obj_set_style_bg_color(bar, c(ui->palette.card_soft), 0);
    lv_obj_set_style_bg_opa(bar, 255, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 6, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);

    int split = ui->cfg.layout == OSK_LAYOUT_SPLIT;
    for (int r = 0; r < 5; r++)
        preview_row(ui, ui->preview, split);
}

/* ── Pages ───────────────────────────────────────────────────────────────── */

static void theme_pick_cb(lv_event_t *e)
{
    Ctx *ctx = lv_event_get_user_data(e);
    SettingsUi *ui = ctx->ui;
    ui->cfg.theme_preset = ctx->value;
    ui_refresh_palette(ui);
    ui_mark_dirty(ui);
    ui_rebuild(ui);
}

static void color_pick_cb(lv_event_t *e)
{
    Ctx *ctx = lv_event_get_user_data(e);
    SettingsUi *ui = ctx->ui;
    ui->cfg.color_accent = (uint32_t)ctx->value;
    ui_refresh_palette(ui);
    ui_mark_dirty(ui);
    ui_rebuild(ui);
}

static void clear_overrides_cb(lv_event_t *e)
{
    Ctx *ctx = lv_event_get_user_data(e);
    SettingsUi *ui = ctx->ui;
    ui->cfg.color_bg = ui->cfg.color_surface = ui->cfg.color_card = 0;
    ui->cfg.color_card_soft = ui->cfg.color_elevated = 0;
    ui->cfg.color_text = ui->cfg.color_text2 = ui->cfg.color_text3 = 0;
    ui->cfg.color_accent = ui->cfg.color_accent2 = 0;
    ui->cfg.color_border = ui->cfg.color_border_focus = 0;
    ui_refresh_palette(ui);
    ui_mark_dirty(ui);
    ui_rebuild(ui);
}

static void build_theme_page(SettingsUi *ui, lv_obj_t *page)
{
    page_title(ui, page, "主题外观", "跟随桌面，也可以完全手动指定");
    focus_new_row(ui);
    lv_obj_t *row = lv_obj_create(page);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

    for (int i = 0; i < osk_palette_count(); i++) {
        const OskPalette *p = osk_palette_preset(i);
        Ctx *ctx = calloc(1, sizeof(*ctx));
        ctx->ui = ui;
        ctx->value = i;
        lv_obj_t *b = lv_button_create(row);
        lv_obj_set_height(b, 96);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_bg_color(b, c(p->surface), 0);
        lv_obj_set_style_bg_opa(b, 255, 0);
        lv_obj_set_style_border_color(b, c(i == ui->cfg.theme_preset
                                               ? ui->palette.accent2
                                               : p->border), 0);
        lv_obj_set_style_border_width(b, i == ui->cfg.theme_preset ? 3 : 1, 0);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_pad_all(b, 10, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        style_focus(b, ui, 1);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(b, 4, 0);
        mk_label(b, p->name, font_sm(ui), p->text);
        char strip[96];
        snprintf(strip, sizeof(strip), "%06X  %06X", p->accent & 0xFFFFFF,
                 p->card & 0xFFFFFF);
        mk_label(b, strip, font_sm(ui), p->text3);
        lv_obj_add_event_cb(b, theme_pick_cb, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(b, ctx_free, LV_EVENT_DELETE, ctx);
        group_add(ui, b);
    }

    mk_switch_row(ui, page, "忽略桌面发来的主题（完全使用本页设置）",
                  F_LOCK_THEME, ui->cfg.lock_theme);

    mk_label(page, "强调色覆盖", font_sm(ui), ui->palette.text2);
    focus_new_row(ui);
    lv_obj_t *colors = lv_obj_create(page);
    lv_obj_remove_flag(colors, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(colors, lv_pct(100));
    lv_obj_set_style_bg_opa(colors, 0, 0);
    lv_obj_set_style_border_width(colors, 0, 0);
    lv_obj_set_style_pad_all(colors, 0, 0);
    lv_obj_set_style_pad_column(colors, 10, 0);
    lv_obj_set_flex_flow(colors, LV_FLEX_FLOW_ROW);
    static const uint32_t swatches[] = {
        0xFF4D8BFF, 0xFF00E5FF, 0xFF5DD39E, 0xFFFFB84C,
        0xFFFF6B9A, 0xFFFF6B6B, 0xFFB48CFF, 0xFFE6EDF5,
    };
    for (size_t i = 0; i < sizeof(swatches) / sizeof(swatches[0]); i++) {
        Ctx *ctx = calloc(1, sizeof(*ctx));
        ctx->ui = ui;
        ctx->value = (int)swatches[i];
        lv_obj_t *b = lv_button_create(colors);
        lv_obj_set_size(b, 56, 56);
        lv_obj_set_style_bg_color(b, c(swatches[i]), 0);
        lv_obj_set_style_bg_opa(b, 255, 0);
        lv_obj_set_style_radius(b, 28, 0);
        lv_obj_set_style_border_color(b, c(ui->palette.text), 0);
        lv_obj_set_style_border_width(b, 2, 0);
        style_focus(b, ui, 1);
        lv_obj_add_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_add_event_cb(b, color_pick_cb, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(b, ctx_free, LV_EVENT_DELETE, ctx);
        group_add(ui, b);
    }

    focus_new_row(ui);
    Ctx *clear = calloc(1, sizeof(*clear));
    clear->ui = ui;
    lv_obj_t *b = mk_button(ui, page, "清除所有颜色覆盖", 260, 56,
                            clear_overrides_cb, clear, 0);
    lv_obj_add_event_cb(b, ctx_free, LV_EVENT_DELETE, clear);
}

static void preview_show_cb(lv_event_t *e)
{
    (void)e;
    ui_send_osk_command("reload");
    ui_send_osk_command("show");
}

static void preview_hide_cb(lv_event_t *e)
{
    (void)e;
    ui_send_osk_command("hide");
}

static void build_layout_page(SettingsUi *ui, lv_obj_t *page)
{
    page_title(ui, page, "键盘布局", "全键盘、紧凑和左右分体");
    static const char *const layouts[] = {"全键盘", "紧凑", "分体"};
    mk_segment(ui, page, "布局模式", F_LAYOUT, layouts, 3, ui->cfg.layout);

    mk_slider_row(ui, page, "键盘高度", F_HEIGHT, 25, 65,
                  ui->cfg.height_pct, "%");
    mk_slider_row(ui, page, "边距", F_MARGIN, 2, 32, ui->cfg.margin, " px");
    mk_slider_row(ui, page, "按键间距", F_GAP, 0, 24, ui->cfg.gap, " px");
    mk_slider_row(ui, page, "圆角", F_RADIUS, 0, 24, ui->cfg.radius, " px");
    mk_slider_row(ui, page, "分体中间间隙", F_SPLIT_GAP, 0, 40,
                  ui->cfg.split_gap, "%");
    mk_slider_row(ui, page, "候选栏高度", F_CANDIDATE_H, 30, 120,
                  ui->cfg.candidate_height, " px");

    mk_label(page, "实时预览", font_sm(ui), ui->palette.text2);
    ui->preview = lv_obj_create(page);
    lv_obj_set_width(ui->preview, lv_pct(100));
    lv_obj_set_height(ui->preview, 220);
    lv_obj_set_style_border_width(ui->preview, 1, 0);
    lv_obj_set_style_radius(ui->preview, 12, 0);
    lv_obj_remove_flag(ui->preview, LV_OBJ_FLAG_SCROLLABLE);
    ui_update_preview(ui);

    focus_new_row(ui);
    lv_obj_t *row = mk_row(ui, page);
    Ctx *c1 = calloc(1, sizeof(*c1));
    c1->ui = ui;
    lv_obj_t *b1 = mk_button(ui, row, "在屏幕上显示键盘", 260, 56,
                             preview_show_cb, c1, 1);
    lv_obj_add_event_cb(b1, ctx_free, LV_EVENT_DELETE, c1);
    Ctx *c2 = calloc(1, sizeof(*c2));
    c2->ui = ui;
    lv_obj_t *b2 = mk_button(ui, row, "隐藏键盘", 200, 56,
                             preview_hide_cb, c2, 0);
    lv_obj_add_event_cb(b2, ctx_free, LV_EVENT_DELETE, c2);
}

static void build_float_page(SettingsUi *ui, lv_obj_t *page)
{
    page_title(ui, page, "悬浮与位置", "停靠底部，或者悬浮在任意位置");
    static const char *const modes[] = {"停靠底部", "悬浮"};
    mk_segment(ui, page, "显示模式", F_MODE, modes, 2, ui->cfg.mode);

    mk_slider_row(ui, page, "悬浮水平位置", F_FLOAT_X, 0, 90,
                  ui->cfg.float_x, "%");
    mk_slider_row(ui, page, "悬浮垂直位置", F_FLOAT_Y, 0, 90,
                  ui->cfg.float_y, "%");
    mk_slider_row(ui, page, "悬浮宽度", F_FLOAT_W, 30, 100,
                  ui->cfg.float_w, "%");
    mk_slider_row(ui, page, "悬浮高度", F_FLOAT_H, 20, 70,
                  ui->cfg.float_h, "%");

    mk_label(page, "悬浮位置和大小会保存在配置文件里，下次启动继续生效。",
             font_sm(ui), ui->palette.text3);

    focus_new_row(ui);
    lv_obj_t *row = mk_row(ui, page);
    Ctx *c1 = calloc(1, sizeof(*c1));
    c1->ui = ui;
    lv_obj_t *b1 = mk_button(ui, row, "应用并显示", 220, 56,
                             preview_show_cb, c1, 1);
    lv_obj_add_event_cb(b1, ctx_free, LV_EVENT_DELETE, c1);
}

static void build_input_page(SettingsUi *ui, lv_obj_t *page)
{
    page_title(ui, page, "输入行为", "默认语言与中文候选");
    static const char *const langs[] = {"默认英文", "默认中文"};
    mk_segment(ui, page, "默认输入语言", F_LANG, langs, 2,
               ui->cfg.default_lang);
    mk_switch_row(ui, page, "显示中文候选栏", F_CANDIDATES,
                  ui->cfg.show_candidates);
    mk_switch_row(ui, page, "弹出动画", F_ANIMATE, ui->cfg.animate);
    mk_slider_row(ui, page, "动画时长", F_ANIM_MS, 0, 400,
                  ui->cfg.anim_ms, " ms");
    mk_slider_row(ui, page, "字号缩放", F_FONT_SCALE, 60, 180,
                  ui->cfg.font_scale, "%");
}

static void reload_cb(lv_event_t *e)
{
    SettingsUi *ui = lv_event_get_user_data(e);
    osk_config_load(&ui->cfg);
    ui_refresh_palette(ui);
    ui_rebuild(ui);
}

static void reset_cb(lv_event_t *e)
{
    SettingsUi *ui = lv_event_get_user_data(e);
    osk_config_defaults(&ui->cfg);
    ui_refresh_palette(ui);
    ui_mark_dirty(ui);
    ui_rebuild(ui);
}

static void restart_daemon_cb(lv_event_t *e)
{
    (void)e;
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c",
              "systemctl --user restart armdeck-oskd.service", (char *)NULL);
        _exit(127);
    }
}

static void license_open_cb(lv_event_t *e)
{
    SettingsUi *ui = lv_event_get_user_data(e);
    ui->license_open = true;
    ui->page = 4;
    ui_rebuild(ui);
}

static void license_back_cb(lv_event_t *e)
{
    SettingsUi *ui = lv_event_get_user_data(e);
    ui->license_open = false;
    ui->page = 4;
    ui_rebuild(ui);
}

static const char *license_text(void)
{
    return
        "本项目代码与自有素材以 GPL-3.0-or-later 分发。\n"
        "Copyright (C) 2026 the Taiji Desktop project authors.\n\n"
        "完整许可证文本见：\n"
        "/usr/share/licenses/armdeck-osk/LICENSE\n\n"
        "第三方组件\n"
        "--------------------------------\n"
        "LVGL 9.3：MIT\n"
        "librime：BSD-3-Clause（运行时动态加载）\n"
        "SDL2：zlib\n"
        "libpng：libpng/zlib\n"
        "Fontconfig：MIT\n"
        "Wayland 与协议代码：MIT\n"
        "ZCOOL QingKe HuangYou 字体：SIL OFL 1.1\n\n"
        "ArmDeck OSK 是独立程序，通过 Wayland input-method-v2\n"
        "或本地 socket 与太极通信，不链接太极的专有代码。\n\n"
        "平台名称与图标属于各自厂商的商标，仅用于识别模拟平台。\n"
        "本项目与任何主机厂商无关联。\n\n"
        "uMTP-Responder 的完整对应源码随太极项目提供。\n";
}

static void build_license_page(SettingsUi *ui, lv_obj_t *page)
{
    page_title(ui, page, "开源许可", "GPL-3.0-or-later 与第三方组件");
    lv_obj_t *head = mk_label(page, "本项目以 GPL-3.0-or-later 分发",
                              font_md(ui), ui->palette.accent2);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_t *body = mk_label(page, license_text(), font_sm(ui),
                              ui->palette.text2);
    lv_obj_set_width(body, lv_pct(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(body, 4, 0);

    focus_new_row(ui);
    lv_obj_t *row = mk_row(ui, page);
    ui->license_back = mk_button(ui, row, "返回", 180, 56,
                                 license_back_cb, ui, 1);
}

static void build_about_page(SettingsUi *ui, lv_obj_t *page)
{
    page_title(ui, page, "关于", "ArmDeck 输入法设置");
#ifndef ARMDECK_OSK_VERSION
#define ARMDECK_OSK_VERSION "1.0.0"
#endif
    mk_label(page, "版本：" ARMDECK_OSK_VERSION, font_lg(ui),
             ui->palette.accent2);
    mk_label(page, "许可证：GPL-3.0-or-later", font_md(ui),
             ui->palette.text2);
    mk_label(page, "配置文件：", font_sm(ui), ui->palette.text2);
    mk_label(page, ui_config_path(), font_sm(ui), ui->palette.text);
    mk_label(page, "命令行：armdeck-osk-settings", font_sm(ui),
             ui->palette.text3);
    mk_label(page, "守护进程：armdeck-oskd", font_sm(ui), ui->palette.text3);

    focus_new_row(ui);
    lv_obj_t *row = mk_row(ui, page);
    lv_obj_t *b1 = mk_button(ui, row, "重新加载配置", 200, 56, reload_cb, ui, 0);
    lv_obj_t *b2 = mk_button(ui, row, "恢复默认", 180, 56, reset_cb, ui, 0);
    lv_obj_t *b3 = mk_button(ui, row, "重启输入法守护进程", 260, 56,
                             restart_daemon_cb, ui, 1);
    focus_new_row(ui);
    lv_obj_t *lic_row = mk_row(ui, page);
    lv_obj_t *b4 = mk_button(ui, lic_row, "开源许可", 200, 56,
                             license_open_cb, ui, 0);
    (void)b1; (void)b2; (void)b3; (void)b4;
}

/* ── Shell ───────────────────────────────────────────────────────────────── */

static void tab_cb(lv_event_t *e)
{
    SettingsUi *ui = lv_event_get_user_data(e);
    /* The tab index is stored in the object's user data. */
    lv_obj_t *btn = lv_event_get_target_obj(e);
    int page = (int)(intptr_t)lv_obj_get_user_data(btn);
    ui->page = page;
    ui_rebuild(ui);
}

static void build_shell(SettingsUi *ui)
{
    ui->focus_count = 0;
    ui->focus = NULL;
    ui->content_rows = 0;
    ui->build_row = -1;
    ui->build_col = 0;
    ui->building_content = false;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, c(ui->palette.bg), 0);
    lv_obj_set_style_bg_opa(scr, 255, 0);
    lv_obj_set_style_text_font(scr, font_md(ui), 0);
    lv_obj_set_style_text_color(scr, c(ui->palette.text), 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 22, 0);
    lv_obj_set_style_pad_row(scr, 12, 0);

    ui->root = lv_obj_create(scr);
    lv_obj_remove_flag(ui->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(ui->root, lv_pct(100));
    lv_obj_set_flex_grow(ui->root, 1);
    lv_obj_set_style_bg_opa(ui->root, 0, 0);
    lv_obj_set_style_border_width(ui->root, 0, 0);
    lv_obj_set_style_pad_all(ui->root, 0, 0);
    lv_obj_set_style_pad_column(ui->root, 18, 0);
    lv_obj_set_flex_flow(ui->root, LV_FLEX_FLOW_ROW);

    lv_obj_t *side = mk_panel(ui, ui->root, 235);
    lv_obj_set_width(side, 235);
    lv_obj_set_height(side, lv_pct(100));
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(side, 8, 0);
    mk_label(side, "ArmDeck", font_lg(ui), ui->palette.text);
    mk_label(side, "输入法设置", font_sm(ui), ui->palette.text3);
    lv_obj_t *sp = lv_obj_create(side);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(sp, lv_pct(100));
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_set_style_bg_opa(sp, 0, 0);
    lv_obj_set_style_border_width(sp, 0, 0);

    static const char *const tabs[] = {
        "主题外观", "键盘布局", "悬浮与位置", "输入行为", "关于",
    };
    for (int i = 0; i < 5; i++) {
        lv_obj_t *b = mk_button(ui, side, tabs[i], 0, 54, tab_cb, ui,
                                i == ui->page ? 1 : 0);
        lv_obj_set_width(b, lv_pct(100));
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        ui->tabs[i] = b;
    }

    lv_obj_t *content_wrap = mk_panel(ui, ui->root, 255);
    lv_obj_set_flex_grow(content_wrap, 1);
    lv_obj_set_height(content_wrap, lv_pct(100));
    lv_obj_set_style_pad_all(content_wrap, 18, 0);
    lv_obj_add_flag(content_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(content_wrap, LV_DIR_VER);
    lv_obj_set_flex_flow(content_wrap, LV_FLEX_FLOW_COLUMN);
    ui->content = lv_obj_create(content_wrap);
    lv_obj_set_width(ui->content, lv_pct(100));
    lv_obj_set_height(ui->content, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(ui->content, 0, 0);
    lv_obj_set_style_border_width(ui->content, 0, 0);
    lv_obj_set_style_pad_all(ui->content, 0, 0);
    lv_obj_set_flex_flow(ui->content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ui->content, 14, 0);

    lv_obj_t *page = mk_page(ui, ui->content);
    ui->preview = NULL;
    ui->building_content = true;
    if (ui->license_open) {
        build_license_page(ui, page);
    } else {
        switch (ui->page) {
            case 0: build_theme_page(ui, page); break;
            case 1: build_layout_page(ui, page); break;
            case 2: build_float_page(ui, page); break;
            case 3: build_input_page(ui, page); break;
            default: build_about_page(ui, page); break;
        }
    }
    ui->building_content = false;

    lv_obj_t *footer = lv_label_create(scr);
    lv_label_set_text(footer, ui->license_open ?
        "方向键 滚动   |   A/Enter 返回   |   B/Esc 返回" :
        "方向键 选择   |   A/Enter 确认   |   B/Esc 返回   |   配置实时生效");
    lv_obj_set_style_text_font(footer, font_sm(ui), 0);
    lv_obj_set_style_text_color(footer, c(ui->palette.text3), 0);

    if (ui->license_open) {
        if (ui->license_back) ui_focus_set(ui, ui->license_back);
    } else if (ui->page >= 0 && ui->page < 5 && ui->tabs[ui->page]) {
        ui_focus_set(ui, ui->tabs[ui->page]);
    }
}

void ui_refresh_palette(SettingsUi *ui)
{
    osk_palette_resolve(&ui->palette, &ui->cfg, NULL);
}

void ui_build(SettingsUi *ui)
{
    ui_refresh_palette(ui);
    build_shell(ui);
}

void ui_rebuild(SettingsUi *ui)
{
    ui_refresh_palette(ui);
    build_shell(ui);
}

void ui_mark_dirty(SettingsUi *ui)
{
    ui->dirty = true;
}

static void dirty_timer_cb(lv_timer_t *t)
{
    SettingsUi *ui = lv_timer_get_user_data(t);
    if (!ui->dirty) return;
    ui->dirty = false;
    osk_config_save(&ui->cfg);
    ui_send_osk_command("reload");
}


/* ── Focus navigation ─────────────────────────────────────────────────────── */

static bool obj_is_tab(SettingsUi *ui, lv_obj_t *obj)
{
    for (int i = 0; i < 5; i++)
        if (ui->tabs[i] == obj) return true;
    return false;
}

static int tab_index(SettingsUi *ui, lv_obj_t *obj)
{
    for (int i = 0; i < 5; i++)
        if (ui->tabs[i] == obj) return i;
    return -1;
}

static lv_obj_t *item_at(SettingsUi *ui, int row, int col)
{
    for (int i = 0; i < ui->focus_count; i++)
        if (ui->focus_item_row[i] == row && ui->focus_item_col[i] == col)
            return ui->focus_items[i];
    return NULL;
}

static int row_max_col(SettingsUi *ui, int row)
{
    int maxc = -1;
    for (int i = 0; i < ui->focus_count; i++)
        if (ui->focus_item_row[i] == row && ui->focus_item_col[i] > maxc)
            maxc = ui->focus_item_col[i];
    return maxc;
}

void ui_focus_set(SettingsUi *ui, lv_obj_t *obj)
{
    if (!ui || !obj) return;
    if (ui->focus == obj) return;
    if (ui->focus) {
        lv_obj_remove_state(ui->focus, LV_STATE_FOCUSED);
        lv_obj_invalidate(ui->focus);
    }
    ui->focus = obj;
    uint32_t fc = next_focus_color();
    lv_obj_set_style_border_color(obj, lv_color_hex(fc), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(obj, lv_color_hex(fc), LV_STATE_FOCUSED);
    lv_obj_add_state(obj, LV_STATE_FOCUSED);
    lv_obj_invalidate(obj);
    lv_obj_scroll_to_view_recursive(obj, LV_ANIM_OFF);

    int ti = tab_index(ui, obj);
    if (ti >= 0) {
        ui->tab_focus = ti;
        return;
    }
    for (int i = 0; i < ui->focus_count; i++) {
        if (ui->focus_items[i] == obj) {
            ui->focus_row = ui->focus_item_row[i];
            ui->focus_col = ui->focus_item_col[i];
            return;
        }
    }
}

void ui_focus_dir(SettingsUi *ui, int dx, int dy)
{
    if (!ui || ui->focus_count <= 0) return;
    if (!ui->focus) {
        if (ui->tabs[ui->page]) ui_focus_set(ui, ui->tabs[ui->page]);
        return;
    }

    /* Left/right on a slider changes its value instead of moving focus. */
    if (dx && lv_obj_check_type(ui->focus, &lv_slider_class)) {
        int32_t v = lv_slider_get_value(ui->focus);
        int32_t min = lv_slider_get_min_value(ui->focus);
        int32_t max = lv_slider_get_max_value(ui->focus);
        int32_t step = (max - min) / 20;
        if (step < 1) step = 1;
        v += dx > 0 ? step : -step;
        if (v < min) v = min;
        if (v > max) v = max;
        if (v != lv_slider_get_value(ui->focus)) {
            lv_slider_set_value(ui->focus, v, LV_ANIM_OFF);
            lv_obj_send_event(ui->focus, LV_EVENT_VALUE_CHANGED, NULL);
        }
        return;
    }

    if (obj_is_tab(ui, ui->focus)) {
        if (dy) {
            int t = ui->tab_focus + dy;
            if (t < 0) t = 0;
            if (t > 4) t = 4;
            ui_focus_set(ui, ui->tabs[t]);
        } else if (dx > 0) {
            lv_obj_t *o = item_at(ui, 0, 0);
            if (o) ui_focus_set(ui, o);
        }
        return;
    }

    if (dx < 0) {
        if (ui->focus_col > 0) {
            lv_obj_t *o = item_at(ui, ui->focus_row, ui->focus_col - 1);
            if (o) ui_focus_set(ui, o);
        } else if (ui->tabs[ui->page]) {
            ui_focus_set(ui, ui->tabs[ui->page]);
        }
        return;
    }
    if (dx > 0) {
        lv_obj_t *o = item_at(ui, ui->focus_row, ui->focus_col + 1);
        if (o) ui_focus_set(ui, o);
        return;
    }
    if (dy) {
        int nr = ui->focus_row + dy;
        while (nr >= 0 && nr < ui->content_rows && row_max_col(ui, nr) < 0)
            nr += dy > 0 ? 1 : -1;
        if (nr < 0 || nr >= ui->content_rows) return;
        int maxc = row_max_col(ui, nr);
        int nc = ui->focus_col;
        if (nc > maxc) nc = maxc;
        if (nc < 0) nc = 0;
        lv_obj_t *o = item_at(ui, nr, nc);
        if (o) ui_focus_set(ui, o);
    }
}

void ui_focus_activate(SettingsUi *ui)
{
    if (!ui || !ui->focus) return;
    lv_obj_t *o = ui->focus;
    if (obj_is_tab(ui, o)) {
        lv_obj_send_event(o, LV_EVENT_CLICKED, NULL);
        return;
    }
    if (lv_obj_check_type(o, &lv_switch_class)) {
        if (lv_obj_has_state(o, LV_STATE_CHECKED))
            lv_obj_remove_state(o, LV_STATE_CHECKED);
        else
            lv_obj_add_state(o, LV_STATE_CHECKED);
        lv_obj_send_event(o, LV_EVENT_VALUE_CHANGED, NULL);
        return;
    }
    if (lv_obj_check_type(o, &lv_slider_class)) return;
    lv_obj_send_event(o, LV_EVENT_CLICKED, NULL);
}

/* ── Socket client ───────────────────────────────────────────────────────── */

static void runtime_path(const char *suffix, char *out, size_t n)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *sid = getenv("XDG_SESSION_ID");
    const char *wd = getenv("WAYLAND_DISPLAY");
    char safe[96] = "default";
    if (wd && *wd) {
        size_t j = 0;
        if (sid && *sid) {
            for (const char *p = sid; *p && j + 1 < sizeof(safe); p++)
                safe[j++] = (*p == '/' || *p == ' ') ? '_' : *p;
            if (j + 1 < sizeof(safe)) safe[j++] = '-';
        }
        for (const char *p = wd; *p && j + 1 < sizeof(safe); p++)
            safe[j++] = (*p == '/' || *p == ' ') ? '_' : *p;
        safe[j] = 0;
    }
    snprintf(out, n, "%s/armdeck-osk/osk-%s.%s",
             runtime && *runtime ? runtime : "/tmp", safe, suffix);
}

static int connect_socket(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int find_socket(char *out, size_t n)
{
    runtime_path("sock", out, n);
    int fd = connect_socket(out);
    if (fd >= 0) return fd;

    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/armdeck-osk",
             runtime && *runtime ? runtime : "/tmp");
    /* Fall back to the most recent socket in the runtime directory.  This
     * makes the GUI usable when launched outside the desktop session. */
    char best[600] = "";
    time_t best_mtime = 0;
    for (int i = 0; i < 16; i++) {
        char path[700];
        snprintf(path, sizeof(path), "%s/osk-%d.sock", dir, i);
        struct stat st;
        if (stat(path, &st) == 0 && st.st_mtime >= best_mtime) {
            snprintf(best, sizeof(best), "%s", path);
            best_mtime = st.st_mtime;
        }
    }
    if (best[0]) {
        fd = connect_socket(best);
        if (fd >= 0) {
            snprintf(out, n, "%s", best);
            return fd;
        }
    }
    return -1;
}

void ui_send_osk_command(const char *cmd)
{
    if (!cmd || !*cmd) return;
    char path[700];
    int fd = find_socket(path, sizeof(path));
    if (fd < 0) return;
    char line[256];
    snprintf(line, sizeof(line), "%s\n", cmd);
    (void)send(fd, line, strlen(line), MSG_NOSIGNAL);
    close(fd);
}

const char *ui_config_path(void)
{
    static char path[700];
    osk_config_path(path, sizeof(path));
    return path;
}

void ui_init_timer(SettingsUi *ui)
{
    lv_timer_create(dirty_timer_cb, 250, ui);
}

void ui_license_scroll(SettingsUi *ui, int dir)
{
    if (!ui || !ui->content) return;
    lv_obj_t *wrap = lv_obj_get_parent(ui->content);
    if (!wrap) return;
    lv_obj_scroll_by(wrap, 0, dir > 0 ? 48 : -48, LV_ANIM_ON);
}
