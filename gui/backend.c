// SPDX-License-Identifier: GPL-3.0-or-later
#include "backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <strings.h>

static bool str_contains_icase(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return false;
    size_t n = strlen(needle);
    if (!n) return true;
    for (const char *p = haystack; *p; p++)
        if (strncasecmp(p, needle, n) == 0) return true;
    return false;
}

/* ── LVGL display plumbing ─────────────────────────────────────────────────── */

static uint32_t tick_cb(void)
{
    return SDL_GetTicks();
}

static int g_refresh_hz = 60;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    Backend *b = lv_display_get_user_data(disp);
    (void)px_map;
    if (getenv("TAJI_DRAW_DEBUG")) {
        fprintf(stderr, "[draw] %d,%d %dx%d\n", area->x1, area->y1,
                area->x2 - area->x1 + 1, area->y2 - area->y1 + 1);
    }
    b->needs_present = true;
    lv_display_flush_ready(disp);
}

static void backend_output_size(Backend *b, int *w, int *h)
{
    int ow = 0, oh = 0;
    if (!b || !b->renderer) {
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }
    int ww = 0, wh = 0;
    SDL_GetWindowSize(b->window, &ww, &wh);
    int raw_w = 0, raw_h = 0;
    SDL_GetRendererOutputSize(b->renderer, &raw_w, &raw_h);
    const char *no_hidpi = getenv("TAIJI_NO_HIDPI");

    /* Small handheld windows keep the old 1x layout.  Without this guard a
     * 640x360 Wayland window on a 1280x720 panel would suddenly use the
     * 720p design and every element would look much smaller. */
    bool use_hidpi = !(no_hidpi && *no_hidpi);
    if (use_hidpi && (ww < 1200 || wh < 700)) use_hidpi = false;
    if (use_hidpi && (raw_w > 3000 || raw_h > 2000)) use_hidpi = false;
    /* Only integer 2x-class scaling benefits from the denser native layout.
     * Fractional compositor scales keep the old 1x layout to avoid shrinking
     * the UI on handhelds. */
    if (use_hidpi && ww > 0 && raw_w > 0) {
        float ratio = (float)raw_w / (float)ww;
        if (ratio < 1.90f) use_hidpi = false;
    }

    if (use_hidpi && raw_w > 0 && raw_h > 0) {
        ow = raw_w;
        oh = raw_h;
    } else {
        ow = ww;
        oh = wh;
    }
    if (w) *w = ow;
    if (h) *h = oh;
}

void backend_logical_to_window(Backend *b, int *x, int *y)
{
    if (!b || !b->renderer || !x || !y) return;
    int wx = 0, wy = 0;
#if SDL_VERSION_ATLEAST(2, 0, 18)
    SDL_RenderLogicalToWindow(b->renderer, (float)*x, (float)*y, &wx, &wy);
    *x = wx;
    *y = wy;
    return;
#endif
    int ww = 0, wh = 0;
    SDL_GetWindowSize(b->window, &ww, &wh);
    if (ww > 0 && wh > 0 && b->width > 0 && b->height > 0) {
        *x = (int)((float)*x * (float)ww / (float)b->width);
        *y = (int)((float)*y * (float)wh / (float)b->height);
    }
}

static void pointer_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    Backend *b = lv_indev_get_user_data(indev);
    data->point.x = b->mouse_x;
    data->point.y = b->mouse_y;
    if (b->mouse_pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        b->mouse_pressed = false;
    } else if (b->mouse_released) {
        data->state = LV_INDEV_STATE_RELEASED;
        b->mouse_released = false;
    } else {
        data->state = b->mouse_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    }
}

/* ── Gamepad mapping ───────────────────────────────────────────────────────── */

typedef struct {
    bool swap_ab;
    bool swap_xy;
    bool custom;
    int confirm_btn;
    int cancel_btn;
    int menu1_btn;
    int menu2_btn;
    bool capture;
    int captured_btn;
} PadMap;

static PadMap g_map = {
    .swap_ab = false,
    .swap_xy = false,
    .custom = false,
    .confirm_btn = SDL_CONTROLLER_BUTTON_A,
    .cancel_btn = SDL_CONTROLLER_BUTTON_B,
    .menu1_btn = SDL_CONTROLLER_BUTTON_X,
    .menu2_btn = SDL_CONTROLLER_BUTTON_Y,
    .capture = false,
    .captured_btn = -1,
};

void backend_configure_gamepad(bool swap_ab, bool swap_xy, bool custom,
                               int confirm_btn, int cancel_btn,
                               int menu1_btn, int menu2_btn)
{
    g_map.swap_ab = swap_ab;
    g_map.swap_xy = swap_xy;
    g_map.custom = custom;
    g_map.confirm_btn = confirm_btn;
    g_map.cancel_btn = cancel_btn;
    g_map.menu1_btn = menu1_btn;
    g_map.menu2_btn = menu2_btn;
}

void backend_set_capture(bool enabled)
{
    g_map.capture = enabled;
    g_map.captured_btn = -1;
}

int backend_take_captured_button(void)
{
    int v = g_map.captured_btn;
    g_map.captured_btn = -1;
    return v;
}

/* ── Input helpers ─────────────────────────────────────────────────────────── */

static void emit(Backend *b, InputAction a, const char *text)
{
    if (b->input_cb) b->input_cb(b->input_userdata, a, text);
}

static void open_controller(Backend *b, int index)
{
    if (b->controller) return;
    if (!SDL_IsGameController(index)) return;
    b->controller = SDL_GameControllerOpen(index);
    if (!b->controller) return;
    SDL_Joystick *js = SDL_GameControllerGetJoystick(b->controller);
    b->controller_id = SDL_JoystickInstanceID(js);
    const char *name = SDL_GameControllerName(b->controller);
    snprintf(b->controller_name, sizeof(b->controller_name), "%s", name ? name : "Gamepad");
    b->controller_family = 0;
#if SDL_VERSION_ATLEAST(2, 0, 12)
    Uint16 vendor = SDL_GameControllerGetVendor(b->controller);
    if (vendor == 0x045e) b->controller_family = 1;
    else if (vendor == 0x054c) b->controller_family = 2;
    else if (vendor == 0x057e) b->controller_family = 3;
#endif
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (b->controller_family == 0) {
        switch (SDL_GameControllerGetType(b->controller)) {
            case SDL_CONTROLLER_TYPE_XBOX360:
            case SDL_CONTROLLER_TYPE_XBOXONE:
                b->controller_family = 1;
                break;
            case SDL_CONTROLLER_TYPE_PS3:
            case SDL_CONTROLLER_TYPE_PS4:
            case SDL_CONTROLLER_TYPE_PS5:
                b->controller_family = 2;
                break;
            case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
                b->controller_family = 3;
                break;
            default:
                break;
        }
    }
#endif
    if (b->controller_family == 0) {
        if (str_contains_icase(b->controller_name, "xbox") ||
            str_contains_icase(b->controller_name, "x-box") ||
            str_contains_icase(b->controller_name, "xinput") ||
            str_contains_icase(b->controller_name, "microsoft")) {
            b->controller_family = 1;
        } else if (str_contains_icase(b->controller_name, "playstation") ||
                   str_contains_icase(b->controller_name, "dualshock") ||
                   str_contains_icase(b->controller_name, "dualsense") ||
                   str_contains_icase(b->controller_name, "sony") ||
                   str_contains_icase(b->controller_name, "ps3") ||
                   str_contains_icase(b->controller_name, "ps4") ||
                   str_contains_icase(b->controller_name, "ps5")) {
            b->controller_family = 2;
        } else if (str_contains_icase(b->controller_name, "switch") ||
                   str_contains_icase(b->controller_name, "joy-con") ||
                   str_contains_icase(b->controller_name, "joycon") ||
                   str_contains_icase(b->controller_name, "nintendo")) {
            b->controller_family = 3;
        }
    }
    /* The settings UI follows the printed labels: Nintendo A (right)
     * confirms, B (bottom) goes back; Xbox A (bottom) confirms, B (right)
     * goes back.  No A/B swap is applied here. */
    SDL_GameControllerEventState(SDL_ENABLE);
    fprintf(stderr, "[armdeck-osk] controller connected: %s\n", b->controller_name);
}

static void close_controller(Backend *b, SDL_JoystickID id)
{
    if (!b->controller) return;
    SDL_Joystick *js = SDL_GameControllerGetJoystick(b->controller);
    if (b->controller_id == id || id < 0) {
        fprintf(stderr, "[armdeck-osk] controller disconnected: %s\n", b->controller_name);
        SDL_GameControllerClose(b->controller);
        b->controller = NULL;
        b->controller_id = -1;
        b->controller_family = 0;
        b->controller_name[0] = '\0';
    }
    (void)js;
}

static InputAction map_face_button(Backend *b, int button, bool down)
{
    (void)b;
    if (!down) return IN_NONE;

    if (g_map.capture) {
        g_map.captured_btn = button;
        g_map.capture = false;
        return IN_NONE;
    }

    if (g_map.custom) {
        if (button == g_map.confirm_btn) return IN_PAD_CONFIRM;
        if (button == g_map.cancel_btn) return IN_PAD_CANCEL;
        if (button == g_map.menu1_btn) return IN_X;
        if (button == g_map.menu2_btn) return IN_Y;
        return IN_NONE;
    }

    if (button == SDL_CONTROLLER_BUTTON_A) return g_map.swap_ab ? IN_PAD_CANCEL : IN_PAD_CONFIRM;
    if (button == SDL_CONTROLLER_BUTTON_B) return g_map.swap_ab ? IN_PAD_CONFIRM : IN_PAD_CANCEL;
    if (button == SDL_CONTROLLER_BUTTON_X) return g_map.swap_xy ? IN_Y : IN_X;
    if (button == SDL_CONTROLLER_BUTTON_Y) return g_map.swap_xy ? IN_X : IN_Y;
    return IN_NONE;
}

/* Direction repeat state shared by keyboard/controller/stick. */
typedef struct {
    int dir;              /* IN_UP..IN_RIGHT or 0 */
    uint32_t since;
    uint32_t next_repeat;
    uint32_t repeat_ms;
    bool active;
} DirRepeat;

static DirRepeat g_dir = {0};
static bool g_lt_held = false;
static bool g_rt_held = false;

static void reset_dir(void)
{
    g_dir.dir = 0;
    g_dir.active = false;
}

static void update_direction(Backend *b, int dir, uint32_t first_ms,
                             uint32_t repeat_ms)
{
    uint32_t now = SDL_GetTicks();
    if (repeat_ms < 30) repeat_ms = 30;
    if (dir <= 0) {
        reset_dir();
        return;
    }
    if (!g_dir.active || g_dir.dir != dir) {
        g_dir.dir = dir;
        g_dir.since = now;
        g_dir.repeat_ms = repeat_ms;
        g_dir.next_repeat = now + first_ms;
        g_dir.active = true;
        emit(b, (InputAction)dir, NULL);
        return;
    }
    g_dir.repeat_ms = repeat_ms;
    if (now >= g_dir.next_repeat) {
        g_dir.next_repeat = now + g_dir.repeat_ms;
        emit(b, (InputAction)dir, NULL);
    }
}

static void backend_apply_size(Backend *b, int w, int h);

/* ── SDL events ────────────────────────────────────────────────────────────── */

static void handle_key(Backend *b, SDL_Keycode key, bool down)
{
    if (!down) return;
    switch (key) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER: emit(b, IN_CONFIRM, NULL); break;
        case SDLK_ESCAPE: emit(b, IN_CANCEL, NULL); break;
        case SDLK_BACKSPACE: emit(b, IN_BACKSPACE, NULL); break;
        case SDLK_F11: emit(b, IN_FULLSCREEN, NULL); break;
        case SDLK_F5: emit(b, IN_SCAN, NULL); break;
        case SDLK_VOLUMEUP: emit(b, IN_VOLUME_UP, NULL); break;
        case SDLK_VOLUMEDOWN: emit(b, IN_VOLUME_DOWN, NULL); break;
        case SDLK_AUDIOMUTE:
        case SDLK_MUTE: emit(b, IN_VOLUME_MUTE, NULL); break;
        case SDLK_q:
            if (SDL_GetModState() & KMOD_CTRL) emit(b, IN_QUIT, NULL);
            break;
        case SDLK_x: emit(b, IN_X, NULL); break;
        case SDLK_y: emit(b, IN_Y, NULL); break;
        case SDLK_TAB: emit(b, (SDL_GetModState() & KMOD_SHIFT) ? IN_LB : IN_RB, NULL); break;
        case SDLK_PAGEUP: emit(b, IN_LT, NULL); break;
        case SDLK_PAGEDOWN: emit(b, IN_RT, NULL); break;
        case SDLK_F1: emit(b, IN_SELECT, NULL); break;
        default: break;
    }
}

static void handle_touch(Backend *b, const SDL_TouchFingerEvent *f,
                        bool down, bool up)
{
    if (!b || !f) return;
    int w = b->width > 0 ? b->width : 1;
    int h = b->height > 0 ? b->height : 1;
    int ww = 0, wh = 0;
    SDL_GetWindowSize(b->window, &ww, &wh);
    int x, y;
    if (b->renderer && ww > 0 && wh > 0) {
        int wx = (int)lroundf(f->x * (float)ww);
        int wy = (int)lroundf(f->y * (float)wh);
        float lx = 0, ly = 0;
        SDL_RenderWindowToLogical(b->renderer, wx, wy, &lx, &ly);
        x = (int)lroundf(lx);
        y = (int)lroundf(ly);
    } else {
        x = (int)lroundf(f->x * (float)w);
        y = (int)lroundf(f->y * (float)h);
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= w) x = w - 1;
    if (y >= h) y = h - 1;
    b->mouse_x = x;
    b->mouse_y = y;
    b->mouse_last = SDL_GetTicks();
    b->mouse_seen = true;
    if (down) {
        b->mouse_down = true;
        b->mouse_pressed = true;
    } else if (up) {
        b->mouse_down = false;
        b->mouse_released = true;
    }
}

void backend_poll(Backend *b)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                b->running = false;
                emit(b, IN_QUIT, NULL);
                break;
            case SDL_WINDOWEVENT:
                if (getenv("TAIJI_MOUSE_DEBUG") &&
                    (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
                     e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)) {
                    fprintf(stderr, "[mouse] focus %s\n",
                            e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED
                                ? "gained" : "lost");
                }
                if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
                    b->running = false;
                } else if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                           e.window.event == SDL_WINDOWEVENT_RESIZED) {
                    int nw = 0, nh = 0;
                    backend_output_size(b, &nw, &nh);
                    if (nw > 0 && nh > 0) backend_apply_size(b, nw, nh);
                }
                break;
            case SDL_KEYDOWN:
                handle_key(b, e.key.keysym.sym, true);
                break;
            case SDL_TEXTINPUT:
                if (e.text.text[0]) emit(b, IN_TEXT, e.text.text);
                break;
            case SDL_MOUSEMOTION:
                if (e.motion.which == SDL_TOUCH_MOUSEID) break;
                /* SDL_RenderSetLogicalSize already converts window mouse
                 * coordinates into renderer logical coordinates.  Do not
                 * scale them a second time. */
                b->mouse_x = e.motion.x;
                b->mouse_y = e.motion.y;
                b->mouse_last = SDL_GetTicks();
                b->mouse_seen = true;
                if (getenv("TAIJI_MOUSE_DEBUG"))
                    fprintf(stderr, "[mouse] move %d,%d\n", e.motion.x, e.motion.y);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.which == SDL_TOUCH_MOUSEID) break;
                if (e.button.button == SDL_BUTTON_LEFT) {
                    b->mouse_x = e.button.x;
                    b->mouse_y = e.button.y;
                    b->mouse_down = true;
                    b->mouse_pressed = true;
                    if (getenv("TAIJI_MOUSE_DEBUG"))
                        fprintf(stderr, "[mouse] down %d,%d\n", e.button.x, e.button.y);
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.which == SDL_TOUCH_MOUSEID) break;
                if (e.button.button == SDL_BUTTON_LEFT) {
                    b->mouse_x = e.button.x;
                    b->mouse_y = e.button.y;
                    b->mouse_down = false;
                    b->mouse_released = true;
                    if (getenv("TAIJI_MOUSE_DEBUG"))
                        fprintf(stderr, "[mouse] up %d,%d\n", e.button.x, e.button.y);
                }
                break;
            case SDL_MOUSEWHEEL:
                if (e.wheel.y > 0) emit(b, IN_SCROLL_UP, NULL);
                else if (e.wheel.y < 0) emit(b, IN_SCROLL_DOWN, NULL);
                break;
            case SDL_FINGERDOWN:
                handle_touch(b, &e.tfinger, true, false);
                break;
            case SDL_FINGERMOTION:
                handle_touch(b, &e.tfinger, false, false);
                break;
            case SDL_FINGERUP:
                handle_touch(b, &e.tfinger, false, true);
                break;
            case SDL_CONTROLLERDEVICEADDED:
                open_controller(b, e.cdevice.which);
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                close_controller(b, e.cdevice.which);
                break;
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP: {
                int btn = e.cbutton.button;
                bool down = e.type == SDL_CONTROLLERBUTTONDOWN;
                if (!down) break;
                InputAction a = map_face_button(b, btn, down);
                if (a != IN_NONE) { emit(b, a, NULL); break; }
                switch (btn) {
                    case SDL_CONTROLLER_BUTTON_DPAD_UP:
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                        break; /* handled continuously below */
                    case SDL_CONTROLLER_BUTTON_START: emit(b, IN_START, NULL); break;
                    case SDL_CONTROLLER_BUTTON_BACK: emit(b, IN_SELECT, NULL); break;
                    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: emit(b, IN_LB, NULL); break;
                    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: emit(b, IN_RB, NULL); break;
                    default: break;
                }
                break;
            }
            default: break;
        }
    }

    /* Keyboard direction repeat (keyboard state, independent of SDL events). */
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    int kdir = 0;
    if (ks[SDL_SCANCODE_UP]) kdir = IN_UP;
    else if (ks[SDL_SCANCODE_DOWN]) kdir = IN_DOWN;
    else if (ks[SDL_SCANCODE_LEFT]) kdir = IN_LEFT;
    else if (ks[SDL_SCANCODE_RIGHT]) kdir = IN_RIGHT;

    /* Controller d-pad + left stick direction. */
    int cdir = 0;
    uint32_t cfirst = 300, crepeat = 110;
    if (b->controller) {
        int dx = 0, dy = 0;
        if (SDL_GameControllerGetButton(b->controller, SDL_CONTROLLER_BUTTON_DPAD_UP)) dy = -1;
        else if (SDL_GameControllerGetButton(b->controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) dy = 1;
        if (SDL_GameControllerGetButton(b->controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) dx = -1;
        else if (SDL_GameControllerGetButton(b->controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) dx = 1;
        if (dx == 0 && dy == 0) {
            float ax = (float)SDL_GameControllerGetAxis(b->controller, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
            float ay = (float)SDL_GameControllerGetAxis(b->controller, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
            float mag = fabsf(ax) > fabsf(ay) ? fabsf(ax) : fabsf(ay);
            if (mag > 0.30f) {
                if (fabsf(ax) > fabsf(ay)) dx = ax > 0 ? 1 : -1;
                else dy = ay > 0 ? 1 : -1;
                /* Analog stick: gentle push scrolls slowly, full push fast. */
                float t = (mag - 0.30f) / 0.70f;
                if (t < 0) t = 0;
                if (t > 1) t = 1;
                cfirst = 220;
                crepeat = (uint32_t)(155.0f - 110.0f * t);
            }
        }
        if (dx < 0) cdir = IN_LEFT;
        else if (dx > 0) cdir = IN_RIGHT;
        else if (dy < 0) cdir = IN_UP;
        else if (dy > 0) cdir = IN_DOWN;

        int lt = SDL_GameControllerGetAxis(b->controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int rt = SDL_GameControllerGetAxis(b->controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (lt > 18000 && !g_lt_held) { emit(b, IN_LT, NULL); g_lt_held = true; }
        if (lt <= 18000) g_lt_held = false;
        if (rt > 18000 && !g_rt_held) { emit(b, IN_RT, NULL); g_rt_held = true; }
        if (rt <= 18000) g_rt_held = false;
    }
    int dir = kdir ? kdir : cdir;
    if (kdir) update_direction(b, dir, 300, 110);
    else update_direction(b, dir, cfirst, crepeat);
}

void backend_text_input(bool enabled)
{
    if (enabled) SDL_StartTextInput();
    else SDL_StopTextInput();
}

/* ── Init / present / shutdown ─────────────────────────────────────────────── */

bool backend_init(Backend *b, int width, int height, BackendInputCb cb, void *userdata)
{
    memset(b, 0, sizeof(*b));
    b->width = width;
    b->height = height;
    b->running = true;
    b->input_cb = cb;
    b->input_userdata = userdata;
    b->controller_id = -1;
    b->controller_family = 0;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[armdeck-osk] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_StopTextInput();
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
    int win_w = width, win_h = height;
    const char *win_env = getenv("TAIJI_WINDOW");
    if (win_env && *win_env) {
        int ew = 0, eh = 0;
        if (sscanf(win_env, "%dx%d", &ew, &eh) == 2 && ew >= 320 && eh >= 240) {
            win_w = ew;
            win_h = eh;
        }
    }
    b->window = SDL_CreateWindow("ArmDeck 输入法设置",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 win_w, win_h,
                                 SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN);
    if (!b->window) {
        fprintf(stderr, "[armdeck-osk] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    {
        SDL_DisplayMode dm;
        int di = SDL_GetWindowDisplayIndex(b->window);
        g_refresh_hz = 60;
        if (di >= 0 && SDL_GetCurrentDisplayMode(di, &dm) == 0 &&
            dm.refresh_rate > 0)
            g_refresh_hz = dm.refresh_rate;
        fprintf(stderr, "[armdeck-osk] display refresh: %d Hz\n", g_refresh_hz);
    }
    SDL_SetRelativeMouseMode(SDL_FALSE);
    SDL_SetWindowGrab(b->window, SDL_FALSE);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    b->renderer = SDL_CreateRenderer(b->window, -1,
                                     SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
    if (!b->renderer) {
        b->renderer = SDL_CreateRenderer(b->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!b->renderer) {
        fprintf(stderr, "[armdeck-osk] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }
    {
        SDL_RendererInfo ri;
        if (SDL_GetRendererInfo(b->renderer, &ri) == 0 && ri.name &&
            strstr(ri.name, "opengl"))
            SDL_GL_SetSwapInterval(1);
    }
    int draw_w = width, draw_h = height;
    backend_output_size(b, &draw_w, &draw_h);
    if (draw_w < 320) draw_w = 320;
    if (draw_h < 240) draw_h = 240;
    b->width = draw_w;
    b->height = draw_h;
    b->mouse_x = draw_w / 2;
    b->mouse_y = draw_h / 2;

    SDL_RenderSetLogicalSize(b->renderer, b->width, b->height);

    b->texture = SDL_CreateTexture(b->renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING, b->width, b->height);
    if (!b->texture) {
        fprintf(stderr, "[armdeck-osk] SDL_CreateTexture failed: %s\n", SDL_GetError());
        return false;
    }

    size_t fb_bytes = (size_t)b->width * (size_t)b->height * 4u;
    b->framebuffer = (uint32_t *)malloc(fb_bytes);
    if (!b->framebuffer) return false;
    memset(b->framebuffer, 0, fb_bytes);

    lv_init();
    lv_tick_set_cb(tick_cb);
    b->display = lv_display_create(b->width, b->height);
    lv_display_set_user_data(b->display, b);
    lv_display_set_color_format(b->display, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(b->display, b->framebuffer, NULL, fb_bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(b->display, flush_cb);
    lv_display_set_default(b->display);
    {
        lv_timer_t *rt = lv_display_get_refr_timer(b->display);
        if (rt && g_refresh_hz > 0)
            lv_timer_set_period(rt, (uint32_t)(1000 / g_refresh_hz));
    }

    b->pointer = lv_indev_create();
    lv_indev_set_type(b->pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(b->pointer, pointer_read_cb);
    lv_indev_set_user_data(b->pointer, b);

    /* Open already connected controllers. */
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) { open_controller(b, i); break; }
    }
    return true;
}

static void backend_apply_size(Backend *b, int w, int h)
{
    if (w < 320) w = 320;
    if (h < 240) h = 240;
    if (w == b->width && h == b->height) return;

    size_t fb_bytes = (size_t)w * (size_t)h * 4u;
    uint32_t *fb = (uint32_t *)malloc(fb_bytes);
    if (!fb) return;
    memset(fb, 0, fb_bytes);

    if (b->texture) SDL_DestroyTexture(b->texture);
    b->texture = SDL_CreateTexture(b->renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!b->texture) { free(fb); return; }

    uint32_t *old = b->framebuffer;
    b->framebuffer = fb;
    b->width = w;
    b->height = h;

    SDL_RenderSetLogicalSize(b->renderer, w, h);
    if (b->display) {
        lv_display_set_resolution(b->display, w, h);
        lv_display_set_buffers(b->display, b->framebuffer, NULL, fb_bytes,
                               LV_DISPLAY_RENDER_MODE_DIRECT);
    }
    free(old);
    b->needs_present = true;
    b->resized = true;
    fprintf(stderr, "[armdeck-osk] display size: %dx%d\n", w, h);
}

void backend_refresh_size(Backend *b)
{
    if (!b->window) return;
    int w = 0, h = 0;
    backend_output_size(b, &w, &h);
    if (w > 0 && h > 0) backend_apply_size(b, w, h);
}

void backend_present(Backend *b)
{
    b->needs_present = false;
    SDL_UpdateTexture(b->texture, NULL, b->framebuffer, b->width * 4);
    SDL_SetRenderDrawColor(b->renderer, 5, 6, 10, 255);
    SDL_RenderClear(b->renderer);
    SDL_RenderCopy(b->renderer, b->texture, NULL, NULL);
    SDL_RenderPresent(b->renderer);

    if (g_refresh_hz > 0) {
        static uint32_t last_present;
        uint32_t interval = 1000u / (uint32_t)g_refresh_hz;
        uint32_t now = SDL_GetTicks();
        if (last_present && (int32_t)(now - last_present) < (int32_t)interval)
            SDL_Delay(interval - (now - last_present));
        last_present = SDL_GetTicks();
    }
}

void backend_toggle_fullscreen(Backend *b)
{
    b->fullscreen = !b->fullscreen;
    SDL_SetWindowFullscreen(b->window, b->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    backend_refresh_size(b);
    SDL_RenderSetLogicalSize(b->renderer, b->width, b->height);
}

void backend_set_title(Backend *b, const char *title)
{
    if (b->window) SDL_SetWindowTitle(b->window, title);
}

bool backend_controller_button(Backend *b, int button)
{
    if (!b || !b->controller) return false;
    return SDL_GameControllerGetButton(b->controller,
                                      (SDL_GameControllerButton)button) != 0;
}

int backend_controller_axis(Backend *b, int axis)
{
    if (!b || !b->controller) return 0;
    return SDL_GameControllerGetAxis(b->controller, (SDL_GameControllerAxis)axis);
}

const char *backend_controller_name(Backend *b)
{
    return b->controller ? b->controller_name : NULL;
}

const char *backend_shoulder_left_label(Backend *b)
{
    if (!b || !b->controller) return "L1";
    if (b->controller_family == 1) return "LB";
    if (b->controller_family == 3) return "L";
    return "L1";
}

const char *backend_shoulder_right_label(Backend *b)
{
    if (!b || !b->controller) return "R1";
    if (b->controller_family == 1) return "RB";
    if (b->controller_family == 3) return "R";
    return "R1";
}

const char *backend_trigger_left_label(Backend *b)
{
    if (!b || !b->controller) return "L2";
    if (b->controller_family == 1) return "LT";
    if (b->controller_family == 3) return "ZL";
    return "L2";
}

const char *backend_trigger_right_label(Backend *b)
{
    if (!b || !b->controller) return "R2";
    if (b->controller_family == 1) return "RT";
    if (b->controller_family == 3) return "ZR";
    return "R2";
}

int backend_controller_battery(Backend *b)
{
    if (!b->controller) return -1;
    SDL_Joystick *js = SDL_GameControllerGetJoystick(b->controller);
    SDL_JoystickPowerLevel lvl = SDL_JoystickCurrentPowerLevel(js);
    switch (lvl) {
        case SDL_JOYSTICK_POWER_EMPTY: return 2;
        case SDL_JOYSTICK_POWER_LOW: return 15;
        case SDL_JOYSTICK_POWER_MEDIUM: return 50;
        case SDL_JOYSTICK_POWER_FULL: return 100;
        default: return -1;
    }
}

void backend_shutdown(Backend *b)
{
    if (b->controller) SDL_GameControllerClose(b->controller);
    if (b->framebuffer) free(b->framebuffer);
    if (b->texture) SDL_DestroyTexture(b->texture);
    if (b->renderer) SDL_DestroyRenderer(b->renderer);
    if (b->window) SDL_DestroyWindow(b->window);
    SDL_Quit();
    memset(b, 0, sizeof(*b));
}

bool backend_save_bmp(Backend *b, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    int row = b->width * 3;
    int pad = (4 - (row % 4)) % 4;
    int size = 54 + (row + pad) * b->height;
    unsigned char hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (unsigned char)(size); hdr[3] = (unsigned char)(size >> 8);
    hdr[4] = (unsigned char)(size >> 16); hdr[5] = (unsigned char)(size >> 24);
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = (unsigned char)(b->width); hdr[19] = (unsigned char)(b->width >> 8);
    hdr[20] = (unsigned char)(b->width >> 16); hdr[21] = (unsigned char)(b->width >> 24);
    hdr[22] = (unsigned char)(b->height); hdr[23] = (unsigned char)(b->height >> 8);
    hdr[24] = (unsigned char)(b->height >> 16); hdr[25] = (unsigned char)(b->height >> 24);
    hdr[26] = 1; hdr[28] = 24;
    hdr[34] = (unsigned char)((row + pad) * b->height);
    hdr[35] = (unsigned char)(((row + pad) * b->height) >> 8);
    hdr[36] = (unsigned char)(((row + pad) * b->height) >> 16);
    hdr[37] = (unsigned char)(((row + pad) * b->height) >> 24);
    fwrite(hdr, 1, 54, f);
    unsigned char *line = malloc((size_t)row + 4);
    memset(line, 0, (size_t)row + 4);
    for (int y = b->height - 1; y >= 0; y--) {
        for (int x = 0; x < b->width; x++) {
            uint32_t c = b->framebuffer[y * b->width + x];
            line[x * 3 + 0] = (unsigned char)(c & 0xFF);
            line[x * 3 + 1] = (unsigned char)((c >> 8) & 0xFF);
            line[x * 3 + 2] = (unsigned char)((c >> 16) & 0xFF);
        }
        fwrite(line, 1, (size_t)row + pad, f);
    }
    free(line);
    fclose(f);
    return true;
}
