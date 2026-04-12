# waynav

Wayland reimplementation of keynav's grid-based mouse navigation.
Only the subset of features used in the user's `~/.keynavrc` is
implemented. Targets niri compositor on Linux.

## Writing style

Read `~/.pi/agent/TROPES.md` before writing prose, documentation, or
AGENTS.md files. It catalogs AI writing patterns to avoid.

Wrap lines at 80 columns in all prose, markdown, and comments. Code
lines can exceed 80 when breaking would hurt readability.

## Build & test

```
meson setup build
meson compile -C build
meson test -C build
```

Run with: `./build/waynav`

Lint check (werror is on by default in meson.build):
```
meson compile -C build
```

The project uses `-Wall -Wextra -Werror` (via meson warning_level=2
+ werror=true).

## Architecture

waynav is a C11 program modeled after wl-kbptr. It:

1. Reads `~/.keynavrc` (same format as keynav).
2. Draws a grid overlay via wlr-layer-shell on the overlay layer.
3. Grabs keyboard input while active (exclusive keyboard
   interactivity on the layer surface).
4. Moves the pointer and clicks via wlr-virtual-pointer.
5. Runs shell commands when the config says `shell "..."`.

Key dependencies:
- wayland-client + wayland-protocols
- wlr-layer-shell-unstable-v1 (overlay surface)
- wlr-virtual-pointer-unstable-v1 (mouse control)
- cairo (grid drawing)
- xkbcommon (keymap handling)

Reference projects (cloned in ~/sources/github.com/):
- moverest/wl-kbptr — primary reference, same protocol stack
- rvaiya/warpd — grid mode, wayland platform layer
- bluedeep/hyprwarp — hint-based, virtual-pointer usage
- niri-wm/niri — compositor source, confirms protocol support

## Config features used

Extracted from `~/.keynavrc`. Only these commands are supported:

- `clear` — reset all bindings
- `start` — activate (with chained commands like `grid 4x4`)
- `end` — deactivate
- `grid NxM` — set grid dimensions
- `cell-select N` — jump to numbered grid cell
- `cut-left/right/up/down` — halve region
- `move-left/right/up/down` — shift region
- `warp` — move pointer to region center
- `click N` — click button N (1-5)
- `drag N` — toggle drag
- `cursorzoom W H` — zoom to area around cursor
- `history-back` — undo last region change
- `shell 'CMD'` — run shell command

## Code layout

```
src/
  waynav.h     — shared types and function declarations
  main.c       — entry point, config loading
  config.c     — keynavrc parser
  grid.c       — region state, grid math, cell selection
  overlay.c    — Wayland layer-shell surface, drawing, vptr
  input.c      — command chain execution
tests/
  test_grid.c   — region/grid operation tests
  test_config.c — config parser tests
protocol/
  *.xml         — Wayland protocol definitions
```

## Testing

Tests use plain assert(). Run with `meson test -C build`.
