// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEVICES 16
#define LONG_BITS (sizeof(unsigned long) * 8)
#define DELETE_LONG_MS 600
#define STICK_DEADZONE 12000
#define REPEAT_FIRST_MS 260
#define REPEAT_MS 110

enum {
    FAMILY_GENERIC = 0,
    FAMILY_NINTENDO,
    FAMILY_XBOX,
    FAMILY_PLAYSTATION,
};

typedef struct {
    int fd;
    char path[64];
    bool grabbed;
    int family;
    int rt_threshold;
} Device;

static Device g_dev[MAX_DEVICES];
static int g_dev_count;
static bool g_guide_down, g_select_down, g_toggle_latched;
static bool g_settings_active;
static bool g_delete_down;
static uint32_t g_delete_at;
static bool g_rt_down;
static int g_hat_x, g_hat_y;
static int g_stick_x, g_stick_y;
static int g_axis_dir;
static uint32_t g_axis_next;
static int g_reported_family = -1;
static uint32_t g_pad_next;
static volatile sig_atomic_t g_stop;

enum { DIR_NONE = 0, DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static bool bit_set(const unsigned long *bits, int code)
{
    return ((bits[code / LONG_BITS] >> (code % LONG_BITS)) & 1UL) != 0;
}

static int detect_family(int fd)
{
    char name[256] = {0};
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) return FAMILY_GENERIC;
    if (strcasestr(name, "nintendo") || strcasestr(name, "switch") ||
        strcasestr(name, "joy-con") || strcasestr(name, "joycon") ||
        strcasestr(name, "pro controller"))
        return FAMILY_NINTENDO;
    if (strcasestr(name, "xbox") || strcasestr(name, "x-box") ||
        strcasestr(name, "xinput") || strcasestr(name, "microsoft"))
        return FAMILY_XBOX;
    if (strcasestr(name, "playstation") || strcasestr(name, "dualshock") ||
        strcasestr(name, "dualsense") || strcasestr(name, "sony") ||
        strcasestr(name, "ps3") || strcasestr(name, "ps4") ||
        strcasestr(name, "ps5") || strcasestr(name, "wireless controller"))
        return FAMILY_PLAYSTATION;
    return FAMILY_GENERIC;
}

static bool is_gamepad(int fd)
{
    unsigned long evbit[(EV_MAX + LONG_BITS - 1) / LONG_BITS] = {0};
    unsigned long keybit[(KEY_MAX + LONG_BITS - 1) / LONG_BITS] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0) return false;
    if (!bit_set(evbit, EV_KEY)) return false;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) < 0) return false;
    return bit_set(keybit, BTN_SOUTH) || bit_set(keybit, BTN_DPAD_UP) ||
           bit_set(keybit, BTN_GAMEPAD);
}

static void scan(void)
{
    for (int i = 0; i < 32 && g_dev_count < MAX_DEVICES; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        bool exists = false;
        for (int j = 0; j < g_dev_count; j++)
            if (!strcmp(g_dev[j].path, path)) exists = true;
        if (exists) continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (!is_gamepad(fd)) { close(fd); continue; }
        g_dev[g_dev_count].fd = fd;
        snprintf(g_dev[g_dev_count].path, sizeof(g_dev[g_dev_count].path), "%s", path);
        g_dev[g_dev_count].grabbed = false;
        g_dev[g_dev_count].family = detect_family(fd);
        g_dev[g_dev_count].rt_threshold = 12000;
        struct input_absinfo ai;
        if (ioctl(fd, EVIOCGABS(ABS_RZ), &ai) == 0 && ai.maximum > 0)
            g_dev[g_dev_count].rt_threshold = ai.maximum / 2;
        g_dev_count++;
    }
}

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

static bool settings_active(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    long pid = 0;
    if (fscanf(f, "%ld", &pid) != 1) pid = 0;
    fclose(f);
    if (pid > 1 && kill((pid_t)pid, 0) != 0 && errno == ESRCH) {
        unlink(path);
        return false;
    }
    return true;
}

static bool keyboard_visible(void)
{
    char path[600];
    runtime_path("visible", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char c = 0;
    if (fread(&c, 1, 1, f) != 1) c = 0;
    fclose(f);
    return c == '1';
}

static void log_cmd(const char *cmd)
{
    const char *path = getenv("ARMDECK_OSK_LOG");
    if (!path || !*path) return;
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%u %s\n", now_ms(), cmd);
    fclose(f);
}

static void send_cmd(const char *cmd)
{
    char path[600];
    runtime_path("sock", path, sizeof(path));
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    log_cmd(cmd);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        char line[64];
        snprintf(line, sizeof(line), "%s\n", cmd);
        (void)send(fd, line, strlen(line), MSG_NOSIGNAL);
    }
    close(fd);
}

static void remove_device(int idx)
{
    if (idx < 0 || idx >= g_dev_count) return;
    if (g_dev[idx].fd >= 0) close(g_dev[idx].fd);
    g_dev[idx] = g_dev[--g_dev_count];
}

static void grab_all(bool grab)
{
    for (int i = 0; i < g_dev_count; i++) {
        if (g_dev[i].fd < 0) continue;
        int mode = grab ? 1 : 0;
        if (ioctl(g_dev[i].fd, EVIOCGRAB, mode) == 0)
            g_dev[i].grabbed = grab;
        else if (!grab)
            g_dev[i].grabbed = false;
    }
}

static int device_index_for_fd(int fd)
{
    for (int i = 0; i < g_dev_count; i++)
        if (g_dev[i].fd == fd) return i;
    return -1;
}

/* Face buttons are reported by label on Nintendo controllers and by label on
 * Xbox controllers too, so physical positions need a per-family table.
 *
 * Nintendo: A=right, B=bottom, X=top, Y=left
 * Xbox:     A=bottom, B=right, X=left, Y=top
 * PS:       cross=bottom, circle=right, square=left, triangle=top
 */
static const char *face_cmd(int code, int family)
{
    if (family == FAMILY_NINTENDO) {
        switch (code) {
            case BTN_SOUTH: return "confirm";   /* physical A, right */
            case BTN_EAST:  return "cancel";    /* physical B, bottom */
            case BTN_WEST:  return "backspace"; /* physical Y, left */
            case BTN_NORTH: return "space";     /* physical X, top */
        }
    } else if (family == FAMILY_XBOX) {
        switch (code) {
            case BTN_SOUTH: return "cancel";    /* physical A, bottom */
            case BTN_EAST:  return "confirm";   /* physical B, right */
            case BTN_NORTH: return "backspace"; /* physical X, left */
            case BTN_WEST:  return "space";     /* physical Y, top */
        }
    } else {
        switch (code) {
            case BTN_SOUTH: return "cancel";
            case BTN_EAST:  return "confirm";
            case BTN_WEST:  return "backspace";
            case BTN_NORTH: return "space";
        }
    }
    return NULL;
}

static int delete_code_for_family(int family)
{
    return family == FAMILY_XBOX ? BTN_NORTH : BTN_WEST;
}

static int axis_direction(void)
{
    if (g_hat_x < 0) return DIR_LEFT;
    if (g_hat_x > 0) return DIR_RIGHT;
    if (g_hat_y < 0) return DIR_UP;
    if (g_hat_y > 0) return DIR_DOWN;

    int ax = g_stick_x < 0 ? -g_stick_x : g_stick_x;
    int ay = g_stick_y < 0 ? -g_stick_y : g_stick_y;
    if (ax > STICK_DEADZONE && ax >= ay)
        return g_stick_x < 0 ? DIR_LEFT : DIR_RIGHT;
    if (ay > STICK_DEADZONE)
        return g_stick_y < 0 ? DIR_UP : DIR_DOWN;
    return DIR_NONE;
}

static const char *dir_cmd(int dir)
{
    switch (dir) {
        case DIR_UP: return "up";
        case DIR_DOWN: return "down";
        case DIR_LEFT: return "left";
        case DIR_RIGHT: return "right";
        default: return NULL;
    }
}

static int current_family(void)
{
    for (int i = 0; i < g_dev_count; i++)
        if (g_dev[i].family != FAMILY_GENERIC) return g_dev[i].family;
    return FAMILY_GENERIC;
}

static const char *family_name(int family)
{
    switch (family) {
        case FAMILY_NINTENDO: return "nintendo";
        case FAMILY_XBOX: return "xbox";
        case FAMILY_PLAYSTATION: return "playstation";
        default: return "none";
    }
}

static void report_family(bool force)
{
    int fam = current_family();
    uint32_t now = now_ms();
    if (!force && fam == g_reported_family && (int32_t)(now - g_pad_next) < 0)
        return;
    char line[32];
    snprintf(line, sizeof(line), "pad %s", family_name(fam));
    send_cmd(line);
    g_reported_family = fam;
    g_pad_next = now + 2000;
}

static void axis_tick(void)
{
    if (g_settings_active || !keyboard_visible()) {
        g_axis_dir = DIR_NONE;
        return;
    }
    int dir = axis_direction();
    uint32_t now = now_ms();
    if (dir == DIR_NONE) {
        g_axis_dir = DIR_NONE;
        return;
    }
    if (dir != g_axis_dir) {
        g_axis_dir = dir;
        send_cmd(dir_cmd(dir));
        g_axis_next = now + REPEAT_FIRST_MS;
    } else if ((int32_t)(now - g_axis_next) >= 0) {
        send_cmd(dir_cmd(dir));
        g_axis_next = now + REPEAT_MS;
    }
}

static void handle_abs(const struct input_event *ev, int rt_threshold)
{
    if (ev->code == ABS_HAT0X) g_hat_x = ev->value;
    else if (ev->code == ABS_HAT0Y) g_hat_y = ev->value;
    else if (ev->code == ABS_X) g_stick_x = ev->value;
    else if (ev->code == ABS_Y) g_stick_y = ev->value;
    else if (ev->code == ABS_RZ) {
        if (rt_threshold < 1) rt_threshold = 12000;
        if (ev->value >= rt_threshold && !g_rt_down) {
            g_rt_down = true;
            if (!g_settings_active && keyboard_visible()) send_cmd("enter");
        } else if (ev->value < rt_threshold / 2) {
            g_rt_down = false;
        }
    }
}

static void handle_key(const struct input_event *ev, int family)
{
    if (ev->code == BTN_MODE || ev->code == KEY_HOME) {
        if (ev->value == 1) g_guide_down = true;
        else if (ev->value == 0) { g_guide_down = false; g_toggle_latched = false; }
        return;
    }
    if (ev->code == BTN_SELECT) {
        if (ev->value == 1) g_select_down = true;
        else if (ev->value == 0) { g_select_down = false; g_toggle_latched = false; }
    }
    if (g_settings_active) return;
    if (!keyboard_visible()) {
        if (g_guide_down && g_select_down && !g_toggle_latched) {
            g_toggle_latched = true;
            send_cmd("toggle");
        }
        return;
    }

    int delete_code = delete_code_for_family(family);
    if (ev->code == delete_code) {
        if (ev->value == 1) {
            g_delete_down = true;
            g_delete_at = now_ms();
        } else if (ev->value == 0 && g_delete_down) {
            g_delete_down = false;
            uint32_t held = now_ms() - g_delete_at;
            send_cmd(held >= DELETE_LONG_MS ? "clear" : "backspace");
        }
        return;
    }

    if (ev->value != 1) return;

    const char *cmd = face_cmd(ev->code, family);
    if (cmd) { send_cmd(cmd); return; }

    switch (ev->code) {
        case BTN_DPAD_UP: send_cmd("up"); break;
        case BTN_DPAD_DOWN: send_cmd("down"); break;
        case BTN_DPAD_LEFT: send_cmd("left"); break;
        case BTN_DPAD_RIGHT: send_cmd("right"); break;
        case BTN_TR2: send_cmd("enter"); break;
        case BTN_SELECT: send_cmd("lang"); break;
        case BTN_TL: send_cmd("cand_prev"); break;
        case BTN_TR: send_cmd("cand_next"); break;
        default: break;
    }
}

static void handle_event(const struct input_event *ev, int family,
                         int rt_threshold)
{
    if (ev->type == EV_ABS) handle_abs(ev, rt_threshold);
    else if (ev->type == EV_KEY) handle_key(ev, family);
}

int main(void)
{
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    char dir[600];
    runtime_path("input.lock", dir, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir(dir, 0700); }

    char lock_path[700];
    runtime_path("input.lock", lock_path, sizeof(lock_path));
    int lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        close(lock_fd);
        return 0;
    }

    scan();
    bool was_visible = false;
    char settings_flag[700];
    runtime_path("settings.active", settings_flag, sizeof(settings_flag));

    while (!g_stop) {
        g_settings_active = settings_active(settings_flag);
        bool vis = keyboard_visible() && !g_settings_active;
        if (vis != was_visible) {
            grab_all(vis);
            was_visible = vis;
        }

        struct pollfd fds[MAX_DEVICES];
        int n = 0;
        for (int i = 0; i < g_dev_count; i++) {
            if (g_dev[i].fd < 0) continue;
            fds[n].fd = g_dev[i].fd;
            fds[n].events = POLLIN;
            n++;
        }
        int pr = poll(fds, (nfds_t)n, 50);
        if (pr > 0) {
            for (int i = 0; i < n; i++) {
                if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
                    continue;
                bool dead = (fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
                int di = device_index_for_fd(fds[i].fd);
                int family = di >= 0 ? g_dev[di].family : FAMILY_GENERIC;
                int rt_threshold = di >= 0 ? g_dev[di].rt_threshold : 12000;
                if (fds[i].revents & POLLIN) {
                    struct input_event ev;
                    ssize_t r;
                    while ((r = read(fds[i].fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev))
                        handle_event(&ev, family, rt_threshold);
                    if (r < 0 && (errno == ENODEV || errno == EBADF)) dead = true;
                }
                if (dead) {
                    for (int j = 0; j < g_dev_count; j++) {
                        if (g_dev[j].fd == fds[i].fd) { remove_device(j); break; }
                    }
                }
            }
        }

        int old_count = g_dev_count;
        if (g_dev_count < MAX_DEVICES) scan();
        if (was_visible && g_dev_count > old_count) grab_all(true);
        if (g_settings_active) {
            g_reported_family = -1;
        } else {
            report_family(false);
        }
        axis_tick();
    }

    grab_all(false);
    for (int i = 0; i < g_dev_count; i++)
        if (g_dev[i].fd >= 0) close(g_dev[i].fd);
    return 0;
}
