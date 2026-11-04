// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ARMDECK_OSK_UI_H
#define ARMDECK_OSK_UI_H

#include <stdbool.h>
#include <lvgl.h>
#include "config.h"
#include "theme.h"

typedef struct {
    OskConfig cfg;
    OskPalette palette;
    bool dirty;
    lv_obj_t *focus_items[96];
    int focus_item_row[96];
    int focus_item_col[96];
    int focus_count;
    lv_obj_t *focus;
    int focus_row, focus_col, tab_focus;
    int content_rows;
    int build_row, build_col;
    bool building_content;
    lv_font_t *font_sm;
    lv_font_t *font_md;
    lv_font_t *font_lg;
    lv_font_t *font_xl;
    lv_obj_t *root;
    lv_obj_t *content;
    lv_obj_t *preview;
    lv_obj_t *tabs[5];
    int page;
    bool license_open;
    lv_obj_t *license_back;
} SettingsUi;

void ui_build(SettingsUi *ui);
void ui_rebuild(SettingsUi *ui);
void ui_refresh_palette(SettingsUi *ui);
void ui_mark_dirty(SettingsUi *ui);
void ui_update_preview(SettingsUi *ui);
void ui_init_timer(SettingsUi *ui);
void ui_focus_set(SettingsUi *ui, lv_obj_t *obj);
void ui_focus_dir(SettingsUi *ui, int dx, int dy);
void ui_focus_activate(SettingsUi *ui);
void ui_license_scroll(SettingsUi *ui, int dir);
void ui_send_osk_command(const char *cmd);
const char *ui_config_path(void);

#endif
