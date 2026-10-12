/*
 * Tests for input dispatch.
 */

#include "../src/waynav.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum test_event {
    EVENT_REDRAW,
    EVENT_STOP,
    EVENT_WARP,
    EVENT_CLICK,
    EVENT_BUTTON_DOWN,
    EVENT_BUTTON_UP,
    EVENT_RELEASE_KEYBOARD,
};

static bool sigaction_failure;
static int fork_calls;

extern pid_t __real_fork(void);
extern int __real_sigaction(int signal_number, const struct sigaction *action,
                            struct sigaction *old_action);

pid_t __wrap_fork(void) {
    fork_calls++;
    return __real_fork();
}

int __wrap_sigaction(int signal_number, const struct sigaction *action,
                     struct sigaction *old_action) {
    if (sigaction_failure) {
        errno = EPERM;
        return -1;
    }
    return __real_sigaction(signal_number, action, old_action);
}

struct overlay {
    int width;
    int height;
    int redraw_calls;
    int stop_calls;
    int warp_calls;
    int click_calls;
    int button_down_calls;
    int button_up_calls;
    int release_keyboard_calls;
    int cursor_position_calls;
    int cursor_x;
    int cursor_y;
    bool cursor_position_known;
    int last_warp_x;
    int last_warp_y;
    int last_click_button;
    int last_button_down;
    int last_button_up;
    enum test_event events[16];
    int event_count;
};

static void record_event(struct overlay *ov, enum test_event event) {
    assert(ov->event_count < (int)(sizeof(ov->events) / sizeof(ov->events[0])));
    ov->events[ov->event_count++] = event;
}

void overlay_redraw(struct overlay *ov, struct region_state *rs) {
    (void)rs;
    ov->redraw_calls++;
    record_event(ov, EVENT_REDRAW);
}

void overlay_stop(struct overlay *ov) {
    ov->stop_calls++;
    record_event(ov, EVENT_STOP);
}

void overlay_release_keyboard(struct overlay *ov) {
    ov->release_keyboard_calls++;
    record_event(ov, EVENT_RELEASE_KEYBOARD);
}

int overlay_get_width(const struct overlay *ov) {
    return ov->width;
}

int overlay_get_height(const struct overlay *ov) {
    return ov->height;
}

bool overlay_get_cursor_position(struct overlay *ov, int *x, int *y) {
    ov->cursor_position_calls++;
    if (!ov->cursor_position_known)
        return false;
    *x = ov->cursor_x;
    *y = ov->cursor_y;
    return true;
}

void vptr_warp(struct overlay *ov, int x, int y) {
    ov->warp_calls++;
    ov->last_warp_x = x;
    ov->last_warp_y = y;
    record_event(ov, EVENT_WARP);
}

void vptr_click(struct overlay *ov, int button) {
    ov->click_calls++;
    ov->last_click_button = button;
    record_event(ov, EVENT_CLICK);
}

void vptr_button_down(struct overlay *ov, int button) {
    ov->button_down_calls++;
    ov->last_button_down = button;
    record_event(ov, EVENT_BUTTON_DOWN);
}

void vptr_button_up(struct overlay *ov, int button) {
    ov->button_up_calls++;
    ov->last_button_up = button;
    record_event(ov, EVENT_BUTTON_UP);
}

void overlay_stop_drag(struct overlay *ov, struct region_state *rs) {
    if (!rs->dragging)
        return;
    vptr_button_up(ov, rs->drag_button);
    rs->dragging = false;
    rs->drag_button = 0;
}

static void test_cursorzoom_uses_pointer_position(void) {
    struct overlay ov;
    memset(&ov, 0, sizeof(ov));
    ov.cursor_x = 123;
    ov.cursor_y = 456;
    ov.cursor_position_known = true;

    struct region_state rs;
    region_init(&rs, 800, 600);

    struct command commands[] = {
        {
            .type = CMD_GRID,
            .arg.grid = {.cols = 1, .rows = 1},
        },
        {
            .type = CMD_CURSORZOOM,
            .arg.zoom = {.w = 10, .h = 20},
        },
    };
    execute_commands(&ov, &rs, commands, 2);

    assert(ov.cursor_position_calls == 1);
    assert(rs.current.x == 118);
    assert(rs.current.y == 446);
    assert(rs.current.w == 10);
    assert(rs.current.h == 20);
    assert(rs.current.grid_cols == 1);
    assert(rs.current.grid_rows == 1);
}

static void test_move_warp_stays_on_screen(void) {
    struct overlay ov;
    memset(&ov, 0, sizeof(ov));

    struct region_state rs;
    region_init(&rs, 800, 600);

    struct command commands[] = {
        {
            .type = CMD_MOVE_LEFT,
        },
        {
            .type = CMD_WARP,
        },
    };
    execute_commands(&ov, &rs, commands, 2);

    assert(rs.current.x == 0);
    assert(rs.current.w == 800);
    assert(ov.last_warp_x == 400);
    assert(ov.last_warp_y == 300);
}

static void test_history_back_restores_previous_region(void) {
    struct overlay ov;
    memset(&ov, 0, sizeof(ov));

    struct region_state rs;
    region_init(&rs, 800, 600);

    struct command cut_left = {
        .type = CMD_CUT_LEFT,
    };
    execute_commands(&ov, &rs, &cut_left, 1);

    assert(rs.current.w == 400);
    assert(rs.current.h == 600);
    assert(rs.history_len == 1);
    assert(rs.history[0].w == 800);
    assert(rs.history[0].h == 600);

    struct command history_back = {
        .type = CMD_HISTORY_BACK,
    };
    execute_commands(&ov, &rs, &history_back, 1);

    assert(rs.current.w == 800);
    assert(rs.current.h == 600);
    assert(rs.history_len == 0);
}

static void test_history_back_undoes_previous_chain_commands(void) {
    struct overlay ov;
    memset(&ov, 0, sizeof(ov));

    struct region_state rs;
    region_init(&rs, 800, 600);

    struct command commands[] = {
        {
            .type = CMD_CUT_LEFT,
        },
        {
            .type = CMD_HISTORY_BACK,
        },
    };
    execute_commands(&ov, &rs, commands, 2);

    assert(rs.current.w == 800);
    assert(rs.current.h == 600);
    assert(rs.history_len == 0);
}

static void test_mutation_after_history_back_is_undoable(void) {
    struct overlay ov;
    memset(&ov, 0, sizeof(ov));

    struct region_state rs;
    region_init(&rs, 800, 600);

    struct command cut_left = {
        .type = CMD_CUT_LEFT,
    };
    execute_commands(&ov, &rs, &cut_left, 1);

    struct command commands[] = {
        {
            .type = CMD_HISTORY_BACK,
        },
        {
            .type = CMD_CUT_UP,
        },
    };
    execute_commands(&ov, &rs, commands, 2);

    assert(rs.current.w == 800);
    assert(rs.current.h == 300);
    assert(rs.history_len == 1);
    assert(rs.history[0].w == 800);
    assert(rs.history[0].h == 600);

    struct command history_back = {
        .type = CMD_HISTORY_BACK,
    };
    execute_commands(&ov, &rs, &history_back, 1);

    assert(rs.current.w == 800);
    assert(rs.current.h == 600);
    assert(rs.history_len == 0);
}

static void test_non_region_command_does_not_save_history(void) {
    struct overlay ov;
    memset(&ov, 0, sizeof(ov));

    struct region_state rs;
    region_init(&rs, 800, 600);

    struct command cut_left = {
        .type = CMD_CUT_LEFT,
    };
    execute_commands(&ov, &rs, &cut_left, 1);
    assert(rs.history_len == 1);

    struct command click = {
        .type = CMD_CLICK,
        .arg.button = 1,
    };
    execute_commands(&ov, &rs, &click, 1);
    assert(rs.history_len == 1);

    struct command history_back = {
        .type = CMD_HISTORY_BACK,
    };
    execute_commands(&ov, &rs, &history_back, 1);

    assert(rs.current.w == 800);
    assert(rs.current.h == 600);
    assert(rs.history_len == 0);
}

static void test_output_resize_is_not_command_history(void) {
    struct overlay ov;
    memset(&ov, 0, sizeof(ov));
    ov.width = 400;
    ov.height = 300;

    struct region_state rs;
    region_init(&rs, 800, 600);

    struct command click = {
        .type = CMD_CLICK,
        .arg.button = 1,
    };
    execute_commands(&ov, &rs, &click, 1);

    assert(rs.current.w == 400);
    assert(rs.current.h == 300);
    assert(rs.history_len == 0);
}

static void test_startup_commands_do_not_save_history(void) {
    struct overlay ov;
    memset(&ov, 0, sizeof(ov));

    struct region_state rs;
    region_init(&rs, 800, 600);

    struct command commands[] = {
        {
            .type = CMD_GRID,
            .arg.grid = {.cols = 4, .rows = 4},
        },
    };
    execute_startup_commands(&ov, &rs, commands, 1);

    assert(rs.current.grid_cols == 4);
    assert(rs.current.grid_rows == 4);
    assert(rs.history_len == 0);
    assert(ov.redraw_calls == 1);
}

static void test_startup_end_stops_command_chain(void) {
    struct overlay ov;
    memset(&ov, 0, sizeof(ov));

    struct region_state rs;
    region_init(&rs, 800, 600);

    struct command commands[] = {
        {.type = CMD_END},
        {.type = CMD_CLICK, .arg.button = 1},
        {.type = CMD_DRAG, .arg.button = 1},
    };
    execute_startup_commands(&ov, &rs, commands, 3);

    assert(ov.stop_calls == 1);
    assert(ov.click_calls == 0);
    assert(ov.button_down_calls == 0);
    assert(!rs.dragging);
    assert(ov.redraw_calls == 1);
}

static void test_end_stops_command_chain(void) {
    struct overlay ov;
    memset(&ov, 0, sizeof(ov));

    struct region_state rs;
    region_init(&rs, 800, 600);

    struct command commands[] = {
        {
            .type = CMD_END,
        },
        {
            .type = CMD_CLICK,
            .arg.button = 1,
        },
        {
            .type = CMD_DRAG,
            .arg.button = 1,
        },
    };
    execute_commands(&ov, &rs, commands, 3);

    assert(ov.stop_calls == 1);
    assert(ov.click_calls == 0);
    assert(ov.button_down_calls == 0);
    assert(!rs.dragging);
    assert(rs.drag_button == 0);
    assert(ov.event_count == 3);
    assert(ov.events[0] == EVENT_RELEASE_KEYBOARD);
    assert(ov.events[1] == EVENT_STOP);
    assert(ov.events[2] == EVENT_REDRAW);
}

static void test_end_makes_later_history_back_unreachable(void) {
    struct overlay ov;
    memset(&ov, 0, sizeof(ov));

    struct region_state rs;
    region_init(&rs, 800, 600);

    struct command commands[] = {
        {
            .type = CMD_CUT_LEFT,
        },
        {
            .type = CMD_END,
        },
        {
            .type = CMD_HISTORY_BACK,
        },
    };
    execute_commands(&ov, &rs, commands, 3);

    assert(rs.current.w == 400);
    assert(rs.history_len == 1);
    assert(rs.history[0].w == 800);

    struct command history_back = {
        .type = CMD_HISTORY_BACK,
    };
    execute_commands(&ov, &rs, &history_back, 1);

    assert(rs.current.w == 800);
    assert(rs.history_len == 0);
}

static void test_invalid_drag_button_does_not_start_drag(void) {
    struct overlay ov;
    memset(&ov, 0, sizeof(ov));

    struct region_state rs;
    region_init(&rs, 800, 600);

    struct command drag = {
        .type = CMD_DRAG,
        .arg.button = 4,
    };
    execute_commands(&ov, &rs, &drag, 1);

    assert(!rs.dragging);
    assert(rs.drag_button == 0);
    assert(ov.button_down_calls == 0);
    assert(ov.button_up_calls == 0);
}

static void test_matching_click_releases_active_drag(void) {
    struct overlay ov;
    memset(&ov, 0, sizeof(ov));

    struct region_state rs;
    region_init(&rs, 800, 600);

    struct command drag = {
        .type = CMD_DRAG,
        .arg.button = 1,
    };
    execute_commands(&ov, &rs, &drag, 1);

    struct command click = {
        .type = CMD_CLICK,
        .arg.button = 1,
    };
    execute_commands(&ov, &rs, &click, 1);

    assert(!rs.dragging);
    assert(rs.drag_button == 0);
    assert(ov.click_calls == 0);
    assert(ov.button_down_calls == 1);
    assert(ov.button_up_calls == 1);
}

static void test_other_click_keeps_active_drag(void) {
    struct overlay ov;
    memset(&ov, 0, sizeof(ov));

    struct region_state rs;
    region_init(&rs, 800, 600);

    struct command drag = {
        .type = CMD_DRAG,
        .arg.button = 1,
    };
    execute_commands(&ov, &rs, &drag, 1);

    struct command click = {
        .type = CMD_CLICK,
        .arg.button = 3,
    };
    execute_commands(&ov, &rs, &click, 1);

    assert(rs.dragging);
    assert(rs.drag_button == 1);
    assert(ov.click_calls == 1);
    assert(ov.button_down_calls == 1);
    assert(ov.button_up_calls == 0);
}

static bool shell_child_was_reaped(void) {
    for (int i = 0; i < 1000; i++) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid < 0 && errno == ECHILD)
            return true;
        if (pid > 0)
            return false;
        usleep(1000);
    }
    return false;
}

static void test_shell_fails_without_child_reaper(void) {
    struct overlay ov;
    memset(&ov, 0, sizeof(ov));

    struct region_state rs;
    region_init(&rs, 800, 600);

    char path[128];
    snprintf(path, sizeof(path), "/tmp/waynav_test_shell_failed_%ld",
             (long)getpid());
    char shell_cmd[256];
    snprintf(shell_cmd, sizeof(shell_cmd), "touch %s", path);
    unlink(path);
    sigaction_failure = true;
    fork_calls = 0;
    struct command shell = {
        .type = CMD_SHELL,
        .arg.shell_cmd = shell_cmd,
    };
    execute_commands(&ov, &rs, &shell, 1);
    sigaction_failure = false;

    assert(fork_calls == 0);
    assert(access(path, F_OK) != 0);
}

static void test_shell_children_are_reaped(void) {
    struct overlay ov;
    memset(&ov, 0, sizeof(ov));

    struct region_state rs;
    region_init(&rs, 800, 600);

    char path[128];
    snprintf(path, sizeof(path), "/tmp/waynav_test_shell_reaped_%ld",
             (long)getpid());
    char shell_cmd[256];
    snprintf(shell_cmd, sizeof(shell_cmd), "touch %s", path);
    unlink(path);
    fork_calls = 0;
    struct command shell = {
        .type = CMD_SHELL,
        .arg.shell_cmd = shell_cmd,
    };
    execute_commands(&ov, &rs, &shell, 1);

    assert(fork_calls == 1);
    assert(shell_child_was_reaped());
    assert(access(path, F_OK) == 0);
    unlink(path);
}

static void test_end_releases_active_drag(void) {
    struct overlay ov;
    memset(&ov, 0, sizeof(ov));

    struct region_state rs;
    region_init(&rs, 800, 600);

    struct command drag = {
        .type = CMD_DRAG,
        .arg.button = 1,
    };
    execute_commands(&ov, &rs, &drag, 1);

    assert(rs.dragging);
    assert(rs.drag_button == 1);
    assert(ov.button_down_calls == 1);
    assert(ov.button_up_calls == 0);
    assert(ov.stop_calls == 0);
    assert(ov.event_count == 3);
    assert(ov.events[0] == EVENT_WARP);
    assert(ov.events[1] == EVENT_BUTTON_DOWN);
    assert(ov.events[2] == EVENT_REDRAW);

    struct command end = {
        .type = CMD_END,
    };
    execute_commands(&ov, &rs, &end, 1);

    assert(!rs.dragging);
    assert(rs.drag_button == 0);
    assert(ov.button_up_calls == 1);
    assert(ov.last_button_up == 1);
    assert(ov.stop_calls == 1);
    assert(ov.event_count == 7);
    assert(ov.events[3] == EVENT_RELEASE_KEYBOARD);
    assert(ov.events[4] == EVENT_BUTTON_UP);
    assert(ov.events[5] == EVENT_STOP);
    assert(ov.events[6] == EVENT_REDRAW);
}

static void test_end_without_drag_does_not_release_button(void) {
    struct overlay ov;
    memset(&ov, 0, sizeof(ov));

    struct region_state rs;
    region_init(&rs, 800, 600);

    struct command end = {
        .type = CMD_END,
    };
    execute_commands(&ov, &rs, &end, 1);

    assert(!rs.dragging);
    assert(rs.drag_button == 0);
    assert(ov.button_up_calls == 0);
    assert(ov.stop_calls == 1);
    assert(ov.event_count == 3);
    assert(ov.events[0] == EVENT_RELEASE_KEYBOARD);
    assert(ov.events[1] == EVENT_STOP);
    assert(ov.events[2] == EVENT_REDRAW);
}

/* "warp,click 2,end" is middle click paste, and the order is the whole of it: a
 * client is offered the primary selection only while it holds keyboard focus,
 * so a click sent before the keyboard is handed back arrives somewhere with
 * nothing to paste. */
static void test_ending_batch_releases_the_keyboard_before_clicking(void) {
    struct overlay ov;
    memset(&ov, 0, sizeof(ov));

    struct region_state rs;
    region_init(&rs, 800, 600);

    struct command commands[] = {
        {.type = CMD_WARP},
        {.type = CMD_CLICK, .arg.button = 2},
        {.type = CMD_END},
    };
    execute_commands(&ov, &rs, commands, 3);

    assert(ov.release_keyboard_calls == 1);
    assert(ov.click_calls == 1);
    assert(ov.last_click_button == 2);
    assert(ov.event_count == 5);
    assert(ov.events[0] == EVENT_RELEASE_KEYBOARD);
    assert(ov.events[1] == EVENT_WARP);
    assert(ov.events[2] == EVENT_CLICK);
    assert(ov.events[3] == EVENT_STOP);
    assert(ov.events[4] == EVENT_REDRAW);
}

/* A persistent click must not use the permanent keyboard release for end. */
static void test_click_without_end_does_not_permanently_release_keyboard(void) {
    struct overlay ov;
    memset(&ov, 0, sizeof(ov));

    struct region_state rs;
    region_init(&rs, 800, 600);

    struct command commands[] = {
        {.type = CMD_WARP},
        {.type = CMD_CLICK, .arg.button = 2},
    };
    execute_commands(&ov, &rs, commands, 2);

    assert(ov.release_keyboard_calls == 0);
    assert(ov.click_calls == 1);
}

int main(void) {
    test_cursorzoom_uses_pointer_position();
    test_move_warp_stays_on_screen();
    test_history_back_restores_previous_region();
    test_history_back_undoes_previous_chain_commands();
    test_mutation_after_history_back_is_undoable();
    test_non_region_command_does_not_save_history();
    test_output_resize_is_not_command_history();
    test_startup_commands_do_not_save_history();
    test_startup_end_stops_command_chain();
    test_end_stops_command_chain();
    test_end_makes_later_history_back_unreachable();
    test_invalid_drag_button_does_not_start_drag();
    test_matching_click_releases_active_drag();
    test_other_click_keeps_active_drag();
    test_shell_fails_without_child_reaper();
    test_shell_children_are_reaped();
    test_end_releases_active_drag();
    test_end_without_drag_does_not_release_button();
    test_ending_batch_releases_the_keyboard_before_clicking();
    test_click_without_end_does_not_permanently_release_keyboard();

    printf("All input tests passed.\n");
    return 0;
}
