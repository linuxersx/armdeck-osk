# armdeck-osk

ArmDeck Wayland on-screen keyboard with Chinese input.

Components:

- `armdeck-osk` — layer-shell keyboard, Cairo/Pango rendering, input-method-v2
- `armdeck-oskd` — persistent supervisor, restarts the keyboard if it exits
- `armdeck-osk-input` — gamepad forwarding to the keyboard
- `armdeck-osk-settings` — LVGL settings application

## Configuration

The keyboard reads:

```text
~/.config/armdeck-osk/config
```

Main options:

- `theme_preset` — 0 deep space, 1 paper white, 2 sunset, 3 forest
- `lock_theme` — 1 ignores the palette sent by the desktop
- `color_*` — per-color AARRGGBB overrides
- `layout` — 0 full, 1 compact, 2 split
- `height_pct`, `margin`, `gap`, `radius`, `split_gap`
- `mode` — 0 docked, 1 floating
- `float_x`, `float_y`, `float_w`, `float_h`
- `default_lang`, `show_candidates`, `font_scale`

The settings application writes this file and asks the running keyboard to
reload it over the socket.

## Socket

```text
$XDG_RUNTIME_DIR/armdeck-osk/osk-<session>-<display>.sock
```

Commands:

```text
show, hide, toggle, reload
up, down, left, right, confirm, cancel
backspace, space, enter, lang, page_prev, page_next
theme <preset> <bg> <surface> <card> <card_soft> <elevated> <text> <text2>
      <text3> <accent> <accent2> <border> <border_focus> <light>
```

## Service

```text
systemctl --user enable --now armdeck-oskd.service
```

The Taiji session starts the daemon automatically.
