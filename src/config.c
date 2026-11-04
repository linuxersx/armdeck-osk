// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void osk_config_defaults(OskConfig *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->theme_preset = 0;
    cfg->lock_theme = 0;

    cfg->layout = OSK_LAYOUT_FULL;
    cfg->height_pct = 42;
    cfg->margin = 8;
    cfg->gap = 4;
    cfg->radius = 8;
    cfg->split_gap = 12;

    cfg->mode = OSK_MODE_DOCKED;
    cfg->float_x = 10;
    cfg->float_y = 55;
    cfg->float_w = 80;
    cfg->float_h = 38;

    cfg->default_lang = 0;
    cfg->show_candidates = 1;
    cfg->font_scale = 100;
    cfg->candidate_height = 60;
    cfg->animate = 1;
    cfg->anim_ms = 180;
}

void osk_config_path(char *out, size_t n)
{
    if (!out || !n) return;
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(out, n, "%s/armdeck-osk/config", xdg);
    else if (home && *home)
        snprintf(out, n, "%s/.config/armdeck-osk/config", home);
    else
        snprintf(out, n, "/tmp/armdeck-osk-config");
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

static int parse_bool(const char *v)
{
    return (!strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes") ||
            !strcmp(v, "on")) ? 1 : 0;
}

/* Accept RRGGBB or AARRGGBB. A zero value means "not overridden". */
static uint32_t parse_color(const char *v)
{
    char *end = NULL;
    while (*v == '#') v++;
    unsigned long val = strtoul(v, &end, 16);
    if (end == v || *end) return 0;
    if (strlen(v) <= 6) return 0xFF000000u | (uint32_t)val;
    return (uint32_t)val;
}

void osk_config_load(OskConfig *cfg)
{
    if (!cfg) return;
    osk_config_defaults(cfg);

    char path[700];
    osk_config_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);

        if (!strcmp(key, "theme_preset")) cfg->theme_preset = atoi(val);
        else if (!strcmp(key, "lock_theme")) cfg->lock_theme = parse_bool(val);
        else if (!strcmp(key, "color_bg")) cfg->color_bg = parse_color(val);
        else if (!strcmp(key, "color_surface")) cfg->color_surface = parse_color(val);
        else if (!strcmp(key, "color_card")) cfg->color_card = parse_color(val);
        else if (!strcmp(key, "color_card_soft")) cfg->color_card_soft = parse_color(val);
        else if (!strcmp(key, "color_elevated")) cfg->color_elevated = parse_color(val);
        else if (!strcmp(key, "color_text")) cfg->color_text = parse_color(val);
        else if (!strcmp(key, "color_text2")) cfg->color_text2 = parse_color(val);
        else if (!strcmp(key, "color_text3")) cfg->color_text3 = parse_color(val);
        else if (!strcmp(key, "color_accent")) cfg->color_accent = parse_color(val);
        else if (!strcmp(key, "color_accent2")) cfg->color_accent2 = parse_color(val);
        else if (!strcmp(key, "color_border")) cfg->color_border = parse_color(val);
        else if (!strcmp(key, "color_border_focus")) cfg->color_border_focus = parse_color(val);
        else if (!strcmp(key, "layout")) cfg->layout = atoi(val);
        else if (!strcmp(key, "height_pct")) cfg->height_pct = atoi(val);
        else if (!strcmp(key, "margin")) cfg->margin = atoi(val);
        else if (!strcmp(key, "gap")) cfg->gap = atoi(val);
        else if (!strcmp(key, "radius")) cfg->radius = atoi(val);
        else if (!strcmp(key, "split_gap")) cfg->split_gap = atoi(val);
        else if (!strcmp(key, "mode")) cfg->mode = atoi(val);
        else if (!strcmp(key, "float_x")) cfg->float_x = atoi(val);
        else if (!strcmp(key, "float_y")) cfg->float_y = atoi(val);
        else if (!strcmp(key, "float_w")) cfg->float_w = atoi(val);
        else if (!strcmp(key, "float_h")) cfg->float_h = atoi(val);
        else if (!strcmp(key, "default_lang")) cfg->default_lang = atoi(val);
        else if (!strcmp(key, "show_candidates")) cfg->show_candidates = parse_bool(val);
        else if (!strcmp(key, "font_scale")) cfg->font_scale = atoi(val);
        else if (!strcmp(key, "candidate_height")) cfg->candidate_height = atoi(val);
        else if (!strcmp(key, "animate")) cfg->animate = parse_bool(val);
        else if (!strcmp(key, "anim_ms")) cfg->anim_ms = atoi(val);
    }
    fclose(f);

    if (cfg->theme_preset < 0) cfg->theme_preset = 0;
    if (cfg->layout < 0 || cfg->layout > OSK_LAYOUT_SPLIT) cfg->layout = OSK_LAYOUT_FULL;
    cfg->height_pct = clamp_int(cfg->height_pct, 25, 65);
    cfg->margin = clamp_int(cfg->margin, 2, 32);
    cfg->gap = clamp_int(cfg->gap, 0, 24);
    cfg->radius = clamp_int(cfg->radius, 0, 24);
    cfg->split_gap = clamp_int(cfg->split_gap, 0, 40);
    if (cfg->mode != OSK_MODE_FLOATING) cfg->mode = OSK_MODE_DOCKED;
    cfg->float_x = clamp_int(cfg->float_x, 0, 90);
    cfg->float_y = clamp_int(cfg->float_y, 0, 90);
    cfg->float_w = clamp_int(cfg->float_w, 30, 100);
    cfg->float_h = clamp_int(cfg->float_h, 20, 70);
    if (cfg->float_x + cfg->float_w > 100) cfg->float_w = 100 - cfg->float_x;
    if (cfg->float_y + cfg->float_h > 100) cfg->float_h = 100 - cfg->float_y;
    if (cfg->default_lang != 1) cfg->default_lang = 0;
    cfg->font_scale = clamp_int(cfg->font_scale, 60, 180);
    cfg->candidate_height = clamp_int(cfg->candidate_height, 30, 120);
    cfg->anim_ms = clamp_int(cfg->anim_ms, 0, 400);
}

static void mkdir_parent(const char *path)
{
    char dir[700];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = 0;
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(dir, 0700);
        *p = '/';
    }
    mkdir(dir, 0700);
}

static void write_color(FILE *f, const char *key, uint32_t v)
{
    if (v) fprintf(f, "%s=%08X\n", key, v);
    else fprintf(f, "%s=\n", key);
}

bool osk_config_save(const OskConfig *cfg)
{
    if (!cfg) return false;
    char path[700];
    osk_config_path(path, sizeof(path));
    mkdir_parent(path);

    char tmp[760];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) return false;

    fprintf(f, "# ArmDeck on-screen keyboard configuration\n");
    fprintf(f, "theme_preset=%d\n", cfg->theme_preset);
    fprintf(f, "lock_theme=%d\n", cfg->lock_theme);
    write_color(f, "color_bg", cfg->color_bg);
    write_color(f, "color_surface", cfg->color_surface);
    write_color(f, "color_card", cfg->color_card);
    write_color(f, "color_card_soft", cfg->color_card_soft);
    write_color(f, "color_elevated", cfg->color_elevated);
    write_color(f, "color_text", cfg->color_text);
    write_color(f, "color_text2", cfg->color_text2);
    write_color(f, "color_text3", cfg->color_text3);
    write_color(f, "color_accent", cfg->color_accent);
    write_color(f, "color_accent2", cfg->color_accent2);
    write_color(f, "color_border", cfg->color_border);
    write_color(f, "color_border_focus", cfg->color_border_focus);
    fprintf(f, "\n");
    fprintf(f, "layout=%d\n", cfg->layout);
    fprintf(f, "height_pct=%d\n", cfg->height_pct);
    fprintf(f, "margin=%d\n", cfg->margin);
    fprintf(f, "gap=%d\n", cfg->gap);
    fprintf(f, "radius=%d\n", cfg->radius);
    fprintf(f, "split_gap=%d\n", cfg->split_gap);
    fprintf(f, "\n");
    fprintf(f, "mode=%d\n", cfg->mode);
    fprintf(f, "float_x=%d\n", cfg->float_x);
    fprintf(f, "float_y=%d\n", cfg->float_y);
    fprintf(f, "float_w=%d\n", cfg->float_w);
    fprintf(f, "float_h=%d\n", cfg->float_h);
    fprintf(f, "\n");
    fprintf(f, "default_lang=%d\n", cfg->default_lang);
    fprintf(f, "show_candidates=%d\n", cfg->show_candidates);
    fprintf(f, "font_scale=%d\n", cfg->font_scale);
    fprintf(f, "candidate_height=%d\n", cfg->candidate_height);
    fprintf(f, "animate=%d\n", cfg->animate);
    fprintf(f, "anim_ms=%d\n", cfg->anim_ms);

    bool ok = (fflush(f) == 0);
    if (fclose(f) != 0) ok = false;
    if (!ok) { unlink(tmp); return false; }
    if (rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}
