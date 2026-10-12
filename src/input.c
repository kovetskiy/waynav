/*
 * Keyboard input dispatch.
 *
 * Executes command chains from bindings that overlay.c resolves by
 * physical xkb keycode and modifier mask.
 */

#include "log.h"
#include "waynav.h"

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
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

static void reap_shell_children(int signal_number) {
    (void)signal_number;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    errno = saved_errno;
}

static bool install_shell_reaper(void) {
    static bool installed;
    if (installed)
        return true;

    struct sigaction action = {0};
    action.sa_handler = reap_shell_children;
    action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGCHLD, &action, NULL) < 0) {
        log_warn("shell: failed to install child reaper");
        return false;
    }

    installed = true;
    return true;
}

static void run_shell(const char *cmd) {
    log_debug("shell: %s", cmd);
    if (!install_shell_reaper())
        return;
    pid_t pid = fork();
    if (pid < 0) {
        log_warn("shell: fork failed");
        return;
    }
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
    [CMD_CUT_LEFT] = region_cut_left,   [CMD_CUT_RIGHT] = region_cut_right,
    [CMD_CUT_UP] = region_cut_up,       [CMD_CUT_DOWN] = region_cut_down,
    [CMD_MOVE_LEFT] = region_move_left, [CMD_MOVE_RIGHT] = region_move_right,
    [CMD_MOVE_UP] = region_move_up,     [CMD_MOVE_DOWN] = region_move_down,
};

static bool drag_button_valid(int button) {
    return button >= 1 && button <= DRAG_BUTTON_MAX;
}

static void exec_drag(struct overlay *ov, struct region_state *rs,
                      const struct command *c) {
    if (!drag_button_valid(c->arg.button)) {
        log_warn("drag: invalid button %d", c->arg.button);
        return;
    }
    if (rs->dragging) {
        overlay_stop_drag(ov, rs);
        return;
    }
    int cx, cy;
    region_center(rs, &cx, &cy);
    log_debug("drag start button=%d at %d,%d", c->arg.button, cx, cy);
    vptr_warp(ov, cx, cy);
    vptr_button_down(ov, c->arg.button);
    rs->dragging = true;
    rs->drag_button = c->arg.button;
}

static void exec_grid(struct overlay *ov, struct region_state *rs,
                      const struct command *c) {
    (void)ov;
    log_debug("grid %dx%d", c->arg.grid.cols, c->arg.grid.rows);
    if (!region_set_grid(rs, c->arg.grid.cols, c->arg.grid.rows))
        log_warn("grid: invalid dimensions %dx%d", c->arg.grid.cols,
                 c->arg.grid.rows);
}

static void exec_cell_select(struct overlay *ov, struct region_state *rs,
                             const struct command *c) {
    (void)ov;
    log_debug("cell-select %d", c->arg.cell);
    if (!region_cell_select(rs, c->arg.cell))
        log_warn("cell-select: invalid cell %d", c->arg.cell);
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
    log_debug("click %d", c->arg.button);
    if (rs->dragging && rs->drag_button == c->arg.button) {
        overlay_stop_drag(ov, rs);
        return;
    }
    vptr_click(ov, c->arg.button);
}

static void exec_cursorzoom(struct overlay *ov, struct region_state *rs,
                            const struct command *c) {
    int cursor_x;
    int cursor_y;
    if (!overlay_get_cursor_position(ov, &cursor_x, &cursor_y)) {
        log_warn("cursorzoom: pointer position unavailable");
        region_center(rs, &cursor_x, &cursor_y);
    }

    log_debug("cursorzoom %dx%d at %d,%d", c->arg.zoom.w, c->arg.zoom.h,
              cursor_x, cursor_y);
    if (!region_cursorzoom(rs, cursor_x, cursor_y, c->arg.zoom.w,
                           c->arg.zoom.h))
        log_warn("cursorzoom: invalid size %dx%d", c->arg.zoom.w,
                 c->arg.zoom.h);
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
    [CMD_GRID] = exec_grid,   [CMD_CELL_SELECT] = exec_cell_select,
    [CMD_WARP] = exec_warp,   [CMD_CLICK] = exec_click,
    [CMD_DRAG] = exec_drag,   [CMD_CURSORZOOM] = exec_cursorzoom,
    [CMD_SHELL] = exec_shell,
};

static void execute_one(struct overlay *ov, struct region_state *rs,
                        const struct command *c) {
    log_debug("exec: %s", cmd_name(c->type));

    if ((size_t)c->type < ARRAY_LEN(region_dispatch) &&
        region_dispatch[c->type]) {
        region_dispatch[c->type](rs);
        return;
    }

    if ((size_t)c->type < ARRAY_LEN(cmd_dispatch) && cmd_dispatch[c->type]) {
        cmd_dispatch[c->type](ov, rs, c);
        return;
    }

    if (c->type == CMD_END) {
        overlay_stop_drag(ov, rs);
        log_info("end");
        overlay_stop(ov);
    } else if (c->type == CMD_HISTORY_BACK) {
        region_history_back(rs);
    }
}

static bool region_equal(const struct region *a, const struct region *b) {
    return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h &&
           a->grid_cols == b->grid_cols && a->grid_rows == b->grid_rows;
}

static int execute_segment(struct overlay *ov, struct region_state *rs,
                           const struct command *cmds, int ncmds, int start,
                           bool record_history, bool *did_end) {
    struct region before = rs->current;
    int i = start;

    for (; i < ncmds; i++) {
        const struct command *cmd = &cmds[i];
        if (cmd->type == CMD_HISTORY_BACK)
            break;
        execute_one(ov, rs, cmd);
        if (cmd->type == CMD_END) {
            *did_end = true;
            i++;
            break;
        }
    }

    if (record_history && !region_equal(&before, &rs->current))
        region_save_snapshot(rs, before);
    return i;
}

static void execute_command_chain(struct overlay *ov, struct region_state *rs,
                                  const struct command *cmds, int ncmds,
                                  bool record_history) {
    /* Focus must reach the client before a terminating click delivers its
     * primary selection. The handoff can dispatch keys reentrantly. */
    for (int index = 0; index < ncmds; index++) {
        if (cmds[index].type == CMD_END) {
            overlay_release_keyboard(ov);
            break;
        }
    }

    region_resize(rs, overlay_get_width(ov), overlay_get_height(ov));

    bool did_end = false;
    int i = 0;
    while (i < ncmds && !did_end) {
        i = execute_segment(ov, rs, cmds, ncmds, i, record_history, &did_end);
        if (did_end)
            break;
        if (i < ncmds) {
            execute_one(ov, rs, &cmds[i]);
            i++;
        }
    }

    log_debug("region: %dx%d+%d+%d", rs->current.w, rs->current.h,
              rs->current.x, rs->current.y);
    overlay_redraw(ov, rs);
}

void execute_commands(struct overlay *ov, struct region_state *rs,
                      const struct command *cmds, int ncmds) {
    execute_command_chain(ov, rs, cmds, ncmds, true);
}

void execute_startup_commands(struct overlay *ov, struct region_state *rs,
                              const struct command *cmds, int ncmds) {
    execute_command_chain(ov, rs, cmds, ncmds, false);
}
