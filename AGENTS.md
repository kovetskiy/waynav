# waynav

Wayland replacement for keynav. Reads `~/.config/waynav/waynavrc`
(same syntax as keynavrc), shows a fullscreen grid overlay via
wlr-layer-shell, and controls the mouse via wlr-virtual-pointer.
Targets wlroots-compatible compositors (tested on niri).

waynav is a short-lived process. A compositor hotkey launches it;
it grabs the keyboard, draws a grid, interprets keypresses, and
exits when the user presses `end`. No daemon, no IPC, no state
between invocations. A flock on `$XDG_RUNTIME_DIR/waynav.lock`
prevents concurrent instances.

## Build & test

```
make          # build (runs meson setup on first call)
make test     # build + run tests
make lint     # clang-tidy + scan-build + cppcheck in parallel
make clean    # wipe build dir
```

Compiler flags: `-Wall -Wextra -Werror`. Warnings are build
errors. `make lint` runs three static analysis tools in parallel;
each can also run alone (`make lint-tidy`, `make lint-scan`,
`make lint-cppcheck`).

## Architecture

Three subsystems with clean boundaries. The grid model and config
parser have zero Wayland dependencies and are tested in isolation.
The overlay owns all Wayland state and is tested manually.

### Region model (grid.c)

Pure geometry. A `region_state` holds the current rectangle, grid
subdivision parameters, a history stack, and drag state. All
`region_*` functions are deterministic — given the same inputs
they produce the same rectangle, with no side effects outside
the struct.

Cell numbering follows keynav's scheme: left-to-right,
top-to-bottom. For a 4×4 grid, cell 1 is top-left, cell 4 is
bottom-left of the first column, cell 5 is top of the second
column. The formula is column-major despite the visual layout
looking row-major. Getting this wrong breaks all cell-select
bindings.

### Config parser (config.c)

Reads waynavrc into a flat array of bindings. Each binding maps
a keysym+modifier pair to a chain of commands. One special case:
lines starting with `start` have their chained commands stored
separately as `start_commands` on the config struct, not as a
normal binding. These run once at startup to set the initial
grid.

Key names are resolved through xkbcommon's
`xkb_keysym_from_name`, so the config matches symbolic names
(like `semicolon` or `Return`), not physical scancodes. Shifted
bindings must use the shifted keysym — `shift+H` not `shift+h`
— because xkb resolves the keysym after applying modifiers.

### Overlay (overlay.c)

Owns the Wayland connection and all protocol objects: layer
surface, keyboard, virtual pointer, SHM buffer pool, frame
callbacks, and key repeat timer. This is the largest file and
the one with the most moving parts.

The overlay creates a fullscreen transparent surface on the
Overlay layer with exclusive keyboard interactivity. Its input
region is set to empty (0×0) so mouse events pass through to
windows underneath — only the keyboard is captured.

Rendering uses cairo into double-buffered wl_shm buffers.
Fractional scaling works by rendering at `buffer_size × scale`
and using `wp_viewport_set_destination` for the logical size.

The event loop polls two file descriptors: the Wayland
connection and a timerfd for key repeat. Key events go through
xkbcommon for keysym resolution, then through
`xkb_mods_to_config()` to translate xkb modifier state into
the config's `MOD_*` bitmask, then into `config_find_binding()`.

### Input dispatch (input.c)

Bridges config and region. Takes a matched binding's command
chain and runs each command in sequence: region mutations, warp,
click, drag, shell exec. After each chain it saves to history
(unless the chain contained `history-back`, to avoid pushing
the restored state right back) and requests a redraw.

Shell commands fork+exec through `/bin/sh -c` and are
fire-and-forget — no waitpid.

## Modifier translation

The config parser defines `MOD_SHIFT`, `MOD_CTRL`, `MOD_ALT`,
`MOD_SUPER` as a bitmask. The Wayland keyboard path receives
xkb modifier state. `xkb_mods_to_config()` in overlay.c
translates between them using `xkb_state_mod_name_is_active`
with `XKB_STATE_MODS_DEPRESSED`. If this translation is wrong
or incomplete, shifted/ctrl'd bindings silently stop matching
with no error.

## Wayland protocol stack

Four non-core protocols, all confirmed working on niri. Protocol
XML files live in `protocol/` and are processed by
wayland-scanner at build time.

- wlr-layer-shell — fullscreen overlay with keyboard grab.
- wlr-virtual-pointer — absolute pointer positioning, button
  press/release, axis scroll.
- wp-fractional-scale + wp-viewporter — correct rendering at
  non-integer scales (e.g. 1.5×).
- xdg-output — logical output geometry.

## Linting

`misc-include-cleaner` is disabled in `.clang-tidy` because
Wayland's generated umbrella header (`wayland-client.h`)
provides all `wl_*` types through inline code, and the checker
can't trace through it — it produces dozens of false positives
per file asking for headers that don't exist.

## Logging

All output goes to stderr through `log.h`. Four levels: error,
warn, info, debug. Use `log_err`/`log_warn`/`log_info`/
`log_debug` everywhere — no raw fprintf. `log_debug` is gated
by a threshold check before the format call, so it costs nothing
when disabled. Keep info-level sparse: startup, activation, exit.

## Writing style

Load the `writing-tropes` skill before writing prose or
AGENTS.md updates. Wrap lines at 80 columns. Code can exceed 80
when breaking would hurt readability.

## Tests

Tests use plain `assert()` and live in `tests/`. Each test
binary is standalone, registered with meson's test runner.
Grid and config are tested in isolation; the overlay is verified
manually on a live compositor.

## Workflow

Keep diffs minimal and update tests and the README in the same
session as behavior changes.
