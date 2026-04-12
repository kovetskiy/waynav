/*
 * Wayland layer-shell overlay, keyboard input, grid rendering,
 * and virtual pointer.
 *
 * Connects to the compositor, creates a fullscreen transparent
 * overlay on the Overlay layer with exclusive keyboard grab,
 * draws the grid with cairo into wl_shm buffers, and controls
 * the mouse via wlr-virtual-pointer.
 *
 * Based on wl-kbptr's approach.
 */

#include "log.h"
#include "waynav.h"

#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#include <cairo/cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

/* ── SHM buffer pool ────────────────────────────────────── */

enum buf_state {
    BUF_UNINIT = 0,
    BUF_READY  = 1,
    BUF_BUSY   = 2,
};

struct shm_buffer {
    enum buf_state       state;
    struct wl_buffer    *wl_buf;
    cairo_surface_t     *cairo_surface;
    cairo_t             *cr;
    void                *data;
    size_t               data_size;
    uint32_t             width;
    uint32_t             height;
};

struct buffer_pool {
    struct shm_buffer bufs[2];
};

static int create_shm_file(size_t size) {
    char name[] = "/tmp/waynav-shm-XXXXXX";
    int fd = mkostemp(name, O_CLOEXEC);
    if (fd < 0) return -1;
    unlink(name);
    int err;
    while ((err = ftruncate(fd, (off_t)size)) && errno == EINTR)
        ;
    if (err) { close(fd); return -1; }
    return fd;
}

static void buf_release(void *data, struct wl_buffer *wl_buf) {
    (void)wl_buf;
    ((struct shm_buffer *)data)->state = BUF_READY;
}

static const struct wl_buffer_listener buf_listener = {
    .release = buf_release,
};

static void buf_destroy(struct shm_buffer *b) {
    if (b->state == BUF_UNINIT) return;
    if (b->cr)             cairo_destroy(b->cr);
    if (b->cairo_surface)  cairo_surface_destroy(b->cairo_surface);
    if (b->wl_buf)         wl_buffer_destroy(b->wl_buf);
    if (b->data)           munmap(b->data, b->data_size);
    memset(b, 0, sizeof(*b));
}

static struct shm_buffer *buf_get(struct wl_shm *shm,
                                  struct buffer_pool *pool,
                                  uint32_t w, uint32_t h) {
    struct shm_buffer *b = NULL;
    for (int i = 0; i < 2; i++) {
        if (pool->bufs[i].state != BUF_BUSY) {
            b = &pool->bufs[i];
            break;
        }
    }
    if (!b) return NULL;

    if (b->width != w || b->height != h)
        buf_destroy(b);

    if (b->state == BUF_UNINIT) {
        uint32_t stride = (uint32_t)cairo_format_stride_for_width(
            CAIRO_FORMAT_ARGB32, (int)w);
        size_t sz = (size_t)h * stride;
        int fd = create_shm_file(sz);
        if (fd < 0) return NULL;

        void *data = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) { close(fd); return NULL; }

        struct wl_shm_pool *pool_wl =
            wl_shm_create_pool(shm, fd, (int32_t)sz);
        b->wl_buf = wl_shm_pool_create_buffer(
            pool_wl, 0, (int32_t)w, (int32_t)h,
            (int32_t)stride, WL_SHM_FORMAT_ARGB8888);
        wl_buffer_add_listener(b->wl_buf, &buf_listener, b);
        wl_shm_pool_destroy(pool_wl);
        close(fd);

        b->data      = data;
        b->data_size = sz;
        b->width     = w;
        b->height    = h;
        b->state     = BUF_READY;
        b->cairo_surface = cairo_image_surface_create_for_data(
            b->data, CAIRO_FORMAT_ARGB32, (int)w, (int)h,
            (int)stride);
        b->cr = cairo_create(b->cairo_surface);
    }
    return b;
}

/* ── Overlay state ──────────────────────────────────────── */

struct overlay {
    /* Wayland globals */
    struct wl_display                      *display;
    struct wl_registry                     *registry;
    struct wl_compositor                   *compositor;
    struct wl_shm                          *shm;
    struct wl_seat                         *seat;
    struct wl_output                       *wl_output;
    struct zwlr_layer_shell_v1             *layer_shell;
    struct zwlr_virtual_pointer_manager_v1 *vptr_mgr;
    struct zxdg_output_manager_v1          *xdg_out_mgr;
    struct wp_viewporter                   *viewporter;
    struct wp_fractional_scale_manager_v1  *frac_scale_mgr;

    /* Objects */
    struct wl_surface                *surface;
    struct zwlr_layer_surface_v1     *layer_surface;
    struct wl_callback               *frame_cb;
    struct wp_viewport               *viewport;
    struct wp_fractional_scale_v1    *frac_scale;
    struct wl_region                 *input_region;
    struct zwlr_virtual_pointer_v1   *vptr;

    /* Keyboard / xkb */
    struct wl_keyboard   *keyboard;
    struct xkb_context   *xkb_ctx;
    struct xkb_keymap    *xkb_keymap;
    struct xkb_state     *xkb_state;

    /* Output info */
    int32_t out_width;
    int32_t out_height;
    int32_t out_x;
    int32_t out_y;
    int32_t out_scale;    /* integer scale */
    int32_t frac_scale_v; /* scale*120, 0 if unavailable */

    /* Surface */
    uint32_t surf_width;
    uint32_t surf_height;
    bool     configured;

    /* Rendering */
    struct buffer_pool pool;

    /* Key repeat */
    int      repeat_fd;    /* timerfd */
    int32_t  repeat_rate;  /* keys per second */
    int32_t  repeat_delay; /* ms before first repeat */
    uint32_t repeat_key;   /* evdev code of held key, 0=none */

    /* State pointers (set during overlay_run) */
    struct config       *cfg;
    struct region_state *rs;
    bool                 running;
};

/* ── Forward declarations ───────────────────────────────── */

static void send_frame(struct overlay *ov);
static void render_grid(struct overlay *ov, cairo_t *cr,
                        struct region_state *rs);
static uint32_t xkb_mods_to_config(struct overlay *ov);

/* ── Registry ───────────────────────────────────────────── */

static void noop() {}

static void registry_global(void *data, struct wl_registry *reg,
                            uint32_t name, const char *iface,
                            uint32_t version) {
    struct overlay *ov = data;
    (void)version;

    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        ov->compositor = wl_registry_bind(
            reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        ov->shm = wl_registry_bind(
            reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        if (!ov->seat) {
            ov->seat = wl_registry_bind(
                reg, name, &wl_seat_interface, 7);
        }
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        if (!ov->wl_output) {
            ov->wl_output = wl_registry_bind(
                reg, name, &wl_output_interface, 3);
        }
    } else if (strcmp(iface,
               zwlr_layer_shell_v1_interface.name) == 0) {
        ov->layer_shell = wl_registry_bind(
            reg, name, &zwlr_layer_shell_v1_interface, 2);
    } else if (strcmp(iface,
               zwlr_virtual_pointer_manager_v1_interface.name)
               == 0) {
        ov->vptr_mgr = wl_registry_bind(
            reg, name,
            &zwlr_virtual_pointer_manager_v1_interface, 2);
    } else if (strcmp(iface,
               zxdg_output_manager_v1_interface.name) == 0) {
        ov->xdg_out_mgr = wl_registry_bind(
            reg, name, &zxdg_output_manager_v1_interface, 2);
    } else if (strcmp(iface,
               wp_viewporter_interface.name) == 0) {
        ov->viewporter = wl_registry_bind(
            reg, name, &wp_viewporter_interface, 1);
    } else if (strcmp(iface,
               wp_fractional_scale_manager_v1_interface.name)
               == 0) {
        ov->frac_scale_mgr = wl_registry_bind(
            reg, name,
            &wp_fractional_scale_manager_v1_interface, 1);
    }
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = noop,
};

/* ── XDG output ─────────────────────────────────────────── */

static void xdg_out_pos(void *data,
                         struct zxdg_output_v1 *xdg_out,
                         int32_t x, int32_t y) {
    (void)xdg_out;
    struct overlay *ov = data;
    ov->out_x = x;
    ov->out_y = y;
}

static void xdg_out_size(void *data,
                          struct zxdg_output_v1 *xdg_out,
                          int32_t w, int32_t h) {
    (void)xdg_out;
    struct overlay *ov = data;
    ov->out_width  = w;
    ov->out_height = h;
}

static const struct zxdg_output_v1_listener xdg_out_listener = {
    .logical_position = xdg_out_pos,
    .logical_size     = xdg_out_size,
    .done             = noop,
    .name             = noop,
    .description      = noop,
};

/* ── Output listener (integer scale) ────────────────────── */

static void output_scale(void *data,
                          struct wl_output *output,
                          int32_t factor) {
    (void)output;
    struct overlay *ov = data;
    ov->out_scale = factor;
}

static const struct wl_output_listener output_listener = {
    .geometry    = noop,
    .mode        = noop,
    .done        = noop,
    .scale       = output_scale,
    .name        = noop,
    .description = noop,
};

/* ── Layer surface ──────────────────────────────────────── */

static void layer_configure(void *data,
                            struct zwlr_layer_surface_v1 *ls,
                            uint32_t serial,
                            uint32_t w, uint32_t h) {
    struct overlay *ov = data;
    ov->surf_width  = w;
    ov->surf_height = h;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    ov->configured = true;
    log_debug("layer configure: %ux%u", w, h);
}

static void layer_closed(void *data,
                         struct zwlr_layer_surface_v1 *ls) {
    (void)ls;
    struct overlay *ov = data;
    ov->running = false;
}

static const struct zwlr_layer_surface_v1_listener
    layer_listener = {
    .configure = layer_configure,
    .closed    = layer_closed,
};

/* ── Fractional scale ───────────────────────────────────── */

static void frac_preferred(void *data,
                           struct wp_fractional_scale_v1 *fs,
                           uint32_t scale) {
    (void)fs;
    struct overlay *ov = data;
    ov->frac_scale_v = (int32_t)scale;
    log_debug("fractional scale: %u/120 = %.2f",
              scale, scale / 120.0);
}

static const struct wp_fractional_scale_v1_listener
    frac_listener = {
    .preferred_scale = frac_preferred,
};

/* ── Keyboard ───────────────────────────────────────────── */

static void kbd_keymap(void *data, struct wl_keyboard *kbd,
                       uint32_t fmt, int fd, uint32_t size) {
    struct overlay *ov = data;
    (void)kbd;

    if (ov->xkb_state) {
        xkb_state_unref(ov->xkb_state);
        ov->xkb_state = NULL;
    }
    if (ov->xkb_keymap) {
        xkb_keymap_unref(ov->xkb_keymap);
        ov->xkb_keymap = NULL;
    }

    if (fmt == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        void *buf = mmap(NULL, size - 1, PROT_READ,
                         MAP_PRIVATE, fd, 0);
        if (buf != MAP_FAILED) {
            ov->xkb_keymap = xkb_keymap_new_from_buffer(
                ov->xkb_ctx, buf, size - 1,
                XKB_KEYMAP_FORMAT_TEXT_V1,
                XKB_KEYMAP_COMPILE_NO_FLAGS);
            munmap(buf, size - 1);
        }
    }
    close(fd);

    if (!ov->xkb_keymap) {
        ov->xkb_keymap = xkb_keymap_new_from_names(
            ov->xkb_ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    }
    ov->xkb_state = xkb_state_new(ov->xkb_keymap);
    log_debug("keyboard keymap loaded");
}

static void disarm_repeat(struct overlay *ov) {
    if (ov->repeat_fd < 0) return;
    struct itimerspec its = {0};
    timerfd_settime(ov->repeat_fd, 0, &its, NULL);
    ov->repeat_key = 0;
}

static void arm_repeat(struct overlay *ov, uint32_t key) {
    if (ov->repeat_fd < 0 || ov->repeat_rate <= 0)
        return;

    ov->repeat_key = key;

    long delay_ns = (long)ov->repeat_delay * 1000000L;
    long rate_ns  = 1000000000L / ov->repeat_rate;

    struct itimerspec its = {
        .it_value    = { delay_ns / 1000000000L,
                         delay_ns % 1000000000L },
        .it_interval = { rate_ns / 1000000000L,
                         rate_ns % 1000000000L },
    };
    timerfd_settime(ov->repeat_fd, 0, &its, NULL);
}

static void handle_key_dispatch(struct overlay *ov,
                                uint32_t key) {
    if (!ov->xkb_state || !ov->cfg) return;

    xkb_keycode_t kc = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(
        ov->xkb_state, kc);
    uint32_t mods = xkb_mods_to_config(ov);

    log_debug("key: sym=0x%x mods=0x%x", sym, mods);

    const struct binding *b =
        config_find_binding(ov->cfg, sym, mods);
    if (!b) return;

    execute_commands(ov, ov->rs, b->commands, b->num_commands);
}

static void kbd_key(void *data, struct wl_keyboard *kbd,
                    uint32_t serial, uint32_t time,
                    uint32_t key, uint32_t state) {
    (void)kbd; (void)serial; (void)time;
    struct overlay *ov = data;

    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (key == ov->repeat_key)
            disarm_repeat(ov);
        return;
    }

    handle_key_dispatch(ov, key);
    if (!ov->running) return;

    /* Check if this key should repeat (xkb knows). */
    if (ov->xkb_keymap &&
        xkb_keymap_key_repeats(ov->xkb_keymap, key + 8))
        arm_repeat(ov, key);
    else
        disarm_repeat(ov);
}

static void kbd_modifiers(void *data, struct wl_keyboard *kbd,
                          uint32_t serial, uint32_t dep,
                          uint32_t lat, uint32_t locked,
                          uint32_t group) {
    (void)kbd; (void)serial;
    struct overlay *ov = data;
    if (ov->xkb_state) {
        xkb_state_update_mask(ov->xkb_state,
                              dep, lat, locked, 0, 0, group);
    }
}

static void kbd_repeat_info(void *data,
                            struct wl_keyboard *kbd,
                            int32_t rate, int32_t delay) {
    (void)kbd;
    struct overlay *ov = data;
    ov->repeat_rate  = rate;
    ov->repeat_delay = delay;
    log_debug("repeat info: rate=%d delay=%d", rate, delay);
}

static const struct wl_keyboard_listener kbd_listener = {
    .keymap      = kbd_keymap,
    .enter       = noop,
    .leave       = noop,
    .key         = kbd_key,
    .modifiers   = kbd_modifiers,
    .repeat_info = kbd_repeat_info,
};

/* ── Seat ───────────────────────────────────────────────── */

static void seat_caps(void *data, struct wl_seat *s,
                      uint32_t caps) {
    (void)s;
    struct overlay *ov = data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !ov->keyboard) {
        ov->keyboard = wl_seat_get_keyboard(ov->seat);
        wl_keyboard_add_listener(ov->keyboard,
                                 &kbd_listener, ov);
    }
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_caps,
    .name         = noop,
};

/* ── Modifier translation ───────────────────────────────── */

static uint32_t xkb_mods_to_config(struct overlay *ov) {
    uint32_t out = 0;
    if (!ov->xkb_state) return 0;

    if (xkb_state_mod_name_is_active(
            ov->xkb_state, XKB_MOD_NAME_SHIFT,
            XKB_STATE_MODS_DEPRESSED))
        out |= MOD_SHIFT;
    if (xkb_state_mod_name_is_active(
            ov->xkb_state, XKB_MOD_NAME_CTRL,
            XKB_STATE_MODS_DEPRESSED))
        out |= MOD_CTRL;
    if (xkb_state_mod_name_is_active(
            ov->xkb_state, XKB_MOD_NAME_ALT,
            XKB_STATE_MODS_DEPRESSED))
        out |= MOD_ALT;
    if (xkb_state_mod_name_is_active(
            ov->xkb_state, XKB_MOD_NAME_LOGO,
            XKB_STATE_MODS_DEPRESSED))
        out |= MOD_SUPER;

    return out;
}

/* ── Rendering ──────────────────────────────────────────── */

static int32_t get_scale_120(struct overlay *ov) {
    if (ov->frac_scale_v > 0)
        return ov->frac_scale_v;
    int32_t s = ov->out_scale > 0 ? ov->out_scale : 1;
    return s * 120;
}

static void send_frame(struct overlay *ov) {
    if (!ov->configured || !ov->rs) return;

    int32_t scale_120 = get_scale_120(ov);
    uint32_t bw = ov->surf_width  * (uint32_t)scale_120 / 120;
    uint32_t bh = ov->surf_height * (uint32_t)scale_120 / 120;

    struct shm_buffer *b = buf_get(ov->shm, &ov->pool, bw, bh);
    if (!b) return;
    b->state = BUF_BUSY;

    cairo_t *cr = b->cr;
    cairo_identity_matrix(cr);
    cairo_scale(cr, scale_120 / 120.0, scale_120 / 120.0);

    /* Clear to transparent. */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);

    render_grid(ov, cr, ov->rs);

    wl_surface_set_buffer_scale(ov->surface, 1);
    wl_surface_attach(ov->surface, b->wl_buf, 0, 0);
    if (ov->viewport) {
        wp_viewport_set_destination(
            ov->viewport,
            (int32_t)ov->surf_width, (int32_t)ov->surf_height);
    }
    wl_surface_damage(ov->surface, 0, 0,
                      (int32_t)ov->surf_width,
                      (int32_t)ov->surf_height);
    wl_surface_commit(ov->surface);
}

static void render_grid(struct overlay *ov, cairo_t *cr,
                        struct region_state *rs) {
    (void)ov;
    int x = rs->current.x;
    int y = rs->current.y;
    int w = rs->current.w;
    int h = rs->current.h;
    int cols = rs->current.grid_cols;
    int rows = rs->current.grid_rows;

    /* Outer rectangle and grid lines — same weight. */
    cairo_set_source_rgba(cr, 0.4, 0.6, 1.0, 0.5);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1, h - 1);
    cairo_stroke(cr);

    /* Vertical lines. */
    for (int c = 1; c < cols; c++) {
        double lx = x + (double)w * c / cols;
        cairo_move_to(cr, lx, y);
        cairo_line_to(cr, lx, y + h);
    }

    /* Horizontal lines. */
    for (int r = 1; r < rows; r++) {
        double ly = y + (double)h * r / rows;
        cairo_move_to(cr, x, ly);
        cairo_line_to(cr, x + w, ly);
    }
    cairo_stroke(cr);

}

/* ── Frame callback ─────────────────────────────────────── */

static void frame_done(void *data, struct wl_callback *cb,
                       uint32_t time) {
    (void)time;
    struct overlay *ov = data;
    wl_callback_destroy(cb);
    ov->frame_cb = NULL;
    send_frame(ov);
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static void request_frame(struct overlay *ov) {
    if (ov->frame_cb) return;
    ov->frame_cb = wl_surface_frame(ov->surface);
    wl_callback_add_listener(ov->frame_cb,
                             &frame_listener, ov);
    wl_surface_commit(ov->surface);
}

/* ── Public API ─────────────────────────────────────────── */

struct overlay *overlay_create(void) {
    struct overlay *ov = calloc(1, sizeof(*ov));
    if (!ov) return NULL;

    ov->out_scale = 1;
    ov->repeat_fd = timerfd_create(CLOCK_MONOTONIC,
                                   TFD_NONBLOCK | TFD_CLOEXEC);
    ov->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ov->xkb_ctx) {
        free(ov);
        return NULL;
    }

    ov->display = wl_display_connect(NULL);
    if (!ov->display) {
        log_err("failed to connect to Wayland compositor");
        xkb_context_unref(ov->xkb_ctx);
        free(ov);
        return NULL;
    }

    ov->registry = wl_display_get_registry(ov->display);
    wl_registry_add_listener(ov->registry,
                             &registry_listener, ov);
    wl_display_roundtrip(ov->display);

    /* Validate required globals. */
    if (!ov->compositor) {
        log_err("missing wl_compositor");
        goto fail;
    }
    if (!ov->shm) {
        log_err("missing wl_shm");
        goto fail;
    }
    if (!ov->layer_shell) {
        log_err("missing zwlr_layer_shell_v1");
        goto fail;
    }
    if (!ov->vptr_mgr) {
        log_err("missing zwlr_virtual_pointer_manager_v1");
        goto fail;
    }
    if (!ov->seat) {
        log_err("missing wl_seat");
        goto fail;
    }

    /* Seat listener -> keyboard. */
    wl_seat_add_listener(ov->seat, &seat_listener, ov);

    /* Output listener (integer scale). */
    if (ov->wl_output) {
        wl_output_add_listener(ov->wl_output,
                               &output_listener, ov);
    }

    /* XDG output for logical geometry. */
    if (ov->xdg_out_mgr && ov->wl_output) {
        struct zxdg_output_v1 *xo =
            zxdg_output_manager_v1_get_xdg_output(
                ov->xdg_out_mgr, ov->wl_output);
        zxdg_output_v1_add_listener(xo, &xdg_out_listener, ov);
    }

    wl_display_roundtrip(ov->display);

    log_debug("output: %dx%d+%d+%d scale=%d",
              ov->out_width, ov->out_height,
              ov->out_x, ov->out_y, ov->out_scale);

    /* Create layer surface. */
    ov->surface = wl_compositor_create_surface(ov->compositor);

    ov->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        ov->layer_shell, ov->surface, ov->wl_output,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "waynav");
    zwlr_layer_surface_v1_add_listener(
        ov->layer_surface, &layer_listener, ov);
    zwlr_layer_surface_v1_set_exclusive_zone(
        ov->layer_surface, -1);
    zwlr_layer_surface_v1_set_anchor(ov->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        ov->layer_surface, true);

    /* Fractional scale. */
    if (ov->frac_scale_mgr) {
        ov->frac_scale =
            wp_fractional_scale_manager_v1_get_fractional_scale(
                ov->frac_scale_mgr, ov->surface);
        wp_fractional_scale_v1_add_listener(
            ov->frac_scale, &frac_listener, ov);
    }

    /* Viewport for fractional scale destination. */
    if (ov->viewporter) {
        ov->viewport = wp_viewporter_get_viewport(
            ov->viewporter, ov->surface);
    }

    /* Empty input region — mouse passes through overlay. */
    ov->input_region =
        wl_compositor_create_region(ov->compositor);
    wl_region_add(ov->input_region, 0, 0, 0, 0);
    wl_surface_set_input_region(ov->surface, ov->input_region);

    wl_surface_commit(ov->surface);

    /* Roundtrip to get configure. */
    wl_display_roundtrip(ov->display);

    /* Create virtual pointer. */
    ov->vptr =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
            ov->vptr_mgr, ov->seat, ov->wl_output);

    log_info("overlay created: %ux%u",
             ov->surf_width, ov->surf_height);
    return ov;

fail:
    overlay_destroy(ov);
    return NULL;
}

void overlay_destroy(struct overlay *ov) {
    if (!ov) return;

    if (ov->repeat_fd >= 0)
        close(ov->repeat_fd);

    if (ov->vptr)
        zwlr_virtual_pointer_v1_destroy(ov->vptr);
    if (ov->frame_cb)
        wl_callback_destroy(ov->frame_cb);
    if (ov->viewport)
        wp_viewport_destroy(ov->viewport);
    if (ov->frac_scale)
        wp_fractional_scale_v1_destroy(ov->frac_scale);

    buf_destroy(&ov->pool.bufs[0]);
    buf_destroy(&ov->pool.bufs[1]);

    if (ov->layer_surface)
        zwlr_layer_surface_v1_destroy(ov->layer_surface);
    if (ov->surface)
        wl_surface_destroy(ov->surface);
    if (ov->input_region)
        wl_region_destroy(ov->input_region);

    if (ov->keyboard)
        wl_keyboard_destroy(ov->keyboard);
    if (ov->xkb_state)
        xkb_state_unref(ov->xkb_state);
    if (ov->xkb_keymap)
        xkb_keymap_unref(ov->xkb_keymap);
    if (ov->xkb_ctx)
        xkb_context_unref(ov->xkb_ctx);

    if (ov->frac_scale_mgr)
        wp_fractional_scale_manager_v1_destroy(
            ov->frac_scale_mgr);
    if (ov->viewporter)
        wp_viewporter_destroy(ov->viewporter);
    if (ov->vptr_mgr)
        zwlr_virtual_pointer_manager_v1_destroy(ov->vptr_mgr);
    if (ov->xdg_out_mgr)
        zxdg_output_manager_v1_destroy(ov->xdg_out_mgr);
    if (ov->layer_shell)
        zwlr_layer_shell_v1_destroy(ov->layer_shell);
    if (ov->seat)
        wl_seat_destroy(ov->seat);
    if (ov->wl_output)
        wl_output_destroy(ov->wl_output);
    if (ov->shm)
        wl_shm_destroy(ov->shm);
    if (ov->compositor)
        wl_compositor_destroy(ov->compositor);
    if (ov->registry)
        wl_registry_destroy(ov->registry);
    if (ov->display) {
        wl_display_roundtrip(ov->display);
        wl_display_disconnect(ov->display);
    }

    free(ov);
}

void overlay_redraw(struct overlay *ov, struct region_state *rs) {
    if (!ov) return;
    ov->rs = rs;
    request_frame(ov);
}

int overlay_get_width(struct overlay *ov) {
    if (!ov) return 0;
    /* Prefer xdg-output logical size, fall back to surface. */
    if (ov->out_width > 0) return ov->out_width;
    return (int)ov->surf_width;
}

int overlay_get_height(struct overlay *ov) {
    if (!ov) return 0;
    if (ov->out_height > 0) return ov->out_height;
    return (int)ov->surf_height;
}

void overlay_stop(struct overlay *ov) {
    if (ov) ov->running = false;
}

int overlay_run(struct overlay *ov, struct config *cfg,
                struct region_state *rs) {
    if (!ov) return -1;
    ov->cfg     = cfg;
    ov->rs      = rs;
    ov->running = true;

    /* Initial frame. */
    send_frame(ov);

    int wl_fd = wl_display_get_fd(ov->display);

    struct pollfd fds[2];
    fds[0].fd     = wl_fd;
    fds[0].events = POLLIN;
    fds[1].fd     = ov->repeat_fd;
    fds[1].events = POLLIN;
    int nfds = ov->repeat_fd >= 0 ? 2 : 1;

    while (ov->running) {
        /* Flush outgoing requests before blocking. */
        while (wl_display_prepare_read(ov->display) != 0)
            wl_display_dispatch_pending(ov->display);
        wl_display_flush(ov->display);

        if (poll(fds, (nfds_t)nfds, -1) < 0) {
            wl_display_cancel_read(ov->display);
            if (errno == EINTR) continue;
            log_err("poll failed: %s", strerror(errno));
            return -1;
        }

        /* Wayland events. */
        if (fds[0].revents & POLLIN) {
            wl_display_read_events(ov->display);
        } else {
            wl_display_cancel_read(ov->display);
        }
        wl_display_dispatch_pending(ov->display);

        /* Key repeat timer. */
        if (nfds > 1 && (fds[1].revents & POLLIN)) {
            uint64_t expirations;
            if (read(ov->repeat_fd, &expirations,
                     sizeof(expirations)) > 0 &&
                ov->repeat_key != 0) {
                handle_key_dispatch(ov, ov->repeat_key);
            }
        }
    }

    return 0;
}

/* ── Virtual pointer ────────────────────────────────────── */

void vptr_warp(struct overlay *ov, int x, int y) {
    if (!ov || !ov->vptr) return;

    uint32_t ow = (uint32_t)overlay_get_width(ov);
    uint32_t oh = (uint32_t)overlay_get_height(ov);

    log_debug("vptr warp: %d,%d in %ux%u", x, y, ow, oh);

    zwlr_virtual_pointer_v1_motion_absolute(
        ov->vptr, 0, (uint32_t)x, (uint32_t)y, ow, oh);
    zwlr_virtual_pointer_v1_frame(ov->vptr);
    wl_display_roundtrip(ov->display);
}

/* Map keynav button numbers to Linux input event codes. */
static uint32_t keynav_btn(int button) {
    switch (button) {
    case 1: return BTN_LEFT;
    case 2: return BTN_MIDDLE;
    case 3: return BTN_RIGHT;
    default: return 0;
    }
}

void vptr_click(struct overlay *ov, int button) {
    if (!ov || !ov->vptr) return;

    /* Buttons 4/5 are scroll up/down. */
    if (button == 4 || button == 5) {
        /* Axis scroll: 4=up (negative), 5=down (positive).
         * Value 15 is ~one notch. */
        int32_t dir = (button == 5) ? 15 : -15;
        zwlr_virtual_pointer_v1_axis(
            ov->vptr, 0, 0 /* vertical */,
            wl_fixed_from_int(dir));
        zwlr_virtual_pointer_v1_frame(ov->vptr);
        wl_display_roundtrip(ov->display);
        return;
    }

    uint32_t btn = keynav_btn(button);
    if (!btn) return;

    zwlr_virtual_pointer_v1_button(
        ov->vptr, 0, btn,
        WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(ov->vptr);
    wl_display_roundtrip(ov->display);

    zwlr_virtual_pointer_v1_button(
        ov->vptr, 0, btn,
        WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(ov->vptr);
    wl_display_roundtrip(ov->display);
}

void vptr_button_down(struct overlay *ov, int button) {
    if (!ov || !ov->vptr) return;
    uint32_t btn = keynav_btn(button);
    if (!btn) return;

    zwlr_virtual_pointer_v1_button(
        ov->vptr, 0, btn,
        WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(ov->vptr);
    wl_display_roundtrip(ov->display);
}

void vptr_button_up(struct overlay *ov, int button) {
    if (!ov || !ov->vptr) return;
    uint32_t btn = keynav_btn(button);
    if (!btn) return;

    zwlr_virtual_pointer_v1_button(
        ov->vptr, 0, btn,
        WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(ov->vptr);
    wl_display_roundtrip(ov->display);
}
