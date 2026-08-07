# waynav

waynav is a grid-based keyboard mouse navigator for Wayland. It is
effectively a from-scratch rewrite of keynav for wlroots-compatible
compositors. I wrote it because keynav is tied to Xorg and I wanted the
same workflow on Wayland, especially on niri.

waynav is a short-lived process. Bind a compositor hotkey to launch it,
it draws a fullscreen overlay, grabs the keyboard, interprets a few
keypresses, moves the pointer through `wlr-virtual-pointer`, and exits
when you press `end`. There is no daemon, no IPC, and no state between
invocations.

Status: experimental, but already usable. Tested on niri.

<p align="center">
  <a href="assets/waynav-demo.gif" target="_blank">
    <img src="assets/waynav-demo.gif" alt="waynav demo" width="800">
  </a>
</p>

## Build and install

```sh
make
make test
make install
```

Useful developer targets:

```sh
make check
make dist
make lint
make clean
```

Builds from a git checkout derive `waynav --version` from
`git describe --tags`. Source tarballs fall back to the Meson project
version.

## Quick start

1. Build and install the binary. The default install prefix is
   `/usr`. Override `PREFIX` if you want a different install
   location.
2. Bind a compositor shortcut that launches `waynav`.
3. Create `~/.config/waynav/waynavrc`.
4. Press the compositor shortcut, navigate, and press `semicolon` or
   `Return` to exit.

For niri, the launcher can look like this:

```kdl
Mod+semicolon { spawn "waynav"; }
```

A small working config:

```text
clear
line-width 3
super+semicolon start,grid 2x2

q cell-select 1,warp
w cell-select 2,warp
a cell-select 3,warp
s cell-select 4,warp

h move-left,warp
j move-down,warp
k move-up,warp
l move-right,warp

space warp,click 1
BackSpace history-back
semicolon end
Return end
```

The config syntax follows keynav closely. The `start` line is kept for
keynav-style compatibility and provides startup commands such as the
initial grid size. The actual activation key is your compositor hotkey,
not the key sequence on the `start` line. `cursorzoom` centers its new region
on the current pointer position, including before waynav has moved the pointer.
Use `line-width <number>` to set the overlay grid line width; the default is
`1.0`.

## What it does

- parses keynav-style bindings from `~/.config/waynav/waynavrc`
- draws a fullscreen overlay through `wlr-layer-shell`
- moves and clicks the mouse through `wlr-virtual-pointer`
- supports region movement, cuts, cell selection, drag, scroll,
  `cursorzoom`, shell commands, and history undo
- handles fractional scale and key repeat
- opens on the compositor-selected active output in multi-output layouts

## Appearance

Three optional directives style the overlay. A later occurrence
overwrites an earlier one. Colors are bare hex (`rgb`, `rrggbb`, or
`rrggbbaa`); the `#` prefix is omitted because `#` starts a comment.
A missing alpha channel is fully opaque.

```text
grid-color 6699ff80   # grid lines and border (default)
region-bg  00000000   # fill of the active region (default: none)
line-width 1.5        # grid line thickness in pixels (default: 1)
```

## Requirements

waynav targets wlroots-compatible compositors. It currently relies on:

- `wlr-layer-shell`
- `wlr-virtual-pointer`
- `wp-fractional-scale`
- `wp-viewporter`
- `xdg-output`

It has been tested on niri.

## References

These projects were useful while designing or implementing waynav:

- [`jordansissel/keynav`](https://github.com/jordansissel/keynav) —
  the original Xorg tool that waynav rewrites for Wayland, including
  its config style and region semantics.
- [`moverest/wl-kbptr`](https://github.com/moverest/wl-kbptr) —
  the main Wayland reference for layer-shell, keyboard handling,
  shared-memory buffers, and virtual-pointer integration.
- [`rvaiya/warpd`](https://github.com/rvaiya/warpd) — grid-mode and
  keyboard-driven pointer design reference.
- [`bluedeep/hyprwarp`](https://github.com/bluedeep/hyprwarp) —
  additional virtual-pointer usage patterns on Wayland.
- [`niri-wm/niri`](https://github.com/niri-wm/niri) — compositor
  source used to confirm protocol support and test the main target
  environment.
