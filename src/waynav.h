#ifndef WAYNAV_H
#define WAYNAV_H

#include <cairo/cairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#define HISTORY_MAX 100

struct region {
    int x, y, w, h;
    int grid_cols, grid_rows;
};

struct region_state {
    struct region current;
    struct region history[HISTORY_MAX];
    int history_len;
    bool dragging;
    int drag_button;
};

/* Reset to full screen with a 2×2 default grid. */
void region_init(struct region_state *rs, int scr_w, int scr_h);

/* Push current region onto the history stack. Drops the
 * oldest entry when full. */
void region_save(struct region_state *rs);

/* Restore the most recent history entry. Returns false if
 * the history stack is empty. */
bool region_history_back(struct region_state *rs);

/* Set grid subdivision. Values ≤ 0 are ignored. */
void region_set_grid(struct region_state *rs, int cols, int rows);

/* Select a cell using keynav's column-major numbering:
 * cell 1 is top-left, cells fill top-to-bottom then
 * left-to-right. Out-of-range cells are ignored. */
void region_cell_select(struct region_state *rs, int cell);
void region_cut_left(struct region_state *rs);
void region_cut_right(struct region_state *rs);
void region_cut_up(struct region_state *rs);
void region_cut_down(struct region_state *rs);
void region_move_left(struct region_state *rs);
void region_move_right(struct region_state *rs);
void region_move_up(struct region_state *rs);
void region_move_down(struct region_state *rs);
void region_cursorzoom(struct region_state *rs, int cursor_x, int cursor_y,
                       int w, int h);

/* Center coordinates of the current region. */
void region_center(const struct region_state *rs, int *x, int *y);

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
    uint32_t mods; /* bitmask: MOD_SHIFT, MOD_CTRL, etc. */
    struct command commands[MAX_COMMANDS];
    int num_commands;
};

#define MOD_SHIFT (1 << 0)
#define MOD_CTRL (1 << 1)
#define MOD_ALT (1 << 2)
#define MOD_SUPER (1 << 3)

struct config {
    struct binding bindings[MAX_BINDINGS];
    int num_bindings;
    /* The start binding's chained commands (grid setup etc.) */
    struct command start_commands[MAX_COMMANDS];
    int num_start_commands;
};

/* Parse a waynavrc file into cfg. Returns 0 on success,
 * -1 if the file cannot be opened. Individual malformed
 * lines are warned and skipped. */
int config_load(struct config *cfg, const char *path);

/* Return the first binding matching sym+mods, or NULL. */
const struct binding *config_find_binding(const struct config *cfg,
                                          xkb_keysym_t sym, uint32_t mods);

struct overlay;

/* Run a command chain: mutate region, warp, click, etc.
 * Saves to history and redraws afterward (skips save if
 * the chain contained history-back). */
void execute_commands(struct overlay *ov, struct region_state *rs,
                      const struct command *cmds, int ncmds);

struct overlay *overlay_create(void);
void overlay_destroy(struct overlay *ov);
/* Schedule a redraw on the next frame callback. */
void overlay_redraw(struct overlay *ov, struct region_state *rs);

/* Logical output dimensions (not buffer pixels). */
int overlay_get_width(const struct overlay *ov);
int overlay_get_height(const struct overlay *ov);

/* Run the event loop. Blocks until CMD_END or error. */
int overlay_run(struct overlay *ov, struct config *cfg,
                struct region_state *rs);

void overlay_stop(struct overlay *ov);

/* Coordinates are in logical output space. */
void vptr_warp(struct overlay *ov, int x, int y);

/* Keynav button numbers: 1=left, 2=middle, 3=right,
 * 4=scroll-up, 5=scroll-down. */
void vptr_click(struct overlay *ov, int button);
void vptr_button_down(struct overlay *ov, int button);
void vptr_button_up(struct overlay *ov, int button);

#endif /* WAYNAV_H */
