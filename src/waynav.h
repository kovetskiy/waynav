#ifndef WAYNAV_H
#define WAYNAV_H

#include "grid.h"

#include <cairo/cairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define MAX_COMMANDS 8
#define MAX_BINDINGS 64

enum command_type {
    CMD_START,
    CMD_END,
    CMD_GRID,
    CMD_CELL_SELECT,
    CMD_CUT_LEFT,
    CMD_CUT_RIGHT,
    CMD_CUT_UP,
    CMD_CUT_DOWN,
    CMD_MOVE_LEFT,
    CMD_MOVE_RIGHT,
    CMD_MOVE_UP,
    CMD_MOVE_DOWN,
    CMD_WARP,
    CMD_CLICK,
    CMD_DRAG,
    CMD_CURSORZOOM,
    CMD_HISTORY_BACK,
    CMD_SHELL,
};

struct command {
    enum command_type type;
    /* Arguments: grid cols/rows, click button, cursorzoom w/h,
     * shell string, cell number. Packed into a union. */
    union {
        struct {
            int cols, rows;
        } grid;
        int button; /* click, drag */
        int cell;   /* cell-select */
        struct {
            int w, h;
        } zoom; /* cursorzoom */
        char *shell_cmd;
    } arg;
};

struct binding {
    xkb_keysym_t keysym;
    xkb_keycode_t keycode;
    uint32_t mods; /* bitmask: MOD_SHIFT, MOD_CTRL, etc. */
    struct command commands[MAX_COMMANDS];
    int num_commands;
};

#define MOD_SHIFT (1 << 0)
#define MOD_CTRL (1 << 1)
#define MOD_ALT (1 << 2)
#define MOD_SUPER (1 << 3)

#define GRID_COLOR_DEFAULT 0x6699ff80u
#define REGION_BG_DEFAULT 0x00000000u
#define GRID_LINE_WIDTH_DEFAULT 1.0
#define CLICK_BUTTON_MAX 5
#define DRAG_BUTTON_MAX 3

struct config {
    struct binding bindings[MAX_BINDINGS];
    int num_bindings;
    /* The start binding's chained commands (grid setup etc.) */
    struct command start_commands[MAX_COMMANDS];
    int num_start_commands;
    /* Appearance. Colors are packed 0xRRGGBBAA and are seeded
     * from the GRID_*_DEFAULT constants in config_load. */
    uint32_t grid_color;
    uint32_t region_bg;
    double line_width;
};

/* Parse a waynavrc file into cfg. Returns 0 on success,
 * -1 if the file cannot be opened. Individual malformed
 * lines are warned and skipped. */
int config_load(struct config *cfg, const char *path);

xkb_keycode_t config_keycode_for_keysym(struct xkb_keymap *keymap,
                                        xkb_keysym_t sym);
void config_resolve_keycodes(struct config *cfg, struct xkb_keymap *keymap);

const struct binding *config_find_binding(const struct config *cfg,
                                          xkb_keycode_t keycode, uint32_t mods);

struct overlay;

/* Run a command chain: mutate region, warp, click, etc.
 * Each segment between history-back commands is a history unit. A changed
 * segment pushes its pre-segment region before the next history-back runs.
 * Stops at end and redraws. */
void execute_commands(struct overlay *ov, struct region_state *rs,
                      const struct command *cmds, int ncmds);

/* Run startup commands without adding an interactive history entry. */
void execute_startup_commands(struct overlay *ov, struct region_state *rs,
                              const struct command *cmds, int ncmds);

struct overlay *overlay_create(void);
void overlay_destroy(struct overlay *ov);
/* Schedule a redraw on the next frame callback. */
void overlay_redraw(struct overlay *ov, struct region_state *rs);

/* Logical output dimensions (not buffer pixels). */
int overlay_get_width(const struct overlay *ov);
int overlay_get_height(const struct overlay *ov);

/* Return the pointer position in logical surface coordinates. */
bool overlay_get_cursor_position(struct overlay *ov, int *x, int *y);

/* Run the event loop. Blocks until CMD_END or error. */
int overlay_run(struct overlay *ov, struct config *cfg,
                struct region_state *rs);

void overlay_stop(struct overlay *ov);
/* Release an active drag, if any. */
void overlay_stop_drag(struct overlay *ov, struct region_state *rs);

/* Give keyboard focus back to whatever is under the pointer, and wait for the
 * compositor to have done it.
 *
 * A click is only as useful as the focus behind it. A Wayland client is handed
 * the selection and primary selection only while it holds keyboard focus, so a
 * middle click delivered while this overlay holds the keyboard reaches a client
 * with nothing to paste. Idempotent, and there is no way back: the overlay is
 * ending by the time this is called. */
void overlay_release_keyboard(struct overlay *ov);

/* Coordinates are in logical output space. */
void vptr_warp(struct overlay *ov, int x, int y);

/* Keynav button numbers: 1=left, 2=middle, 3=right,
 * 4=scroll-up, 5=scroll-down. Button clicks temporarily hand keyboard focus to
 * the target so it can receive primary selection offers and focused clicks. */
void vptr_click(struct overlay *ov, int button);
void vptr_button_down(struct overlay *ov, int button);
void vptr_button_up(struct overlay *ov, int button);

#endif /* WAYNAV_H */
