# waynav

Wayland replacement for keynav. Reads the same `~/.keynavrc`
config format but only implements the commands actually present
in the user's config. Targets wlroots-compatible compositors
(tested on niri).

## Writing style

Read `~/.pi/agent/TROPES.md` before writing prose, documentation,
or AGENTS.md files.

Wrap lines at 80 columns. Code can exceed 80 when breaking would
hurt readability.

## Build & test

```
make          # build (runs meson setup on first call)
make test     # build + run tests
make clean    # wipe build dir
```

Compiler flags enforce `-Wall -Wextra -Werror`. Fix warnings
before committing; the build will reject them.

## How it works

waynav is a short-lived process. It accepts `-c PATH` to
override the config file and `-l LEVEL` for log verbosity (see
`--help`). A compositor hotkey or manual CLI invocation launches
it; it shows a fullscreen grid overlay,
grabs the keyboard, and interprets keypresses according to the
loaded keynavrc. When the user triggers `end`, waynav tears
everything down and exits. There is no daemon mode, no IPC, no
persistent state between invocations.

The data flow through one keypress:

1. Wayland delivers a `wl_keyboard.key` event with an evdev
   scancode.
2. xkbcommon translates the scancode into a keysym. Modifier
   state is tracked separately via `wl_keyboard.modifiers`.
3. The keysym+mods pair is looked up in the config's binding
   table.
4. The matched binding has a command chain (1-N commands).
   `execute_commands()` runs each command in sequence.
5. Commands mutate `region_state` (grid math), call into the
   virtual pointer for mouse actions, or spawn shell processes.
6. After the chain finishes, the region is saved to history and
   the overlay redraws.

## Three subsystems

The code separates into three independent concerns. Understanding
the boundary between them matters more than any single file.

**Region model** (grid.c) — pure geometry with no Wayland
dependency. A `region_state` holds the current rectangle, its
grid subdivision parameters, a history stack, and drag state.
All `region_*` functions are deterministic and testable in
isolation. The cell numbering follows keynav's column-major
scheme: cells count down rows first, then across columns.
Getting this wrong breaks the entire 4x4 grid layout.

**Config parser** (config.c) — reads keynavrc into a flat array
of bindings. Each binding maps a keysym+modifier pair to a chain
of commands. One special case: the `start` command is extracted
from the binding line and stored separately as `start_commands`
on the config struct. These run once at activation to set up the
initial grid. The parser uses xkbcommon's `xkb_keysym_from_name`
for key name resolution, which means it works with any layout but
matches against the symbolic name written in the config file, not
against physical scancodes.

**Overlay** (overlay.c) — the Wayland surface, drawing, keyboard
handling, and virtual pointer. This is currently stubbed and is
the main remaining work. It will own the `wl_display` connection
and all protocol objects.

`input.c` bridges config and region: it takes a matched binding's
command chain and dispatches each command to the right region
function or overlay call. It also handles the `shell` command
(fork+exec, fire-and-forget).

## Wayland protocol stack

The overlay needs four non-core protocols. All are confirmed
working on niri. The protocol XML files live in `protocol/` and
are processed by `wayland-scanner` at build time.

- `wlr-layer-shell` — fullscreen overlay on the Overlay layer
  with exclusive keyboard interactivity.
- `wlr-virtual-pointer` — absolute pointer positioning, button
  press/release. This is how warp and click work.
- `wp-fractional-scale` + `wp-viewporter` — correct rendering
  on fractional-scale outputs (the target display is 1.5x).
- `xdg-output` — logical output geometry.

Read `docs/wayland-approach.md` before implementing the overlay.
It has code snippets and function references into wl-kbptr's
source for each piece.

## Modifier mismatch pitfall

The config parser defines its own modifier bitmask (MOD_SHIFT,
MOD_CTRL, etc.) which is matched during binding lookup. The
Wayland keyboard path will receive xkb modifier state. These
two representations must be translated consistently — the xkb
depressed-modifier mask needs to be converted to the config's
MOD_* bitmask before calling `config_find_binding()`. Getting
this translation wrong means shifted bindings silently stop
matching.

## Logging

All output goes to stderr through `log.h`. Four levels: error,
warn, info, debug. Set via `-l LEVEL` flag or `WAYNAV_LOG` env
var (flag wins). Default: info. `WAYNAV_LOG_COLOR` overrides
terminal color detection.

Use `log_err`/`log_warn`/`log_info`/`log_debug` everywhere.
No raw fprintf or printf for status messages. `log_debug` is
gated by a threshold check before the format call, so it has
zero cost when disabled. At debug level, output includes
`[file:line]`; at info and below it doesn't.

Keep info-level sparse: startup summary, activation, exit.
Put everything else at debug. Never log from per-frame or
per-motion paths unless gated behind debug.

## Tests

Tests live in `tests/` and use plain `assert()`. Each test
binary is a standalone executable registered with meson's test
runner. The grid and config modules are tested independently
of Wayland — the overlay is not unit-testable and gets verified
manually.

## Workflow

Keep diffs minimal and update tests and the README in the same
session as behavior changes.
