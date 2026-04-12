/*
 * Keyboard input dispatch.
 *
 * Maps xkb keysyms + modifiers to config bindings and executes
 * command chains.
 */

#include "log.h"
#include "waynav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *cmd_name(enum command_type type) {
    switch (type) {
    case CMD_START:        return "start";
    case CMD_END:          return "end";
    case CMD_GRID:         return "grid";
    case CMD_CELL_SELECT:  return "cell-select";
    case CMD_CUT_LEFT:     return "cut-left";
    case CMD_CUT_RIGHT:    return "cut-right";
    case CMD_CUT_UP:       return "cut-up";
    case CMD_CUT_DOWN:     return "cut-down";
    case CMD_MOVE_LEFT:    return "move-left";
    case CMD_MOVE_RIGHT:   return "move-right";
    case CMD_MOVE_UP:      return "move-up";
    case CMD_MOVE_DOWN:    return "move-down";
    case CMD_WARP:         return "warp";
    case CMD_CLICK:        return "click";
    case CMD_DRAG:         return "drag";
    case CMD_CURSORZOOM:   return "cursorzoom";
    case CMD_HISTORY_BACK: return "history-back";
    case CMD_SHELL:        return "shell";
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

void execute_commands(struct overlay *ov,
                      struct region_state *rs,
                      const struct command *cmds,
                      int ncmds) {
    for (int i = 0; i < ncmds; i++) {
        const struct command *c = &cmds[i];
        int cx, cy;

        log_debug("exec: %s", cmd_name(c->type));

        switch (c->type) {
        case CMD_START:
            /* Should not appear in normal bindings. */
            break;
        case CMD_END:
            log_info("end");
            overlay_hide(ov);
            break;
        case CMD_GRID:
            log_debug("grid %dx%d",
                      c->arg.grid.cols, c->arg.grid.rows);
            region_set_grid(rs, c->arg.grid.cols,
                            c->arg.grid.rows);
            break;
        case CMD_CELL_SELECT:
            log_debug("cell-select %d", c->arg.cell);
            region_cell_select(rs, c->arg.cell);
            break;
        case CMD_CUT_LEFT:
            region_cut_left(rs);
            break;
        case CMD_CUT_RIGHT:
            region_cut_right(rs);
            break;
        case CMD_CUT_UP:
            region_cut_up(rs);
            break;
        case CMD_CUT_DOWN:
            region_cut_down(rs);
            break;
        case CMD_MOVE_LEFT:
            region_move_left(rs);
            break;
        case CMD_MOVE_RIGHT:
            region_move_right(rs);
            break;
        case CMD_MOVE_UP:
            region_move_up(rs);
            break;
        case CMD_MOVE_DOWN:
            region_move_down(rs);
            break;
        case CMD_WARP:
            region_center(rs, &cx, &cy);
            log_debug("warp to %d,%d", cx, cy);
            vptr_warp(ov, cx, cy);
            break;
        case CMD_CLICK:
            log_debug("click %d", c->arg.button);
            vptr_click(ov, c->arg.button);
            break;
        case CMD_DRAG:
            if (rs->dragging) {
                log_debug("drag end button=%d", rs->drag_button);
                vptr_button_up(ov, rs->drag_button);
                rs->dragging = false;
            } else {
                region_center(rs, &cx, &cy);
                log_debug("drag start button=%d at %d,%d",
                          c->arg.button, cx, cy);
                vptr_warp(ov, cx, cy);
                vptr_button_down(ov, c->arg.button);
                rs->dragging = true;
                rs->drag_button = c->arg.button;
            }
            break;
        case CMD_CURSORZOOM:
            region_center(rs, &cx, &cy);
            log_debug("cursorzoom %dx%d at %d,%d",
                      c->arg.zoom.w, c->arg.zoom.h, cx, cy);
            region_cursorzoom(rs, cx, cy,
                              c->arg.zoom.w, c->arg.zoom.h);
            break;
        case CMD_HISTORY_BACK:
            region_history_back(rs);
            break;
        case CMD_SHELL:
            if (c->arg.shell_cmd)
                run_shell(c->arg.shell_cmd);
            break;
        }
    }

    log_debug("region: %dx%d+%d+%d",
              rs->current.w, rs->current.h,
              rs->current.x, rs->current.y);

    /* After each command chain, save state and redraw. */
    region_save(rs);
    overlay_redraw(ov, rs);
}
