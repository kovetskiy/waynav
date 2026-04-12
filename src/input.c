/*
 * Keyboard input dispatch.
 *
 * Maps xkb keysyms + modifiers to config bindings and executes
 * command chains.
 */

#include "log.h"
#include "waynav.h"

#include <unistd.h>

static const char *cmd_name(enum command_type type) {
    switch (type) {
    case CMD_START:
        return "start";
    case CMD_END:
        return "end";
    case CMD_GRID:
        return "grid";
    case CMD_CELL_SELECT:
        return "cell-select";
    case CMD_CUT_LEFT:
        return "cut-left";
    case CMD_CUT_RIGHT:
        return "cut-right";
    case CMD_CUT_UP:
        return "cut-up";
    case CMD_CUT_DOWN:
        return "cut-down";
    case CMD_MOVE_LEFT:
        return "move-left";
    case CMD_MOVE_RIGHT:
        return "move-right";
    case CMD_MOVE_UP:
        return "move-up";
    case CMD_MOVE_DOWN:
        return "move-down";
    case CMD_WARP:
        return "warp";
    case CMD_CLICK:
        return "click";
    case CMD_DRAG:
        return "drag";
    case CMD_CURSORZOOM:
        return "cursorzoom";
    case CMD_HISTORY_BACK:
        return "history-back";
    case CMD_SHELL:
        return "shell";
    }
    return "?";
}

static void run_shell(const char *cmd) {
    log_debug("shell: %s", cmd);
    pid_t pid = fork();
    if (pid == 0) {
        const char *argv[] = {"/bin/sh", "-c", cmd, NULL};
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    /* Don't wait — fire and forget like keynav. */
}

typedef void (*region_fn)(struct region_state *);

/* Index by command_type for simple region-only commands.
 * NULL entries are handled by the main dispatch. */
static region_fn region_dispatch[] = {
    [CMD_CUT_LEFT] = region_cut_left,
    [CMD_CUT_RIGHT] = region_cut_right,
    [CMD_CUT_UP] = region_cut_up,
    [CMD_CUT_DOWN] = region_cut_down,
    [CMD_MOVE_LEFT] = region_move_left,
    [CMD_MOVE_RIGHT] = region_move_right,
    [CMD_MOVE_UP] = region_move_up,
    [CMD_MOVE_DOWN] = region_move_down,
};

static void exec_drag(struct overlay *ov, struct region_state *rs,
                      const struct command *c) {
    if (rs->dragging) {
        log_debug("drag end button=%d", rs->drag_button);
        vptr_button_up(ov, rs->drag_button);
        rs->dragging = false;
        return;
    }
    int cx, cy;
    region_center(rs, &cx, &cy);
    log_debug("drag start button=%d at %d,%d",
              c->arg.button, cx, cy);
    vptr_warp(ov, cx, cy);
    vptr_button_down(ov, c->arg.button);
    rs->dragging = true;
    rs->drag_button = c->arg.button;
}

static void exec_grid(struct overlay *ov, struct region_state *rs,
                      const struct command *c) {
    (void)ov;
    log_debug("grid %dx%d", c->arg.grid.cols, c->arg.grid.rows);
    region_set_grid(rs, c->arg.grid.cols, c->arg.grid.rows);
}

static void exec_cell_select(struct overlay *ov,
                              struct region_state *rs,
                              const struct command *c) {
    (void)ov;
    log_debug("cell-select %d", c->arg.cell);
    region_cell_select(rs, c->arg.cell);
}

static void exec_warp(struct overlay *ov, struct region_state *rs,
                      const struct command *c) {
    (void)c;
    int cx, cy;
    region_center(rs, &cx, &cy);
    log_debug("warp to %d,%d", cx, cy);
    vptr_warp(ov, cx, cy);
}

static void exec_click(struct overlay *ov, struct region_state *rs,
                       const struct command *c) {
    (void)rs;
    log_debug("click %d", c->arg.button);
    vptr_click(ov, c->arg.button);
}

static void exec_cursorzoom(struct overlay *ov,
                             struct region_state *rs,
                             const struct command *c) {
    (void)ov;
    int cx, cy;
    region_center(rs, &cx, &cy);
    log_debug("cursorzoom %dx%d at %d,%d",
              c->arg.zoom.w, c->arg.zoom.h, cx, cy);
    region_cursorzoom(rs, cx, cy, c->arg.zoom.w, c->arg.zoom.h);
}

static void exec_shell(struct overlay *ov, struct region_state *rs,
                       const struct command *c) {
    (void)ov;
    (void)rs;
    if (c->arg.shell_cmd)
        run_shell(c->arg.shell_cmd);
}

typedef void (*cmd_handler)(struct overlay *, struct region_state *,
                            const struct command *);

/* Index by command_type. NULL entries are no-ops. */
static cmd_handler cmd_dispatch[] = {
    [CMD_GRID] = exec_grid,
    [CMD_CELL_SELECT] = exec_cell_select,
    [CMD_WARP] = exec_warp,
    [CMD_CLICK] = exec_click,
    [CMD_DRAG] = exec_drag,
    [CMD_CURSORZOOM] = exec_cursorzoom,
    [CMD_SHELL] = exec_shell,
};

static bool execute_one(struct overlay *ov, struct region_state *rs,
                        const struct command *c) {
    log_debug("exec: %s", cmd_name(c->type));

    if ((size_t)c->type < sizeof(region_dispatch) / sizeof(region_dispatch[0])
        && region_dispatch[c->type]) {
        region_dispatch[c->type](rs);
        return false;
    }

    if ((size_t)c->type < sizeof(cmd_dispatch) / sizeof(cmd_dispatch[0])
        && cmd_dispatch[c->type]) {
        cmd_dispatch[c->type](ov, rs, c);
        return false;
    }

    if (c->type == CMD_END) {
        log_info("end");
        overlay_stop(ov);
    } else if (c->type == CMD_HISTORY_BACK) {
        region_history_back(rs);
        return true;
    }

    return false;
}

void execute_commands(struct overlay *ov, struct region_state *rs,
                      const struct command *cmds, int ncmds) {
    bool did_history_back = false;

    for (int i = 0; i < ncmds; i++) {
        if (execute_one(ov, rs, &cmds[i]))
            did_history_back = true;
    }

    log_debug("region: %dx%d+%d+%d", rs->current.w, rs->current.h,
              rs->current.x, rs->current.y);

    if (!did_history_back)
        region_save(rs);
    overlay_redraw(ov, rs);
}
