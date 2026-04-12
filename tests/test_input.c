/*
 * Tests for input dispatch.
 */

#include "../src/waynav.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum test_event {
    EVENT_REDRAW,
    EVENT_STOP,
    EVENT_WARP,
    EVENT_CLICK,
    EVENT_BUTTON_DOWN,
    EVENT_BUTTON_UP,
};

struct overlay {
    int redraw_calls;
    int stop_calls;
    int warp_calls;
    int click_calls;
    int button_down_calls;
    int button_up_calls;
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
    assert(ov.event_count == 6);
    assert(ov.events[3] == EVENT_BUTTON_UP);
    assert(ov.events[4] == EVENT_STOP);
    assert(ov.events[5] == EVENT_REDRAW);
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
    assert(ov.event_count == 2);
    assert(ov.events[0] == EVENT_STOP);
    assert(ov.events[1] == EVENT_REDRAW);
}

int main(void) {
    test_end_releases_active_drag();
    test_end_without_drag_does_not_release_button();

    printf("All input tests passed.\n");
    return 0;
}
