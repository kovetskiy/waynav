/*
 * Wayland layer-shell overlay surface and grid rendering.
 *
 * Based on wl-kbptr's approach: layer-shell on overlay layer
 * with exclusive keyboard interactivity, wl_shm buffer, cairo
 * rendering, wlr-virtual-pointer for mouse control.
 */

#include "waynav.h"

/* Placeholder — full Wayland setup will go here. */

struct overlay {
    int width;
    int height;
    /* TODO: wl_display, wl_surface, layer_surface, shm,
     * virtual_pointer_mgr, seat, xkb state, cairo surface */
};

struct overlay *overlay_create(void) {
    /* TODO: connect to Wayland, bind globals, create layer
     * surface on overlay layer with exclusive keyboard. */
    return NULL;
}

void overlay_destroy(struct overlay *ov) {
    (void)ov;
}

void overlay_show(struct overlay *ov, struct region_state *rs) {
    (void)ov;
    (void)rs;
}

void overlay_hide(struct overlay *ov) {
    (void)ov;
}

void overlay_redraw(struct overlay *ov, struct region_state *rs) {
    (void)ov;
    (void)rs;
}

int overlay_get_width(struct overlay *ov) {
    return ov ? ov->width : 0;
}

int overlay_get_height(struct overlay *ov) {
    return ov ? ov->height : 0;
}

/* ── Virtual pointer ────────────────────────────────────── */

void vptr_warp(struct overlay *ov, int x, int y) {
    (void)ov;
    (void)x;
    (void)y;
}

void vptr_click(struct overlay *ov, int button) {
    (void)ov;
    (void)button;
}

void vptr_button_down(struct overlay *ov, int button) {
    (void)ov;
    (void)button;
}

void vptr_button_up(struct overlay *ov, int button) {
    (void)ov;
    (void)button;
}
