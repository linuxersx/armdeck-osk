// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "ui.h"
#include "backend.h"

#include <SDL.h>
#include <fontconfig/fontconfig.h>
#include <lvgl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static SettingsUi g_ui;

static void on_input(void *userdata, InputAction action, const char *text)
{
    (void)userdata;
    (void)text;
    if (g_ui.license_open) {
        if (action == IN_CANCEL || action == IN_PAD_CANCEL || action == IN_B) {
            g_ui.license_open = false;
            g_ui.page = 4;
            ui_rebuild(&g_ui);
        } else if (action == IN_UP || action == IN_LB) {
            ui_license_scroll(&g_ui, -1);
        } else if (action == IN_DOWN || action == IN_RB) {
            ui_license_scroll(&g_ui, 1);
        } else if (action == IN_CONFIRM || action == IN_PAD_CONFIRM ||
                   action == IN_A) {
            ui_focus_activate(&g_ui);
        }
        return;
    }
    switch (action) {
        case IN_UP:
        case IN_LB:
            ui_focus_dir(&g_ui, 0, -1);
            break;
        case IN_DOWN:
        case IN_RB:
            ui_focus_dir(&g_ui, 0, 1);
            break;
        case IN_LEFT:
            ui_focus_dir(&g_ui, -1, 0);
            break;
        case IN_RIGHT:
            ui_focus_dir(&g_ui, 1, 0);
            break;
        case IN_CONFIRM:
        case IN_PAD_CONFIRM:
        case IN_A:
            ui_focus_activate(&g_ui);
            break;
        case IN_CANCEL:
        case IN_PAD_CANCEL:
        case IN_B:
            if (g_ui.page != 0) {
                g_ui.page = 0;
                ui_rebuild(&g_ui);
            }
            break;
        default:
            break;
    }
}

static char *dup_if_readable(const char *path)
{
    if (!path || !*path) return NULL;
    if (access(path, R_OK) != 0) return NULL;
    return strdup(path);
}

static char *find_cjk_font(void)
{
    char *env = dup_if_readable(getenv("ARMDECK_OSK_FONT"));
    if (env) return env;

    static const char *const candidates[] = {
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansSC-Regular.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttf",
        "/usr/share/taiji/assets/fonts/TaijiCJK.ttf",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        char *p = dup_if_readable(candidates[i]);
        if (p) return p;
    }

    FcInit();
    FcPattern *pat = FcPatternCreate();
    if (!pat) return NULL;
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)"sans");
    FcPatternAddString(pat, FC_LANG, (const FcChar8 *)"zh-cn");
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result = FcResultNoMatch;
    FcPattern *match = FcFontMatch(NULL, pat, &result);
    char *path = NULL;
    if (match) {
        FcChar8 *file = NULL;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file)
            path = dup_if_readable((const char *)file);
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);
    return path;
}

static void *read_whole_file(const char *path, size_t *out_size)
{
    *out_size = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    rewind(f);
    void *data = malloc((size_t)n);
    if (!data) { fclose(f); return NULL; }
    size_t got = fread(data, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(data); return NULL; }
    *out_size = got;
    return data;
}

static char g_settings_flag[700];

static void remove_settings_flag(void)
{
    if (g_settings_flag[0]) unlink(g_settings_flag);
}

static void create_settings_flag(void)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/armdeck-osk",
             runtime && *runtime ? runtime : "/tmp");
    mkdir(dir, 0700);
    snprintf(g_settings_flag, sizeof(g_settings_flag), "%s/settings.active", dir);
    int fd = open(g_settings_flag, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        char pid[32];
        int n = snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());
        (void)write(fd, pid, (size_t)n);
        close(fd);
    }
    atexit(remove_settings_flag);
}

static lv_font_t *load_font(const void *data, size_t size, int px)
{
    if (!data || !size) return NULL;
    return lv_tiny_ttf_create_data_ex(data, size, px, LV_FONT_KERNING_NONE, 4096);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) {
#ifndef ARMDECK_OSK_VERSION
#define ARMDECK_OSK_VERSION "1.0.0"
#endif
            printf("armdeck-osk-settings %s\n", ARMDECK_OSK_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("用法: armdeck-osk-settings\n");
            return 0;
        }
    }

    memset(&g_ui, 0, sizeof(g_ui));
    create_settings_flag();
    ui_send_osk_command("hide");
    osk_config_load(&g_ui.cfg);
    const char *start_page = getenv("ARMDECK_OSK_SETTINGS_PAGE");
    if (start_page && *start_page) {
        g_ui.page = atoi(start_page);
        if (g_ui.page < 0 || g_ui.page > 4) g_ui.page = 0;
    }

    Backend backend;
    if (!backend_init(&backend, 1280, 720, on_input, NULL)) {
        fprintf(stderr, "armdeck-osk-settings: SDL init failed\n");
        return 1;
    }
    backend_set_title(&backend, "ArmDeck 输入法设置");
    if (!getenv("ARMDECK_OSK_SETTINGS_WINDOWED")) {
        SDL_SetWindowFullscreen(backend.window, SDL_WINDOW_FULLSCREEN_DESKTOP);
        backend.fullscreen = true;
    }

    char *font_path = find_cjk_font();
    void *font_data = NULL;
    size_t font_size = 0;
    if (font_path) {
        font_data = read_whole_file(font_path, &font_size);
        if (font_data) {
            g_ui.font_sm = load_font(font_data, font_size, 18);
            g_ui.font_md = load_font(font_data, font_size, 22);
            g_ui.font_lg = load_font(font_data, font_size, 30);
            g_ui.font_xl = load_font(font_data, font_size, 38);
            fprintf(stderr, "armdeck-osk-settings: font %s (%zu bytes)\n",
                    font_path, font_size);
        }
    }
    if (!g_ui.font_sm) g_ui.font_sm = (lv_font_t *)&lv_font_montserrat_16;
    if (!g_ui.font_md) g_ui.font_md = (lv_font_t *)&lv_font_montserrat_20;
    if (!g_ui.font_lg) g_ui.font_lg = (lv_font_t *)&lv_font_montserrat_28;
    if (!g_ui.font_xl) g_ui.font_xl = (lv_font_t *)&lv_font_montserrat_32;

    ui_build(&g_ui);

    ui_init_timer(&g_ui);

    while (backend.running) {
        backend_poll(&backend);
        lv_timer_handler();
        backend_present(&backend);
        SDL_Delay(4);
    }

    if (font_path) {
        if (g_ui.font_sm && g_ui.font_sm != &lv_font_montserrat_16)
            lv_tiny_ttf_destroy(g_ui.font_sm);
        if (g_ui.font_md && g_ui.font_md != &lv_font_montserrat_20)
            lv_tiny_ttf_destroy(g_ui.font_md);
        if (g_ui.font_lg && g_ui.font_lg != &lv_font_montserrat_28)
            lv_tiny_ttf_destroy(g_ui.font_lg);
        if (g_ui.font_xl && g_ui.font_xl != &lv_font_montserrat_32)
            lv_tiny_ttf_destroy(g_ui.font_xl);
        free(font_path);
    }
    free(font_data);
    backend_shutdown(&backend);
    return 0;
}
