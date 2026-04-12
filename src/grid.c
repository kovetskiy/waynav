/*
 * Region/grid state and manipulation.
 *
 * The region is a rectangle on screen. Grid operations subdivide
 * it, cut/move shift it, and history allows undo.
 */

#include "waynav.h"
#include <math.h>
#include <string.h>

void region_init(struct region_state *rs, int scr_w, int scr_h) {
    memset(rs, 0, sizeof(*rs));
    rs->current.x = 0;
    rs->current.y = 0;
    rs->current.w = scr_w;
    rs->current.h = scr_h;
    rs->current.grid_cols = 2;
    rs->current.grid_rows = 2;
    rs->history_len = 0;
    rs->dragging = false;
    rs->drag_button = 0;
}

void region_save(struct region_state *rs) {
    /* Shift history if full. */
    if (rs->history_len >= HISTORY_MAX) {
        memmove(&rs->history[0], &rs->history[1],
                (HISTORY_MAX - 1) * sizeof(struct region));
        rs->history_len = HISTORY_MAX - 1;
    }
    rs->history[rs->history_len] = rs->current;
    rs->history_len++;
}

bool region_history_back(struct region_state *rs) {
    if (rs->history_len <= 0)
        return false;
    rs->history_len--;
    rs->current = rs->history[rs->history_len];
    return true;
}

void region_set_grid(struct region_state *rs, int cols, int rows) {
    if (cols > 0)
        rs->current.grid_cols = cols;
    if (rows > 0)
        rs->current.grid_rows = rows;
}

void region_cell_select(struct region_state *rs, int cell) {
    int cols = rs->current.grid_cols;
    int rows = rs->current.grid_rows;
    if (cell < 1 || cell > cols * rows)
        return;

    /* keynav numbering: top-to-bottom within a column, then
     * left-to-right across columns.
     * col = ceil(cell / rows), row = cell % rows (or rows if 0). */
    int col = (cell - 1) / rows;
    int row = (cell - 1) % rows;

    int cell_w = rs->current.w / cols;
    int cell_h = rs->current.h / rows;

    rs->current.x += cell_w * col;
    rs->current.y += cell_h * row;
    rs->current.w = cell_w;
    rs->current.h = cell_h;
}

void region_cut_left(struct region_state *rs) {
    rs->current.w /= 2;
}

void region_cut_right(struct region_state *rs) {
    int orig = rs->current.w;
    rs->current.w /= 2;
    rs->current.x += orig - rs->current.w;
}

void region_cut_up(struct region_state *rs) {
    rs->current.h /= 2;
}

void region_cut_down(struct region_state *rs) {
    int orig = rs->current.h;
    rs->current.h /= 2;
    rs->current.y += orig - rs->current.h;
}

void region_move_left(struct region_state *rs) {
    rs->current.x -= rs->current.w;
}

void region_move_right(struct region_state *rs) {
    rs->current.x += rs->current.w;
}

void region_move_up(struct region_state *rs) {
    rs->current.y -= rs->current.h;
}

void region_move_down(struct region_state *rs) {
    rs->current.y += rs->current.h;
}

void region_cursorzoom(struct region_state *rs,
                       int cursor_x, int cursor_y,
                       int w, int h) {
    rs->current.x = cursor_x - w / 2;
    rs->current.y = cursor_y - h / 2;
    rs->current.w = w;
    rs->current.h = h;
}

void region_center(const struct region_state *rs, int *x, int *y) {
    *x = rs->current.x + rs->current.w / 2;
    *y = rs->current.y + rs->current.h / 2;
}
