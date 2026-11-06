// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "osk.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "input-method-unstable-v2-client-protocol.h"

typedef struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_touch *touch;
    struct wl_output *output;
    int output_w;
    int output_h;
    int refresh_hz;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_surface *surface;
    struct wl_buffer *buffers[2];
    void *shm_data[2];
    size_t shm_size[2];
    bool buffer_busy[2];
    int buffer_index;
    int width, height;
    bool configured;
    bool visible;
    bool running;
    int anim_offset;
    int anim_from;
    int anim_to;
    uint32_t anim_start;
    uint32_t anim_duration;
    bool anim_active;
    bool anim_pending;

    struct zwp_input_method_manager_v2 *im_manager;
    struct zwp_input_method_v2 *im;
    bool im_active;
    uint32_t im_serial;
    uint32_t im_cursor;
    uint32_t im_anchor;
    bool im_has_surrounding;
    char im_surrounding[4096];
    int socket_fd;
    struct { int fd; bool attached; } clients[16];
    int client_count;
    Osk osk;
} OskApp;

static OskApp g_app;

#define CANDIDATE_BAR_H 42

static OskConfig g_cfg;
static OskPalette g_taiji_palette;
static bool g_has_taiji_palette;
static time_t g_cfg_mtime;

static uint32_t g_press_ms;
static int g_press_index = -1;

static uint32_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void redraw(OskApp *app);
static void create_buffer(OskApp *app);
static void destroy_buffers(OskApp *app);
static void ensure_surface(OskApp *app);
static void destroy_surface(OskApp *app);
static void update_surface_size(OskApp *app);
static void unmap_surface(OskApp *app);
static void start_animation(OskApp *app, int from, int to, uint32_t duration);
static void anim_tick(OskApp *app);
static void apply_input_region(OskApp *app);
static void reload_config(OskApp *app, bool force);
static void apply_theme_command(OskApp *app, const char *args);
static void create_buffer(OskApp *app);
static void show_keyboard(OskApp *app);
static void hide_keyboard(OskApp *app);

static char g_pid_path[700];
static void osk_runtime_path(const char *suffix, char *out, size_t n);

static void remove_pid_file(void)
{
    if (g_pid_path[0]) unlink(g_pid_path);
}

static void mark_hidden_on_exit(void)
{
    char path[700];
    osk_runtime_path("visible", path, sizeof(path));
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) { (void)write(fd, "0\n", 2); close(fd); }
}

static void osk_runtime_path(const char *suffix, char *out, size_t n)
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

static void write_visible_state(OskApp *app)
{
    char path[600];
    osk_runtime_path("visible", path, sizeof(path));
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        const char *v = app->visible ? "1\n" : "0\n";
        (void)write(fd, v, 2);
        close(fd);
    }
}

static void client_remove(OskApp *app, int idx)
{
    if (idx < 0 || idx >= app->client_count) return;
    close(app->clients[idx].fd);
    app->clients[idx] = app->clients[--app->client_count];
}

static void send_clients(OskApp *app, const char *line)
{
    if (!line || !*line) return;
    size_t len = strlen(line);
    for (int i = app->client_count - 1; i >= 0; i--) {
        if (!app->clients[i].attached) continue;
        int fd = app->clients[i].fd;
        ssize_t n = send(fd, line, len, MSG_NOSIGNAL);
        if (n != (ssize_t)len) client_remove(app, i);
    }
}

static bool im_commit_text(OskApp *app, const char *text)
{
    if (!app->im || !app->im_active || !text || !*text) return false;
    zwp_input_method_v2_commit_string(app->im, text);
    zwp_input_method_v2_commit(app->im, app->im_serial);
    return true;
}

static bool im_delete_backspace(OskApp *app)
{
    if (!app->im || !app->im_active) return false;
    if (app->im_has_surrounding) {
        size_t n = 1;
        size_t len = strlen(app->im_surrounding);
        size_t cursor = app->im_cursor;
        if (cursor > len) cursor = len;
        if (cursor > 0) {
            size_t k = 1;
            while (k < cursor &&
                   ((unsigned char)app->im_surrounding[cursor - k] & 0xC0) == 0x80)
                k++;
            n = k;
        }
        zwp_input_method_v2_delete_surrounding_text(app->im, (uint32_t)n, 0);
    } else {
        /* Terminals like Konsole do not expose surrounding text.  A plain
         * backspace byte is handled by the line editor (readline). */
        zwp_input_method_v2_commit_string(app->im, "\b");
    }
    zwp_input_method_v2_commit(app->im, app->im_serial);
    return true;
}

static void commit_text(OskApp *app, const char *text)
{
    if (!text || !*text) return;
    if (im_commit_text(app, text)) return;
    char line[256];
    snprintf(line, sizeof(line), "TEXT %s\n", text);
    send_clients(app, line);
}

static void update_rime_context(OskApp *app)
{
    if (!app->osk.rime_ready) return;
    char pre[OSK_MAX_TEXT] = "";
    char cand[OSK_MAX_CANDIDATES][64] = {{0}};
    int count = 0, page = 0;
    bool last = true;
    if (osk_rime_context(&app->osk.rime, pre, sizeof(pre), cand,
                         OSK_MAX_CANDIDATES, &count, &page, &last)) {
        snprintf(app->osk.preedit, sizeof(app->osk.preedit), "%s", pre);
        app->osk.candidate_count = count;
        app->osk.candidate_page = page;
        for (int i = 0; i < count; i++)
            snprintf(app->osk.candidates[i].text,
                     sizeof(app->osk.candidates[i].text), "%s", cand[i]);
    } else {
        app->osk.preedit[0] = 0;
        app->osk.candidate_count = 0;
    }
    if (app->osk.candidate_focus >= app->osk.candidate_count)
        app->osk.candidate_focus = 0;
    app->osk.candidate_mode = false;
}

static void commit_rime_result(OskApp *app)
{
    if (!app->osk.rime_ready) return;
    char out[OSK_MAX_TEXT] = "";
    if (osk_rime_commit(&app->osk.rime, out, sizeof(out)))
        commit_text(app, out);
    app->osk.preedit[0] = 0;
    app->osk.candidate_count = 0;
    update_rime_context(app);
}

static void rime_text_key(OskApp *app, const char *text)
{
    if (!app->osk.rime_ready || !text || !*text) return;
    int keysym = (unsigned char)tolower((unsigned char)text[0]);
    osk_rime_key(&app->osk.rime, keysym, app->osk.shift);
    update_rime_context(app);
}

static void toggle_language(OskApp *app)
{
    app->osk.lang = app->osk.lang ? 0 : 1;
    if (app->osk.rime_ready) {
        osk_rime_reset(&app->osk.rime);
        osk_rime_set_chinese(&app->osk.rime, app->osk.lang == 1);
        update_rime_context(app);
    }
    app->osk.preedit[0] = 0;
    app->osk.candidate_count = 0;
    app->osk.candidate_mode = false;
    app->osk.candidate_focus = 0;
    if (app->width > 0) osk_layout(&app->osk, app->width, app->height);
}

static void toggle_symbols(OskApp *app)
{
    app->osk.layer = app->osk.layer ? 0 : 1;
    app->osk.preedit[0] = 0;
    app->osk.candidate_count = 0;
    app->osk.candidate_mode = false;
    if (app->osk.rime_ready) {
        osk_rime_reset(&app->osk.rime);
        osk_rime_set_chinese(&app->osk.rime, app->osk.lang == 1);
        update_rime_context(app);
    }
    osk_layout(&app->osk, app->width, app->height);
    app->osk.focus = 0;
}

static void activate_key(OskApp *app, int index)
{
    if (index < 0 || index >= app->osk.key_count) return;
    OskKey *k = &app->osk.keys[index];
    char text[2] = {0};
    switch (k->action) {
        case OSK_KEY_TEXT:
            if (app->osk.layer == 1) {
                commit_text(app, k->text);
                break;
            }
            text[0] = k->text[0];
            if (app->osk.shift && text[0] >= 'a' && text[0] <= 'z')
                text[0] = (char)(text[0] - 'a' + 'A');
            if (app->osk.lang == 1 && app->osk.rime_ready) {
                if (text[0] >= '1' && text[0] <= '8' &&
                    app->osk.candidate_count > 0) {
                    int idx = text[0] - '1';
                    if (idx < app->osk.candidate_count) {
                        osk_rime_select(&app->osk.rime, idx);
                        commit_rime_result(app);
                    }
                } else {
                    rime_text_key(app, text);
                }
            } else if (app->osk.lang == 1) {
                size_t n = strlen(app->osk.preedit);
                if (n + 1 < sizeof(app->osk.preedit)) {
                    app->osk.preedit[n] = k->text[0];
                    app->osk.preedit[n + 1] = 0;
                }
            } else {
                commit_text(app, text);
            }
            break;
        case OSK_KEY_BACKSPACE:
            if (app->osk.layer == 1) {
                if (!im_delete_backspace(app)) send_clients(app, "BACKSPACE\n");
                break;
            }
            if (app->osk.lang == 1 && app->osk.rime_ready &&
                (app->osk.preedit[0] || app->osk.candidate_count > 0)) {
                osk_rime_backspace(&app->osk.rime);
                update_rime_context(app);
            } else if (app->osk.lang == 1 && app->osk.preedit[0]) {
                size_t n = strlen(app->osk.preedit);
                app->osk.preedit[n - 1] = 0;
            } else {
                if (!im_delete_backspace(app)) send_clients(app, "BACKSPACE\n");
            }
            break;
        case OSK_KEY_SHIFT:
            app->osk.shift = !app->osk.shift;
            osk_layout(&app->osk, app->width, app->height);
            break;
        case OSK_KEY_ENTER:
            if (app->osk.layer == 1) {
                if (!im_commit_text(app, "\n")) send_clients(app, "ENTER\n");
                break;
            }
            if (app->osk.lang == 1 && app->osk.rime_ready &&
                (app->osk.preedit[0] || app->osk.candidate_count > 0)) {
                osk_rime_commit_composition(&app->osk.rime);
                commit_rime_result(app);
            } else if (app->osk.lang == 1 && app->osk.preedit[0]) {
                commit_text(app, app->osk.preedit);
                app->osk.preedit[0] = 0;
            } else {
                if (!im_commit_text(app, "\n")) send_clients(app, "ENTER\n");
            }
            break;
        case OSK_KEY_SPACE:
            if (app->osk.lang == 1 && app->osk.rime_ready &&
                app->osk.candidate_count > 0) {
                osk_rime_select(&app->osk.rime, 0);
                commit_rime_result(app);
            } else {
                commit_text(app, " ");
            }
            break;
        case OSK_KEY_LANG:
            toggle_language(app);
            break;
        case OSK_KEY_SYMBOLS:
            toggle_symbols(app);
            break;
        case OSK_KEY_HIDE:
            send_clients(app, "CANCEL\n");
            hide_keyboard(app);
            return;
    }
    redraw(app);
}

static void handle_command(OskApp *app, const char *cmd)
{
    if (!strcmp(cmd, "show")) { show_keyboard(app); return; }
    else if (!strcmp(cmd, "hide")) { hide_keyboard(app); return; }
    else if (!strcmp(cmd, "reload")) { reload_config(app, true); return; }
    else if (!strcmp(cmd, "clear")) { send_clients(app, "CLEAR\n"); return; }
    else if (!strncmp(cmd, "text ", 5)) {
        commit_text(app, cmd + 5);
        if (app->visible) redraw(app);
        return;
    }
    else if (!strncmp(cmd, "theme ", 6)) { apply_theme_command(app, cmd + 6); return; }
    else if (!strncmp(cmd, "pad ", 4)) {
        const char *p = cmd + 4;
        app->osk.pad_family = !strcmp(p, "nintendo") ? 1 :
                              !strcmp(p, "xbox") ? 2 :
                              !strcmp(p, "playstation") ? 3 : 0;
        if (app->visible) redraw(app);
        return;
    }
    else if (!strcmp(cmd, "cand_next") || !strcmp(cmd, "cand_prev")) {
        if (app->osk.candidate_count > 0) {
            if (!app->osk.candidate_mode) {
                app->osk.candidate_mode = true;
                app->osk.candidate_focus = 0;
            } else if (!strcmp(cmd, "cand_next")) {
                app->osk.candidate_focus =
                    (app->osk.candidate_focus + 1) % app->osk.candidate_count;
            } else {
                app->osk.candidate_focus =
                    (app->osk.candidate_focus + app->osk.candidate_count - 1) %
                    app->osk.candidate_count;
            }
            if (app->visible) redraw(app);
        }
        return;
    }
    else if (!strcmp(cmd, "toggle")) {
        if (app->visible) hide_keyboard(app); else show_keyboard(app);
        return;
    } else if (!strcmp(cmd, "up")) osk_move_focus(&app->osk, 0, -1);
    else if (!strcmp(cmd, "down")) osk_move_focus(&app->osk, 0, 1);
    else if (!strcmp(cmd, "left")) osk_move_focus(&app->osk, -1, 0);
    else if (!strcmp(cmd, "right")) osk_move_focus(&app->osk, 1, 0);
    else if (!strcmp(cmd, "confirm")) {
        if (app->osk.candidate_mode && app->osk.candidate_count > 0 &&
            app->osk.rime_ready) {
            int idx = app->osk.candidate_focus;
            if (idx < 0 || idx >= app->osk.candidate_count) idx = 0;
            osk_rime_select(&app->osk.rime, idx);
            commit_rime_result(app);
            app->osk.candidate_mode = false;
            app->osk.candidate_focus = 0;
            if (app->visible) redraw(app);
            return;
        }
        activate_key(app, app->osk.focus);
        return;
    }
    else if (!strcmp(cmd, "cancel")) {
        send_clients(app, "CANCEL\n");
        hide_keyboard(app);
        return;
    }
    else if (!strcmp(cmd, "backspace")) {
        int i = app->osk.focus;
        for (int k = 0; k < app->osk.key_count; k++)
            if (app->osk.keys[k].action == OSK_KEY_BACKSPACE) { i = k; break; }
        activate_key(app, i);
        return;
    }
    else if (!strcmp(cmd, "space")) commit_text(app, " ");
    else if (!strcmp(cmd, "enter")) {
        int i = app->osk.focus;
        for (int k = 0; k < app->osk.key_count; k++)
            if (app->osk.keys[k].action == OSK_KEY_ENTER) { i = k; break; }
        activate_key(app, i);
        return;
    }
    else if (!strcmp(cmd, "lang")) {
        toggle_language(app);
    }
    else if (!strcmp(cmd, "symbols")) {
        toggle_symbols(app);
    }
    if (app->visible) redraw(app);
}

static void socket_event(OskApp *app)
{
    for (;;) {
        int fd = accept4(app->socket_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        if (app->client_count < (int)(sizeof(app->clients) / sizeof(app->clients[0]))) {
            app->clients[app->client_count].fd = fd;
            app->clients[app->client_count].attached = false;
            app->client_count++;
        } else {
            close(fd);
        }
    }
}

/* Keep every accepted connection in the poll set for its whole lifetime.
 * A client may send attach/show once and then hide/show many times later;
 * dropping the fd after the first read made later show/hide commands never
 * arrive, so the keyboard could not reappear after being collapsed. */
static void client_read(OskApp *app, int idx)
{
    if (idx < 0 || idx >= app->client_count) return;
    int fd = app->clients[idx].fd;
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (n <= 0) {
        client_remove(app, idx);
        return;
    }
    buf[n] = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\r\n", &save); line;
         line = strtok_r(NULL, "\r\n", &save)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;
        if (!strcmp(p, "attach")) {
            app->clients[idx].attached = true;
        } else {
            handle_command(app, p);
        }
    }
}

static void shm_format(void *data, struct wl_shm *shm, uint32_t format)
{
    (void)data; (void)shm; (void)format;
}

static const struct wl_shm_listener shm_listener = { shm_format };

static void output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
                            int32_t pw, int32_t ph, int32_t subpixel,
                            const char *make, const char *model, int32_t transform)
{
    (void)output; (void)x; (void)y; (void)subpixel; (void)make; (void)model; (void)transform;
    OskApp *app = data;
    app->output_w = pw;
    app->output_h = ph;
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh)
{
    (void)output;
    OskApp *app = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        app->output_w = width;
        app->output_h = height;
        if (refresh > 0) app->refresh_hz = (int)((refresh + 500) / 1000);
    }
}

static void output_done(void *data, struct wl_output *output) { (void)data; (void)output; }
static void output_scale(void *data, struct wl_output *output, int32_t factor)
{ (void)data; (void)output; (void)factor; }

static const struct wl_output_listener output_listener = {
    output_geometry, output_mode, output_done, output_scale
};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                            uint32_t serial, uint32_t width, uint32_t height)
{
    OskApp *app = data;
    if (surface != app->layer_surface) return;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    app->width = (int)width;
    app->height = (int)height;
    app->configured = true;
    osk_layout(&app->osk, app->width, app->height);
    apply_input_region(app);
    if (app->visible) {
        if (app->anim_pending) {
            app->anim_pending = false;
            if (g_cfg.animate && g_cfg.anim_ms > 0)
                start_animation(app, app->height, 0, (uint32_t)g_cfg.anim_ms);
            else
                app->anim_offset = 0;
        }
        create_buffer(app);
        redraw(app);
    } else if (app->anim_active) {
        create_buffer(app);
        redraw(app);
    }
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    OskApp *app = data;
    if (surface != app->layer_surface) return;
    app->running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    layer_configure, layer_closed
};


static int cfg_clamp(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void surface_geometry(OskApp *app, int *w, int *h, uint32_t *anchor,
                             int *mx, int *my)
{
    int ow = app->output_w > 0 ? app->output_w : 1280;
    int oh = app->output_h > 0 ? app->output_h : 720;
    if (g_cfg.mode == OSK_MODE_FLOATING) {
        int fw = cfg_clamp(g_cfg.float_w, 30, 100);
        int fh = cfg_clamp(g_cfg.float_h, 20, 70);
        int fx = cfg_clamp(g_cfg.float_x, 0, 90);
        int fy = cfg_clamp(g_cfg.float_y, 0, 90);
        if (fx + fw > 100) fw = 100 - fx;
        if (fy + fh > 100) fh = 100 - fy;
        *w = ow * fw / 100;
        *h = oh * fh / 100;
        *anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                  ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
        *mx = ow * fx / 100;
        *my = oh * fy / 100;
    } else {
        int pct = cfg_clamp(g_cfg.height_pct, 25, 65);
        *w = 0;
        *h = oh * pct / 100;
        if (*h < 120) *h = 120;
        *anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                  ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                  ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        *mx = 0;
        *my = 0;
    }
    if (app->osk.show_candidates) {
        int extra = app->osk.candidate_height > 0 ? app->osk.candidate_height
                                                  : CANDIDATE_BAR_H;
        *h += extra;
        if (*h > oh) *h = oh;
        if (g_cfg.mode == OSK_MODE_FLOATING) {
            *my -= extra;
            if (*my < 0) *my = 0;
        }
    }
}

static void update_surface_size(OskApp *app)
{
    if (!app->layer_surface || !app->surface) return;
    int w = 0, h = 0, mx = 0, my = 0;
    uint32_t anchor = 0;
    surface_geometry(app, &w, &h, &anchor, &mx, &my);
    zwlr_layer_surface_v1_set_size(app->layer_surface, (uint32_t)w, (uint32_t)h);
    zwlr_layer_surface_v1_set_margin(app->layer_surface,
        g_cfg.mode == OSK_MODE_FLOATING ? my : 0,
        0,
        0,
        g_cfg.mode == OSK_MODE_FLOATING ? mx : 0);
    wl_surface_commit(app->surface);
    if (app->display) wl_display_flush(app->display);
}

static void apply_input_region(OskApp *app)
{
    if (!app->surface || !app->compositor) return;
    int top = 0;
    if (app->osk.show_candidates) {
        top = app->osk.candidate_height > 0 ? app->osk.candidate_height
                                            : CANDIDATE_BAR_H;
    }
    if (top < 0 || top >= app->height) top = 0;
    int body_h = app->height - top;
    if (body_h <= 0) return;

    struct wl_region *region = wl_compositor_create_region(app->compositor);
    if (!region) return;
    if (app->osk.layout != OSK_LAYOUT_SPLIT || app->osk.layer == 1) {
        wl_region_add(region, 0, top, app->width, body_h);
    } else {
        int gap = app->width * (g_cfg.split_gap > 0 ? g_cfg.split_gap : 12) / 100;
        int mid = app->width / 2;
        int lr = mid - gap / 2;
        int rl = mid + gap / 2;
        if (lr < 1) lr = 1;
        if (rl > app->width - 1) rl = app->width - 1;
        wl_region_add(region, 0, top, lr, body_h);
        wl_region_add(region, rl, top, app->width - rl, body_h);
    }
    wl_surface_set_input_region(app->surface, region);
    wl_region_destroy(region);
}

static void osk_apply_config(OskApp *app, bool set_language)
{
    app->osk.layout = g_cfg.layout;
    app->osk.margin = g_cfg.margin;
    app->osk.gap = g_cfg.gap;
    app->osk.radius = g_cfg.radius;
    app->osk.split_gap = g_cfg.split_gap;
    app->osk.font_scale = g_cfg.font_scale;
    app->osk.show_candidates = g_cfg.show_candidates;
    app->osk.candidate_height = g_cfg.candidate_height;
    if (set_language) app->osk.lang = g_cfg.default_lang ? 1 : 0;
    const OskPalette *ov = (g_has_taiji_palette && !g_cfg.lock_theme)
                               ? &g_taiji_palette : NULL;
    osk_palette_resolve(&app->osk.theme, &g_cfg, ov);
    if (app->configured) {
        osk_layout(&app->osk, app->width, app->height);
        apply_input_region(app);
    }
}

static time_t config_mtime(void)
{
    char path[700];
    osk_config_path(path, sizeof(path));
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

static void reload_config(OskApp *app, bool force)
{
    time_t m = config_mtime();
    if (!force && m == g_cfg_mtime) return;
    g_cfg_mtime = m;
    OskConfig old = g_cfg;
    osk_config_load(&g_cfg);
    bool geometry = old.mode != g_cfg.mode ||
                    old.height_pct != g_cfg.height_pct ||
                    old.float_x != g_cfg.float_x ||
                    old.float_y != g_cfg.float_y ||
                    old.float_w != g_cfg.float_w ||
                    old.float_h != g_cfg.float_h;
    osk_apply_config(app, !app->visible);
    if (geometry && app->surface) {
        destroy_surface(app);
        ensure_surface(app);
    } else if (app->surface) {
        update_surface_size(app);
        if (app->visible) redraw(app);
    }
}

static uint32_t parse_hex_color(const char *text)
{
    if (!text || !*text) return 0;
    while (*text == '#') text++;
    char *end = NULL;
    unsigned long v = strtoul(text, &end, 16);
    if (end == text) return 0;
    if (strlen(text) <= 6) return 0xFF000000u | (uint32_t)v;
    return (uint32_t)v;
}

/* theme <preset> <bg> <surface> <card> <card_soft> <elevated> <text>
 *       <text2> <text3> <accent> <accent2> <border> <border_focus> <light> */
static void apply_theme_command(OskApp *app, const char *args)
{
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", args ? args : "");
    char *save = NULL;
    char *tok = strtok_r(copy, " \t", &save);
    int n = 0;
    uint32_t colors[12];
    bool any = false;
    if (tok) {
        /* First token is the preset index; it is informational. */
        tok = strtok_r(NULL, " \t", &save);
    }
    while (tok && n < 12) {
        colors[n] = parse_hex_color(tok);
        if (colors[n]) any = true;
        n++;
        tok = strtok_r(NULL, " \t", &save);
    }
    if (!any || n < 12) return;
    g_taiji_palette.bg = colors[0];
    g_taiji_palette.surface = colors[1];
    g_taiji_palette.card = colors[2];
    g_taiji_palette.card_soft = colors[3];
    g_taiji_palette.elevated = colors[4];
    g_taiji_palette.text = colors[5];
    g_taiji_palette.text2 = colors[6];
    g_taiji_palette.text3 = colors[7];
    g_taiji_palette.accent = colors[8];
    g_taiji_palette.accent2 = colors[9];
    g_taiji_palette.border = colors[10];
    g_taiji_palette.border_focus = colors[11];
    g_taiji_palette.light = tok && atoi(tok) ? true : false;
    g_taiji_palette.name = "desktop";
    g_has_taiji_palette = true;
    osk_apply_config(app, false);
    if (app->visible) redraw(app);
}

static void destroy_buffers(OskApp *app)
{
    for (int i = 0; i < 2; i++) {
        if (app->buffers[i]) { wl_buffer_destroy(app->buffers[i]); app->buffers[i] = NULL; }
        if (app->shm_data[i]) {
            munmap(app->shm_data[i], app->shm_size[i]);
            app->shm_data[i] = NULL;
            app->shm_size[i] = 0;
        }
        app->buffer_busy[i] = false;
    }
    app->buffer_index = 0;
}

static void destroy_surface(OskApp *app)
{
    destroy_buffers(app);
    if (app->layer_surface) {
        zwlr_layer_surface_v1_destroy(app->layer_surface);
        app->layer_surface = NULL;
    }
    if (app->surface) {
        wl_surface_destroy(app->surface);
        app->surface = NULL;
    }
    app->configured = false;
    app->width = 0;
    app->height = 0;
}

static void ensure_surface(OskApp *app)
{
    if (app->surface || !app->compositor || !app->layer_shell) return;
    int w = 0, h = 0, mx = 0, my = 0;
    uint32_t anchor = 0;
    surface_geometry(app, &w, &h, &anchor, &mx, &my);
    app->surface = wl_compositor_create_surface(app->compositor);
    app->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        app->layer_shell, app->surface, app->output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        "armdeck-osk");
    zwlr_layer_surface_v1_set_anchor(app->layer_surface, anchor);
    zwlr_layer_surface_v1_set_size(app->layer_surface, (uint32_t)w, (uint32_t)h);
    zwlr_layer_surface_v1_set_margin(app->layer_surface,
        g_cfg.mode == OSK_MODE_FLOATING ? my : 0,
        0,
        g_cfg.mode == OSK_MODE_FLOATING ? 0 : 0,
        g_cfg.mode == OSK_MODE_FLOATING ? mx : 0);
    zwlr_layer_surface_v1_set_exclusive_zone(app->layer_surface, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(app->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(app->layer_surface, &layer_listener, app);
    wl_surface_commit(app->surface);
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    OskApp *app = data;
    for (int i = 0; i < 2; i++)
        if (app->buffers[i] == buffer) app->buffer_busy[i] = false;
}

static const struct wl_buffer_listener buffer_listener = { buffer_release };

static void create_buffer(OskApp *app)
{
    if (app->width <= 0 || app->height <= 0) return;
    int stride = app->width * 4;
    size_t size = (size_t)stride * app->height;
    if (app->shm_data[0] && app->shm_size[0] == size) return;
    destroy_buffers(app);
    for (int i = 0; i < 2; i++) {
        int fd = memfd_create("armdeck-osk", MFD_CLOEXEC);
        if (fd < 0) { destroy_buffers(app); return; }
        if (ftruncate(fd, (off_t)size) != 0) { close(fd); destroy_buffers(app); return; }
        void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) { close(fd); destroy_buffers(app); return; }
        struct wl_shm_pool *pool = wl_shm_create_pool(app->shm, fd, (int32_t)size);
        struct wl_buffer *buffer = wl_shm_pool_create_buffer(
            pool, 0, app->width, app->height, stride, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        if (!buffer) {
            munmap(data, size);
            destroy_buffers(app);
            return;
        }
        app->shm_data[i] = data;
        app->shm_size[i] = size;
        app->buffers[i] = buffer;
        app->buffer_busy[i] = false;
        wl_buffer_add_listener(buffer, &buffer_listener, app);
    }
    app->buffer_index = 0;
}

static int acquire_buffer(OskApp *app)
{
    for (int k = 0; k < 2; k++) {
        int i = (app->buffer_index + k) % 2;
        if (!app->buffer_busy[i]) return i;
    }
    /* Both buffers are still held by the compositor. Reusing the oldest one
     * is still better than dropping the update. */
    return app->buffer_index;
}

static void present_buffer(OskApp *app, int idx)
{
    if (!app->surface || !app->buffers[idx]) return;
    wl_surface_attach(app->surface, app->buffers[idx], 0, 0);
    wl_surface_damage_buffer(app->surface, 0, 0, app->width, app->height);
    wl_surface_commit(app->surface);
    wl_display_flush(app->display);
    app->buffer_busy[idx] = true;
    app->buffer_index = 1 - idx;
}

static void unmap_surface(OskApp *app)
{
    if (!app->surface) return;
    wl_surface_attach(app->surface, NULL, 0, 0);
    wl_surface_commit(app->surface);
    if (app->display) wl_display_flush(app->display);
}

static void start_animation(OskApp *app, int from, int to, uint32_t duration)
{
    app->anim_from = from;
    app->anim_to = to;
    app->anim_offset = from;
    app->anim_start = mono_ms();
    app->anim_duration = duration;
    app->anim_active = duration > 0 && from != to;
    if (!app->anim_active) app->anim_offset = to;
}

static void anim_tick(OskApp *app)
{
    if (!app->anim_active) return;
    uint32_t now = mono_ms();
    uint32_t elapsed = now - app->anim_start;
    float t = app->anim_duration ? (float)elapsed / (float)app->anim_duration : 1.0f;
    if (t > 1.0f) t = 1.0f;
    float e = 1.0f - powf(1.0f - t, 3.0f);
    float v = (float)app->anim_from +
              (float)(app->anim_to - app->anim_from) * e;
    app->anim_offset = (int)(v + 0.5f);
    if (t >= 1.0f) {
        app->anim_active = false;
        app->anim_offset = app->anim_to;
        if (!app->visible) unmap_surface(app);
    }
    if (app->anim_active || app->visible) redraw(app);
}

static void redraw(OskApp *app)
{
    if (!app->surface || !app->buffers[0] || !app->shm_data[0]) return;
    int idx = acquire_buffer(app);
    if (!app->shm_data[idx]) return;
    cairo_surface_t *cs = cairo_image_surface_create_for_data(
        (unsigned char *)app->shm_data[idx], CAIRO_FORMAT_ARGB32,
        app->width, app->height, app->width * 4);
    cairo_t *cr = cairo_create(cs);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    if (app->anim_offset > 0)
        cairo_translate(cr, 0, app->anim_offset);
    osk_draw(&app->osk, cr);
    cairo_destroy(cr);
    cairo_surface_flush(cs);
    cairo_surface_destroy(cs);
    present_buffer(app, idx);
}

static void show_keyboard(OskApp *app)
{
    bool was_visible = app->visible;
    reload_config(app, false);
    ensure_surface(app);
    app->visible = true;
    write_visible_state(app);
    if (!app->surface) return;
    apply_input_region(app);
    if (app->configured) {
        create_buffer(app);
        if (!was_visible) {
            if (g_cfg.animate && g_cfg.anim_ms > 0)
                start_animation(app, app->anim_active ? app->anim_offset
                                                       : app->height,
                                0, (uint32_t)g_cfg.anim_ms);
            else {
                app->anim_active = false;
                app->anim_offset = 0;
            }
        }
        redraw(app);
    } else {
        app->anim_pending = true;
        app->anim_offset = 0;
    }
}

static void hide_keyboard(OskApp *app)
{
    app->visible = false;
    write_visible_state(app);
    if (!app->configured || !app->surface) return;
    struct wl_region *empty = wl_compositor_create_region(app->compositor);
    if (empty) {
        wl_surface_set_input_region(app->surface, empty);
        wl_region_destroy(empty);
    }
    if (g_cfg.animate && g_cfg.anim_ms > 0) {
        start_animation(app, app->anim_active ? app->anim_offset : 0,
                        app->height, (uint32_t)g_cfg.anim_ms);
        redraw(app);
    } else {
        app->anim_active = false;
        app->anim_offset = 0;
        unmap_surface(app);
    }
}

static void release_key(OskApp *app, int idx)
{
    if (idx < 0) { redraw(app); return; }
    if (idx == g_press_index &&
        app->osk.keys[idx].action == OSK_KEY_BACKSPACE &&
        mono_ms() - g_press_ms >= 600) {
        send_clients(app, "CLEAR\n");
        g_press_index = -1;
        redraw(app);
        return;
    }
    g_press_index = -1;
    activate_key(app, idx);
}

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{ (void)data; (void)p; (void)serial; (void)surface; (void)sx; (void)sy; }
static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *surface)
{ (void)data; (void)p; (void)serial; (void)surface; }
static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time,
                           wl_fixed_t sx, wl_fixed_t sy)
{
    (void)p; (void)time;
    OskApp *app = data;
    int idx = osk_hit(&app->osk, wl_fixed_to_int(sx), wl_fixed_to_int(sy));
    if (idx >= 0) app->osk.focus = idx;
    if (app->osk.pressed >= 0 && idx != app->osk.pressed) {
        app->osk.pressed = idx;
        redraw(app);
    }
}
static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state)
{
    (void)p; (void)serial; (void)time;
    OskApp *app = data;
    if (button != BTN_LEFT) return;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        struct wl_pointer *dummy = NULL; (void)dummy;
        /* position is tracked by motion; use focus from last motion */
        app->osk.pressed = app->osk.focus;
        g_press_index = app->osk.focus;
        g_press_ms = mono_ms();
        redraw(app);
    } else {
        int idx = app->osk.pressed;
        app->osk.pressed = -1;
        release_key(app, idx);
    }
}
static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value)
{ (void)data; (void)p; (void)time; (void)axis; (void)value; }
static void pointer_frame(void *data, struct wl_pointer *p) { (void)data; (void)p; }
static void pointer_axis_source(void *data, struct wl_pointer *p, uint32_t source)
{ (void)data; (void)p; (void)source; }
static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis)
{ (void)data; (void)p; (void)time; (void)axis; }
static void pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t discrete)
{ (void)data; (void)p; (void)axis; (void)discrete; }

static const struct wl_pointer_listener pointer_listener = {
    pointer_enter, pointer_leave, pointer_motion, pointer_button,
    pointer_axis, pointer_frame, pointer_axis_source, pointer_axis_stop,
    pointer_axis_discrete
};

static void touch_down(void *data, struct wl_touch *t, uint32_t serial,
                       uint32_t time, struct wl_surface *surface, int32_t id,
                       wl_fixed_t sx, wl_fixed_t sy)
{
    (void)t; (void)serial; (void)time; (void)surface;
    OskApp *app = data;
    int idx = osk_hit(&app->osk, wl_fixed_to_int(sx), wl_fixed_to_int(sy));
    if (idx >= 0) {
        app->osk.focus = idx;
        app->osk.pressed = idx;
        g_press_index = idx;
        g_press_ms = mono_ms();
        redraw(app);
    }
    (void)id;
}
static void touch_up(void *data, struct wl_touch *t, uint32_t serial,
                     uint32_t time, int32_t id)
{
    (void)t; (void)serial; (void)time; (void)id;
    OskApp *app = data;
    int idx = app->osk.pressed;
    app->osk.pressed = -1;
    release_key(app, idx);
}
static void touch_motion(void *data, struct wl_touch *t, uint32_t time,
                         int32_t id, wl_fixed_t sx, wl_fixed_t sy)
{ (void)data; (void)t; (void)time; (void)id; (void)sx; (void)sy; }
static void touch_frame(void *data, struct wl_touch *t) { (void)data; (void)t; }
static void touch_cancel(void *data, struct wl_touch *t)
{
    (void)t; OskApp *app = data; app->osk.pressed = -1; redraw(app);
}
static void touch_shape(void *data, struct wl_touch *t, int32_t id, wl_fixed_t major, wl_fixed_t minor)
{ (void)data; (void)t; (void)id; (void)major; (void)minor; }
static void touch_orientation(void *data, struct wl_touch *t, int32_t id, wl_fixed_t orientation)
{ (void)data; (void)t; (void)id; (void)orientation; }

static const struct wl_touch_listener touch_listener = {
    touch_down, touch_up, touch_motion, touch_frame, touch_cancel,
    touch_shape, touch_orientation
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    OskApp *app = data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !app->pointer) {
        app->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app->pointer, &pointer_listener, app);
    }
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !app->touch) {
        app->touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(app->touch, &touch_listener, app);
    }
}
static void seat_name(void *data, struct wl_seat *seat, const char *name)
{ (void)data; (void)seat; (void)name; }
static const struct wl_seat_listener seat_listener = { seat_capabilities, seat_name };

static void im_activate(void *data, struct zwp_input_method_v2 *im)
{
    (void)im;
    OskApp *app = data;
    app->im_active = true;
    app->im_has_surrounding = false;
    app->im_surrounding[0] = 0;
    app->im_cursor = 0;
    app->im_anchor = 0;
    show_keyboard(app);
}

static void im_deactivate(void *data, struct zwp_input_method_v2 *im)
{
    (void)im;
    OskApp *app = data;
    app->im_active = false;
    hide_keyboard(app);
}

static void im_surrounding_text(void *data, struct zwp_input_method_v2 *im,
                                const char *text, uint32_t cursor,
                                uint32_t anchor)
{
    (void)im;
    OskApp *app = data;
    snprintf(app->im_surrounding, sizeof(app->im_surrounding), "%s",
             text ? text : "");
    app->im_cursor = cursor;
    app->im_anchor = anchor;
    app->im_has_surrounding = true;
}

static void im_text_change_cause(void *data, struct zwp_input_method_v2 *im,
                                 uint32_t cause)
{
    (void)data; (void)im; (void)cause;
}

static void im_content_type(void *data, struct zwp_input_method_v2 *im,
                            uint32_t hint, uint32_t purpose)
{
    (void)data; (void)im; (void)hint; (void)purpose;
}

static void im_done(void *data, struct zwp_input_method_v2 *im)
{
    (void)im;
    OskApp *app = data;
    app->im_serial++;
}

static void im_unavailable(void *data, struct zwp_input_method_v2 *im)
{
    (void)im;
    OskApp *app = data;
    app->im_active = false;
    hide_keyboard(app);
}

static const struct zwp_input_method_v2_listener im_listener = {
    im_activate,
    im_deactivate,
    im_surrounding_text,
    im_text_change_cause,
    im_content_type,
    im_done,
    im_unavailable,
};

static void ensure_input_method(OskApp *app)
{
    if (app->im || !app->im_manager || !app->seat) return;
    app->im = zwp_input_method_manager_v2_get_input_method(app->im_manager,
                                                            app->seat);
    if (app->im)
        zwp_input_method_v2_add_listener(app->im, &im_listener, app);
}

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *interface, uint32_t version)
{
    OskApp *app = data;
    if (!strcmp(interface, wl_compositor_interface.name)) {
        app->compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
                                           version < 4 ? version : 4);
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        app->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
        wl_shm_add_listener(app->shm, &shm_listener, app);
    } else if (!strcmp(interface, wl_seat_interface.name)) {
        app->seat = wl_registry_bind(reg, name, &wl_seat_interface, 5);
        wl_seat_add_listener(app->seat, &seat_listener, app);
        ensure_input_method(app);
    } else if (!strcmp(interface, wl_output_interface.name)) {
        app->output = wl_registry_bind(reg, name, &wl_output_interface, 2);
        wl_output_add_listener(app->output, &output_listener, app);
    } else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
        app->layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface,
                                            version < 2 ? version : 2);
    } else if (!strcmp(interface, zwp_input_method_manager_v2_interface.name)) {
        app->im_manager = wl_registry_bind(reg, name,
                                           &zwp_input_method_manager_v2_interface, 1);
        ensure_input_method(app);
    }
}
static void registry_remove(void *data, struct wl_registry *reg, uint32_t name)
{ (void)data; (void)reg; (void)name; }
static const struct wl_registry_listener registry_listener = {
    registry_global, registry_remove
};

static int create_socket(OskApp *app)
{
    char path[600];
    osk_runtime_path("sock", path, sizeof(path));
    char dir[600];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir(dir, 0700); }
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    if (listen(fd, 8) != 0) { close(fd); return -1; }
    app->socket_fd = fd;
    return 0;
}

int main(void)
{
    OskApp *app = &g_app;
    memset(app, 0, sizeof(*app));
    signal(SIGPIPE, SIG_IGN);
    app->running = true;
    app->osk.focus = 0;
    app->osk.pressed = -1;
    app->output_w = 1280;
    app->output_h = 720;
    osk_config_load(&g_cfg);
    g_cfg_mtime = config_mtime();
    osk_apply_config(app, true);
    if (!getenv("ARMDECK_OSK_DISABLE_RIME"))
        app->osk.rime_ready = osk_rime_init(&app->osk.rime);
    if (getenv("ARMDECK_OSK_RIME_TEST")) {
        fprintf(stderr, "rime_ready=%d\n", app->osk.rime_ready);
        if (app->osk.rime_ready) osk_rime_selftest(&app->osk.rime);
        if (app->osk.rime_ready) osk_rime_fini(&app->osk.rime);
        return 0;
    }

    app->display = wl_display_connect(NULL);
    if (!app->display) { fprintf(stderr, "armdeck-osk: cannot connect Wayland\n"); return 1; }
    app->registry = wl_display_get_registry(app->display);
    wl_registry_add_listener(app->registry, &registry_listener, app);
    wl_display_roundtrip(app->display);
    wl_display_roundtrip(app->display);
    if (!app->compositor || !app->shm || !app->layer_shell) {
        fprintf(stderr, "armdeck-osk: missing compositor/shm/layer-shell\n");
        return 1;
    }
    ensure_surface(app);
    char lock_path[700];
    osk_runtime_path("lock", lock_path, sizeof(lock_path));
    int lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "armdeck-osk: another instance is already running for this display\n");
        close(lock_fd);
        if (app->osk.rime_ready) osk_rime_fini(&app->osk.rime);
        wl_display_disconnect(app->display);
        return 0;
    }
    osk_runtime_path("pid", g_pid_path, sizeof(g_pid_path));
    FILE *pf = fopen(g_pid_path, "w");
    if (pf) { fprintf(pf, "%ld\n", (long)getpid()); fclose(pf); }
    atexit(remove_pid_file);
    atexit(mark_hidden_on_exit);
    app->visible = false;
    write_visible_state(app);
    create_socket(app);
    wl_display_flush(app->display);

    while (app->running) {
        struct pollfd fds[2 + 16];
        int nfds = 0;
        fds[nfds].fd = wl_display_get_fd(app->display);
        fds[nfds].events = POLLIN;
        nfds++;
        if (app->socket_fd >= 0) {
            fds[nfds].fd = app->socket_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }
        int client_base = nfds;
        int nclients = app->client_count;
        for (int i = 0; i < nclients; i++) {
            fds[nfds].fd = app->clients[i].fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }
        int hz = app->refresh_hz > 0 ? app->refresh_hz : 60;
        int timeout = app->anim_active ? (1000 / hz) : 1000;
        if (timeout < 1) timeout = 1;
        int pr = poll(fds, (nfds_t)nfds, timeout);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        reload_config(app, false);
        anim_tick(app);
        if (fds[0].revents & POLLIN) {
            if (wl_display_dispatch(app->display) < 0) break;
        }
        if (app->socket_fd >= 0 && (fds[1].revents & POLLIN)) socket_event(app);
        for (int i = nclients - 1; i >= 0; i--) {
            if (i >= app->client_count) continue;
            if (fds[client_base + i].revents & (POLLIN | POLLHUP | POLLERR))
                client_read(app, i);
        }
        if ((app->visible || app->anim_active) && app->configured) {
            if (!app->buffers[0]) create_buffer(app);
        }
        if (app->display) wl_display_flush(app->display);
    }
    if (app->osk.rime_ready) osk_rime_fini(&app->osk.rime);
    if (app->display) wl_display_disconnect(app->display);
    return 0;
}
