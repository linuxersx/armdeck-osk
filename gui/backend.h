// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_BACKEND_H
#define TAIJI_BACKEND_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL.h>
#include <lvgl.h>

typedef enum {
    IN_NONE = 0,
    IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT,
    IN_CONFIRM,       /* Enter / mouse double click */
    IN_CANCEL,        /* Escape */
    IN_PAD_CONFIRM,   /* gamepad A / confirm */
    IN_PAD_CANCEL,    /* gamepad B / cancel */
    IN_A, IN_B, IN_X, IN_Y,
    IN_START, IN_SELECT, IN_SCAN,
    IN_LB, IN_RB, IN_LT, IN_RT,
    IN_SCROLL_UP, IN_SCROLL_DOWN,
    IN_TEXT,          /* utf8 text in `text` */
    IN_BACKSPACE,
    IN_FULLSCREEN,
    IN_VOLUME_UP,
    IN_VOLUME_DOWN,
    IN_VOLUME_MUTE,
    IN_QUIT,
    IN_MOUSE_MOVE,
} InputAction;

typedef struct Backend Backend;
typedef void (*BackendInputCb)(void *userdata, InputAction action, const char *text);

struct Backend {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    lv_display_t *display;
    lv_indev_t *pointer;

    uint32_t *framebuffer;
    int width;
    int height;
    bool needs_present;
    bool running;
    bool resized;
    bool fullscreen;

    int mouse_x;
    int mouse_y;
    uint32_t mouse_last;
    bool mouse_seen;
    bool mouse_down;
    bool mouse_pressed;
    bool mouse_released;

    SDL_GameController *controller;
    SDL_JoystickID controller_id;
    char controller_name[128];
    int controller_family; /* 0 generic, 1 xbox, 2 playstation, 3 nintendo */

    BackendInputCb input_cb;
    void *input_userdata;

    uint32_t last_frame_ms;
};

bool backend_init(Backend *b, int width, int height, BackendInputCb cb, void *userdata);
void backend_poll(Backend *b);
void backend_present(Backend *b);
void backend_shutdown(Backend *b);
void backend_toggle_fullscreen(Backend *b);
void backend_set_title(Backend *b, const char *title);
void backend_refresh_size(Backend *b);
void backend_logical_to_window(Backend *b, int *x, int *y);
void backend_text_input(bool enabled);
void backend_configure_gamepad(bool swap_ab, bool swap_xy, bool custom,
                               int confirm_btn, int cancel_btn,
                               int menu1_btn, int menu2_btn);
void backend_set_capture(bool enabled);
int backend_take_captured_button(void);
const char *backend_controller_name(Backend *b);
int backend_controller_battery(Backend *b); /* 0..100, -1 unknown */
bool backend_controller_button(Backend *b, int button);
int backend_controller_axis(Backend *b, int axis);
const char *backend_shoulder_left_label(Backend *b);
const char *backend_shoulder_right_label(Backend *b);
const char *backend_trigger_left_label(Backend *b);
const char *backend_trigger_right_label(Backend *b); /* 0..100, -1 unknown */

#endif

bool backend_save_bmp(Backend *b, const char *path);
