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

enum buf_state {
    BUF_UNINIT = 0,
    BUF_READY = 1,
    BUF_BUSY = 2,
};

struct shm_buffer {
    enum buf_state state;
    struct wl_buffer *wl_buf;
    cairo_surface_t *cairo_surface;
    cairo_t *cr;
    void *data;
    size_t data_size;
    uint32_t width;
    uint32_t height;
};

struct buffer_pool {
    struct shm_buffer bufs[2];
};

struct output {
    uint32_t global_name;
    struct wl_output *wl_output;
    struct zxdg_output_v1 *xdg_output;
    int32_t width;
    int32_t height;
    int32_t x;
    int32_t y;
    int32_t scale;
    char *name;
    struct output *next;
};

static int create_shm_file(size_t size) {
    char name[] = "/tmp/waynav-shm-XXXXXX";
    int fd = mkostemp(name, O_CLOEXEC);
    if (fd < 0)
        return -1;
    unlink(name);
    int err;
    while ((err = ftruncate(fd, (off_t)size)) && errno == EINTR)
        ;
    if (err) {
        close(fd);
        return -1;
    }
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
    if (b->state == BUF_UNINIT)
        return;
    if (b->cr)
        cairo_destroy(b->cr);
    if (b->cairo_surface)
        cairo_surface_destroy(b->cairo_surface);
    if (b->wl_buf)
        wl_buffer_destroy(b->wl_buf);
    if (b->data)
        munmap(b->data, b->data_size);
    memset(b, 0, sizeof(*b));
}

static struct shm_buffer *buf_get(struct wl_shm *shm, struct buffer_pool *pool,
                                  uint32_t w, uint32_t h) {
    struct shm_buffer *b = NULL;
    for (int i = 0; i < 2; i++) {
        if (pool->bufs[i].state != BUF_BUSY) {
            b = &pool->bufs[i];
            break;
        }
    }
    if (!b)
        return NULL;

    if (b->width != w || b->height != h)
        buf_destroy(b);

    if (b->state == BUF_UNINIT) {
        uint32_t stride = (uint32_t)cairo_format_stride_for_width(
            CAIRO_FORMAT_ARGB32, (int)w);
        size_t sz = (size_t)h * stride;
        int fd = create_shm_file(sz);
        if (fd < 0)
            return NULL;

        void *data = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            close(fd);
            return NULL;
        }

        struct wl_shm_pool *pool_wl = wl_shm_create_pool(shm, fd, (int32_t)sz);
        b->wl_buf =
            wl_shm_pool_create_buffer(pool_wl, 0, (int32_t)w, (int32_t)h,
                                      (int32_t)stride, WL_SHM_FORMAT_ARGB8888);
        wl_buffer_add_listener(b->wl_buf, &buf_listener, b);
        wl_shm_pool_destroy(pool_wl);
        close(fd);

        b->data = data;
        b->data_size = sz;
        b->width = w;
        b->height = h;
        b->state = BUF_READY;
        b->cairo_surface = cairo_image_surface_create_for_data(
            b->data, CAIRO_FORMAT_ARGB32, (int)w, (int)h, (int)stride);
        b->cr = cairo_create(b->cairo_surface);
    }
    return b;
}

struct overlay {
    /* Wayland globals */
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zwlr_virtual_pointer_manager_v1 *vptr_mgr;
    struct zxdg_output_manager_v1 *xdg_out_mgr;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *frac_scale_mgr;

    /* Objects */
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_callback *frame_cb;
    struct wp_viewport *viewport;
    struct wp_fractional_scale_v1 *frac_scale;
    struct wl_region *input_region;
    struct zwlr_virtual_pointer_v1 *vptr;

    /* Input */
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    int cursor_x;
    int cursor_y;
    bool cursor_position_known;

    /* Keyboard / xkb */
    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;

    /* Outputs */
    struct output *outputs;
    struct output *selected_output;
    int32_t frac_scale_v; /* scale*120, 0 if unavailable */

    /* Surface */
    uint32_t surf_width;
    uint32_t surf_height;
    bool configured;

    /* Rendering */
    struct buffer_pool pool;

    /* Key repeat */
    int repeat_fd;        /* timerfd */
    int32_t repeat_rate;  /* keys per second */
    int32_t repeat_delay; /* ms before first repeat */
    uint32_t repeat_key;  /* evdev code of held key, 0=none */

    /* State pointers (set during overlay_run) */
    struct config *cfg;
    struct region_state *rs;
    bool running;
};

static void send_frame(struct overlay *ov);
static void render_grid(struct overlay *ov, cairo_t *cr,
                        struct region_state *rs);
static uint32_t xkb_mods_to_config(struct overlay *ov);

static void noop() {
}

static uint32_t negotiated_version(uint32_t advertised_version,
                                   uint32_t supported_version) {
    if (advertised_version < supported_version)
        return advertised_version;
    return supported_version;
}

static const char *output_name_or_unknown(const struct output *output) {
    return output->name ? output->name : "<unknown>";
}

static void set_output_name(struct output *output, const char *name) {
    char *copy = strdup(name);
    if (!copy)
        return;

    free(output->name);
    output->name = copy;
}

static void output_scale(void *data, struct wl_output *wl_output,
                         int32_t factor) {
    (void)wl_output;
    struct output *output = data;
    output->scale = factor;
}

static void output_name(void *data, struct wl_output *wl_output,
                        const char *name) {
    (void)wl_output;
    set_output_name(data, name);
}

static const struct wl_output_listener output_listener = {
    .geometry = noop,
    .mode = noop,
    .done = noop,
    .scale = output_scale,
    .name = output_name,
    .description = noop,
};

static void xdg_output_logical_position(void *data,
                                        struct zxdg_output_v1 *xdg_output,
                                        int32_t x, int32_t y) {
    (void)xdg_output;
    struct output *output = data;
    output->x = x;
    output->y = y;
}

static void xdg_output_logical_size(void *data,
                                    struct zxdg_output_v1 *xdg_output,
                                    int32_t width, int32_t height) {
    (void)xdg_output;
    struct output *output = data;
    output->width = width;
    output->height = height;
}

static void xdg_output_name(void *data, struct zxdg_output_v1 *xdg_output,
                            const char *name) {
    (void)xdg_output;
    set_output_name(data, name);
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = xdg_output_logical_position,
    .logical_size = xdg_output_logical_size,
    .done = noop,
    .name = xdg_output_name,
    .description = noop,
};

static void create_xdg_output(struct overlay *ov, struct output *output) {
    if (!ov->xdg_out_mgr || output->xdg_output)
        return;

    output->xdg_output = zxdg_output_manager_v1_get_xdg_output(
        ov->xdg_out_mgr, output->wl_output);
    zxdg_output_v1_add_listener(output->xdg_output, &xdg_output_listener,
                                output);
}

static void create_xdg_outputs(struct overlay *ov) {
    for (struct output *output = ov->outputs; output; output = output->next)
        create_xdg_output(ov, output);
}

static void output_destroy(struct output *output) {
    if (output->xdg_output)
        zxdg_output_v1_destroy(output->xdg_output);
    wl_output_destroy(output->wl_output);
    free(output->name);
    free(output);
}

static void bind_output(struct overlay *ov, struct wl_registry *registry,
                        uint32_t name, uint32_t version) {
    struct output *output = calloc(1, sizeof(*output));
    if (!output) {
        log_err("failed to allocate wl_output");
        return;
    }

    output->global_name = name;
    output->scale = 1;
    output->wl_output = wl_registry_bind(registry, name, &wl_output_interface,
                                         negotiated_version(version, 4));
    wl_output_add_listener(output->wl_output, &output_listener, output);
    output->next = ov->outputs;
    ov->outputs = output;
    create_xdg_output(ov, output);
}

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
    struct overlay *ov = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        ov->compositor =
            wl_registry_bind(registry, name, &wl_compositor_interface,
                             negotiated_version(version, 4));
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        ov->shm = wl_registry_bind(registry, name, &wl_shm_interface,
                                   negotiated_version(version, 1));
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        if (!ov->seat) {
            ov->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                        negotiated_version(version, 7));
        }
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        bind_output(ov, registry, name, version);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        uint32_t layer_shell_version = version;
        if (layer_shell_version > ZWLR_LAYER_SHELL_V1_DESTROY_SINCE_VERSION)
            layer_shell_version = ZWLR_LAYER_SHELL_V1_DESTROY_SINCE_VERSION;
        ov->layer_shell =
            wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface,
                             layer_shell_version);
    } else if (strcmp(interface,
                      zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        ov->vptr_mgr = wl_registry_bind(
            registry, name, &zwlr_virtual_pointer_manager_v1_interface,
            negotiated_version(version, 2));
    } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        ov->xdg_out_mgr =
            wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface,
                             negotiated_version(version, 2));
        create_xdg_outputs(ov);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        ov->viewporter =
            wl_registry_bind(registry, name, &wp_viewporter_interface,
                             negotiated_version(version, 1));
    } else if (strcmp(interface,
                      wp_fractional_scale_manager_v1_interface.name) == 0) {
        ov->frac_scale_mgr = wl_registry_bind(
            registry, name, &wp_fractional_scale_manager_v1_interface,
            negotiated_version(version, 1));
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {
    (void)registry;
    struct overlay *ov = data;
    struct output **link = &ov->outputs;

    while (*link) {
        struct output *output = *link;
        if (output->global_name == name) {
            *link = output->next;
            if (ov->selected_output == output)
                ov->selected_output = NULL;
            output_destroy(output);
            return;
        }
        link = &output->next;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static struct output *find_output(const struct overlay *ov,
                                  const struct wl_output *wl_output) {
    for (struct output *output = ov->outputs; output; output = output->next) {
        if (output->wl_output == wl_output)
            return output;
    }
    return NULL;
}

static void surface_enter(void *data, struct wl_surface *surface,
                          struct wl_output *wl_output) {
    (void)surface;
    struct overlay *ov = data;
    struct output *output = find_output(ov, wl_output);
    if (!output) {
        log_warn("surface entered an unknown output");
        return;
    }

    ov->selected_output = output;
    log_debug("surface entered output %s: %dx%d+%d+%d scale=%d",
              output_name_or_unknown(output), output->width, output->height,
              output->x, output->y, output->scale);
}

static void surface_leave(void *data, struct wl_surface *surface,
                          struct wl_output *wl_output) {
    (void)surface;
    struct overlay *ov = data;
    if (ov->selected_output && ov->selected_output->wl_output == wl_output) {
        ov->selected_output = NULL;
    }
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                            uint32_t serial, uint32_t w, uint32_t h) {
    struct overlay *ov = data;
    ov->surf_width = w;
    ov->surf_height = h;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    ov->configured = true;
    log_debug("layer configure: %ux%u", w, h);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    (void)ls;
    struct overlay *ov = data;
    ov->running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void frac_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                           uint32_t scale) {
    (void)fs;
    struct overlay *ov = data;
    ov->frac_scale_v = (int32_t)scale;
    log_debug("fractional scale: %u/120 = %.2f", scale, scale / 120.0);
}

static const struct wp_fractional_scale_v1_listener frac_listener = {
    .preferred_scale = frac_preferred,
};

static void kbd_keymap(void *data, struct wl_keyboard *kbd, uint32_t fmt,
                       int fd, uint32_t size) {
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
        void *buf = mmap(NULL, size - 1, PROT_READ, MAP_PRIVATE, fd, 0);
        if (buf != MAP_FAILED) {
            ov->xkb_keymap = xkb_keymap_new_from_buffer(
                ov->xkb_ctx, buf, size - 1, XKB_KEYMAP_FORMAT_TEXT_V1,
                XKB_KEYMAP_COMPILE_NO_FLAGS);
            munmap(buf, size - 1);
        }
    }
    close(fd);

    if (!ov->xkb_keymap) {
        ov->xkb_keymap = xkb_keymap_new_from_names(ov->xkb_ctx, NULL,
                                                   XKB_KEYMAP_COMPILE_NO_FLAGS);
    }
    ov->xkb_state = xkb_state_new(ov->xkb_keymap);
    log_debug("keyboard keymap loaded");
}

static void disarm_repeat(struct overlay *ov) {
    if (ov->repeat_fd < 0)
        return;
    struct itimerspec its = {0};
    timerfd_settime(ov->repeat_fd, 0, &its, NULL);
    ov->repeat_key = 0;
}

static void arm_repeat(struct overlay *ov, uint32_t key) {
    if (ov->repeat_fd < 0 || ov->repeat_rate <= 0)
        return;

    ov->repeat_key = key;

    long delay_ns = (long)ov->repeat_delay * 1000000L;
    long rate_ns = 1000000000L / ov->repeat_rate;

    struct itimerspec its = {
        .it_value = {delay_ns / 1000000000L, delay_ns % 1000000000L},
        .it_interval = {rate_ns / 1000000000L, rate_ns % 1000000000L},
    };
    timerfd_settime(ov->repeat_fd, 0, &its, NULL);
}

static void handle_key_dispatch(struct overlay *ov, uint32_t key) {
    if (!ov->xkb_state || !ov->cfg)
        return;

    xkb_keycode_t kc = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(ov->xkb_state, kc);
    uint32_t mods = xkb_mods_to_config(ov);

    log_debug("key: sym=0x%x mods=0x%x", sym, mods);

    const struct binding *b = config_find_binding(ov->cfg, sym, mods);
    if (!b)
        return;

    execute_commands(ov, ov->rs, b->commands, b->num_commands);
}

static void kbd_key(void *data, struct wl_keyboard *kbd, uint32_t serial,
                    uint32_t time, uint32_t key, uint32_t state) {
    (void)kbd;
    (void)serial;
    (void)time;
    struct overlay *ov = data;

    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (key == ov->repeat_key)
            disarm_repeat(ov);
        return;
    }

    handle_key_dispatch(ov, key);
    if (!ov->running)
        return;

    if (ov->xkb_keymap && xkb_keymap_key_repeats(ov->xkb_keymap, key + 8))
        arm_repeat(ov, key);
    else
        disarm_repeat(ov);
}

static void kbd_modifiers(void *data, struct wl_keyboard *kbd, uint32_t serial,
                          uint32_t dep, uint32_t lat, uint32_t locked,
                          uint32_t group) {
    (void)kbd;
    (void)serial;
    struct overlay *ov = data;
    if (ov->xkb_state) {
        xkb_state_update_mask(ov->xkb_state, dep, lat, locked, 0, 0, group);
    }
}

static void kbd_repeat_info(void *data, struct wl_keyboard *kbd, int32_t rate,
                            int32_t delay) {
    (void)kbd;
    struct overlay *ov = data;
    ov->repeat_rate = rate;
    ov->repeat_delay = delay;
    log_debug("repeat info: rate=%d delay=%d", rate, delay);
}

static const struct wl_keyboard_listener kbd_listener = {
    .keymap = kbd_keymap,
    .enter = noop,
    .leave = noop,
    .key = kbd_key,
    .modifiers = kbd_modifiers,
    .repeat_info = kbd_repeat_info,
};

static void save_cursor_position(struct overlay *ov, wl_fixed_t surface_x,
                                 wl_fixed_t surface_y) {
    ov->cursor_x = wl_fixed_to_int(surface_x);
    ov->cursor_y = wl_fixed_to_int(surface_y);
    ov->cursor_position_known = true;
    log_debug("pointer at %d,%d", ov->cursor_x, ov->cursor_y);
}

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t surface_x, wl_fixed_t surface_y) {
    (void)pointer;
    (void)serial;
    struct overlay *ov = data;
    if (surface == ov->surface)
        save_cursor_position(ov, surface_x, surface_y);
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t surface_x,
                           wl_fixed_t surface_y) {
    (void)pointer;
    (void)time;
    save_cursor_position(data, surface_x, surface_y);
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = noop,
    .motion = pointer_motion,
    .button = noop,
    .axis = noop,
    .frame = noop,
    .axis_source = noop,
    .axis_stop = noop,
    .axis_discrete = noop,
};

static void seat_caps(void *data, struct wl_seat *s, uint32_t caps) {
    (void)s;
    struct overlay *ov = data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !ov->keyboard) {
        ov->keyboard = wl_seat_get_keyboard(ov->seat);
        wl_keyboard_add_listener(ov->keyboard, &kbd_listener, ov);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ov->pointer) {
        ov->pointer = wl_seat_get_pointer(ov->seat);
        wl_pointer_add_listener(ov->pointer, &pointer_listener, ov);
    }
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_caps,
    .name = noop,
};

static uint32_t xkb_mods_to_config(struct overlay *ov) {
    uint32_t out = 0;
    if (!ov->xkb_state)
        return 0;

    if (xkb_state_mod_name_is_active(ov->xkb_state, XKB_MOD_NAME_SHIFT,
                                     XKB_STATE_MODS_DEPRESSED))
        out |= MOD_SHIFT;
    if (xkb_state_mod_name_is_active(ov->xkb_state, XKB_MOD_NAME_CTRL,
                                     XKB_STATE_MODS_DEPRESSED))
        out |= MOD_CTRL;
    if (xkb_state_mod_name_is_active(ov->xkb_state, XKB_MOD_NAME_ALT,
                                     XKB_STATE_MODS_DEPRESSED))
        out |= MOD_ALT;
    if (xkb_state_mod_name_is_active(ov->xkb_state, XKB_MOD_NAME_LOGO,
                                     XKB_STATE_MODS_DEPRESSED))
        out |= MOD_SUPER;

    return out;
}

static int32_t get_scale_120(struct overlay *ov) {
    if (ov->frac_scale_v > 0)
        return ov->frac_scale_v;
    if (ov->selected_output && ov->selected_output->scale > 0)
        return ov->selected_output->scale * 120;
    return 120;
}

static void clear_buffer(struct shm_buffer *buffer) {
    cairo_identity_matrix(buffer->cr);
    cairo_set_operator(buffer->cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(buffer->cr, 0, 0, 0, 0);
    cairo_paint(buffer->cr);
}

static void commit_buffer(struct overlay *ov, struct shm_buffer *buffer) {
    buffer->state = BUF_BUSY;
    wl_surface_set_buffer_scale(ov->surface, 1);
    wl_surface_attach(ov->surface, buffer->wl_buf, 0, 0);
    if (ov->viewport) {
        wp_viewport_set_destination(ov->viewport, (int32_t)ov->surf_width,
                                    (int32_t)ov->surf_height);
    }
    wl_surface_damage(ov->surface, 0, 0, (int32_t)ov->surf_width,
                      (int32_t)ov->surf_height);
    wl_surface_commit(ov->surface);
}

static bool map_transparent_surface(struct overlay *ov) {
    if (!ov->configured || ov->surf_width == 0 || ov->surf_height == 0)
        return false;

    struct shm_buffer *buffer =
        buf_get(ov->shm, &ov->pool, ov->surf_width, ov->surf_height);
    if (!buffer)
        return false;

    clear_buffer(buffer);
    commit_buffer(ov, buffer);
    return true;
}

static void send_frame(struct overlay *ov) {
    if (!ov->configured || !ov->rs)
        return;

    int32_t scale_120 = get_scale_120(ov);
    uint32_t buffer_width = ov->surf_width * (uint32_t)scale_120 / 120;
    uint32_t buffer_height = ov->surf_height * (uint32_t)scale_120 / 120;

    struct shm_buffer *buffer =
        buf_get(ov->shm, &ov->pool, buffer_width, buffer_height);
    if (!buffer)
        return;

    clear_buffer(buffer);
    cairo_scale(buffer->cr, scale_120 / 120.0, scale_120 / 120.0);
    render_grid(ov, buffer->cr, ov->rs);
    commit_buffer(ov, buffer);
}

/* Set the cairo source to a packed 0xRRGGBBAA color, scaling each
 * channel to cairo's 0..1 range. */
static void set_source_color(cairo_t *cr, uint32_t packed) {
    cairo_set_source_rgba(
        cr, ((packed >> 24) & 0xff) / 255.0, ((packed >> 16) & 0xff) / 255.0,
        ((packed >> 8) & 0xff) / 255.0, (packed & 0xff) / 255.0);
}

static void render_grid(struct overlay *ov, cairo_t *cr,
                        struct region_state *rs) {
    int x = rs->current.x;
    int y = rs->current.y;
    int w = rs->current.w;
    int h = rs->current.h;
    int cols = rs->current.grid_cols;
    int rows = rs->current.grid_rows;

    if (w <= 0 || h <= 0)
        return;

    double line_width = ov->cfg->line_width;
    int line_width_max = w < h ? w : h;
    if (line_width > line_width_max)
        line_width = line_width_max;
    double line_inset = line_width / 2.0;

    set_source_color(cr, ov->cfg->region_bg);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);

    cairo_set_line_width(cr, line_width);
    set_source_color(cr, ov->cfg->grid_color);
    cairo_rectangle(cr, x + line_inset, y + line_inset, w - line_width,
                    h - line_width);
    cairo_stroke(cr);

    for (int c = 1; c < cols; c++) {
        double lx = x + (double)w * c / cols;
        cairo_move_to(cr, lx, y);
        cairo_line_to(cr, lx, y + h);
    }

    for (int r = 1; r < rows; r++) {
        double ly = y + (double)h * r / rows;
        cairo_move_to(cr, x, ly);
        cairo_line_to(cr, x + w, ly);
    }
    cairo_stroke(cr);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
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
    if (ov->frame_cb)
        return;
    ov->frame_cb = wl_surface_frame(ov->surface);
    wl_callback_add_listener(ov->frame_cb, &frame_listener, ov);
    wl_surface_commit(ov->surface);
}

static bool required_globals_available(const struct overlay *ov) {
    if (!ov->compositor) {
        log_err("missing wl_compositor");
        return false;
    }
    if (!ov->shm) {
        log_err("missing wl_shm");
        return false;
    }
    if (!ov->layer_shell) {
        log_err("missing zwlr_layer_shell_v1");
        return false;
    }
    if (!ov->vptr_mgr) {
        log_err("missing zwlr_virtual_pointer_manager_v1");
        return false;
    }
    if (zwlr_virtual_pointer_manager_v1_get_version(ov->vptr_mgr) <
        ZWLR_VIRTUAL_POINTER_MANAGER_V1_CREATE_VIRTUAL_POINTER_WITH_OUTPUT_SINCE_VERSION) {
        log_err("zwlr_virtual_pointer_manager_v1 version 2 is required");
        return false;
    }
    if (!ov->seat) {
        log_err("missing wl_seat");
        return false;
    }
    if (!ov->outputs) {
        log_err("missing wl_output");
        return false;
    }
    return true;
}

static void log_outputs(const struct overlay *ov) {
    for (const struct output *output = ov->outputs; output;
         output = output->next) {
        log_debug("output %s: %dx%d+%d+%d scale=%d",
                  output_name_or_unknown(output), output->width, output->height,
                  output->x, output->y, output->scale);
    }
}

static bool create_overlay_surface(struct overlay *ov) {
    ov->surface = wl_compositor_create_surface(ov->compositor);
    wl_surface_add_listener(ov->surface, &surface_listener, ov);

    ov->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        ov->layer_shell, ov->surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        "waynav");
    zwlr_layer_surface_v1_add_listener(ov->layer_surface, &layer_listener, ov);
    zwlr_layer_surface_v1_set_exclusive_zone(ov->layer_surface, -1);
    zwlr_layer_surface_v1_set_anchor(ov->layer_surface,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
    zwlr_layer_surface_v1_set_keyboard_interactivity(ov->layer_surface, true);

    if (ov->frac_scale_mgr) {
        ov->frac_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            ov->frac_scale_mgr, ov->surface);
        wp_fractional_scale_v1_add_listener(ov->frac_scale, &frac_listener, ov);
    }
    if (ov->viewporter)
        ov->viewport = wp_viewporter_get_viewport(ov->viewporter, ov->surface);

    /* Keep pointer input on the windows beneath the overlay. */
    ov->input_region = wl_compositor_create_region(ov->compositor);
    wl_surface_set_input_region(ov->surface, ov->input_region);
    wl_surface_commit(ov->surface);

    wl_display_roundtrip(ov->display);
    if (!map_transparent_surface(ov)) {
        log_err("failed to map overlay surface");
        return false;
    }

    /* Wait for wl_surface.enter to identify the compositor-selected output. */
    wl_display_roundtrip(ov->display);
    if (!ov->selected_output) {
        log_err("compositor did not select an output for the overlay");
        return false;
    }
    return true;
}

struct overlay *overlay_create(void) {
    struct overlay *ov = calloc(1, sizeof(*ov));
    if (!ov)
        return NULL;

    ov->repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    ov->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ov->xkb_ctx)
        goto fail;

    ov->display = wl_display_connect(NULL);
    if (!ov->display) {
        log_err("failed to connect to Wayland compositor");
        goto fail;
    }

    ov->registry = wl_display_get_registry(ov->display);
    wl_registry_add_listener(ov->registry, &registry_listener, ov);
    wl_display_roundtrip(ov->display);

    if (!required_globals_available(ov))
        goto fail;

    wl_seat_add_listener(ov->seat, &seat_listener, ov);
    wl_display_roundtrip(ov->display);
    log_outputs(ov);

    if (!create_overlay_surface(ov))
        goto fail;

    ov->vptr =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
            ov->vptr_mgr, ov->seat, ov->selected_output->wl_output);

    log_info("overlay created: %ux%u on %s", ov->surf_width, ov->surf_height,
             output_name_or_unknown(ov->selected_output));
    return ov;

fail:
    overlay_destroy(ov);
    return NULL;
}

static void destroy_layer_shell(struct zwlr_layer_shell_v1 *layer_shell) {
    if (!layer_shell)
        return;

    if (zwlr_layer_shell_v1_get_version(layer_shell) >=
        ZWLR_LAYER_SHELL_V1_DESTROY_SINCE_VERSION) {
        zwlr_layer_shell_v1_destroy(layer_shell);
    } else {
        wl_proxy_destroy((struct wl_proxy *)layer_shell);
    }
}

void overlay_destroy(struct overlay *ov) {
    if (!ov)
        return;

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
    if (ov->pointer)
        wl_pointer_destroy(ov->pointer);
    if (ov->xkb_state)
        xkb_state_unref(ov->xkb_state);
    if (ov->xkb_keymap)
        xkb_keymap_unref(ov->xkb_keymap);
    if (ov->xkb_ctx)
        xkb_context_unref(ov->xkb_ctx);

    if (ov->frac_scale_mgr)
        wp_fractional_scale_manager_v1_destroy(ov->frac_scale_mgr);
    if (ov->viewporter)
        wp_viewporter_destroy(ov->viewporter);
    if (ov->vptr_mgr)
        zwlr_virtual_pointer_manager_v1_destroy(ov->vptr_mgr);
    while (ov->outputs) {
        struct output *output = ov->outputs;
        ov->outputs = output->next;
        output_destroy(output);
    }
    if (ov->xdg_out_mgr)
        zxdg_output_manager_v1_destroy(ov->xdg_out_mgr);
    destroy_layer_shell(ov->layer_shell);
    if (ov->seat)
        wl_seat_destroy(ov->seat);
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
    if (!ov)
        return;
    ov->rs = rs;
    request_frame(ov);
}

int overlay_get_width(const struct overlay *ov) {
    if (!ov)
        return 0;
    if (ov->surf_width > 0)
        return (int)ov->surf_width;
    if (ov->selected_output)
        return ov->selected_output->width;
    return 0;
}

int overlay_get_height(const struct overlay *ov) {
    if (!ov)
        return 0;
    if (ov->surf_height > 0)
        return (int)ov->surf_height;
    if (ov->selected_output)
        return ov->selected_output->height;
    return 0;
}

bool overlay_get_cursor_position(struct overlay *ov, int *x, int *y) {
    if (!ov || !x || !y)
        return false;

    if (!ov->cursor_position_known && ov->pointer && ov->surface) {
        /* Wayland has no global pointer-position query. Give the overlay
         * pointer focus briefly so wl_pointer.enter supplies it. */
        wl_surface_set_input_region(ov->surface, NULL);
        wl_surface_commit(ov->surface);
        int capture_result = wl_display_roundtrip(ov->display);

        wl_surface_set_input_region(ov->surface, ov->input_region);
        wl_surface_commit(ov->surface);
        int restore_result = wl_display_roundtrip(ov->display);

        if (capture_result < 0 || restore_result < 0)
            return false;
    }

    if (!ov->cursor_position_known)
        return false;

    *x = ov->cursor_x;
    *y = ov->cursor_y;
    return true;
}

void overlay_stop(struct overlay *ov) {
    if (ov)
        ov->running = false;
}

int overlay_run(struct overlay *ov, struct config *cfg,
                struct region_state *rs) {
    if (!ov)
        return -1;
    ov->cfg = cfg;
    ov->rs = rs;
    ov->running = true;

    send_frame(ov);

    int wl_fd = wl_display_get_fd(ov->display);

    struct pollfd fds[2];
    fds[0].fd = wl_fd;
    fds[0].events = POLLIN;
    fds[1].fd = ov->repeat_fd;
    fds[1].events = POLLIN;
    int nfds = ov->repeat_fd >= 0 ? 2 : 1;

    while (ov->running) {
        /* Flush outgoing requests before blocking. */
        while (wl_display_prepare_read(ov->display) != 0)
            wl_display_dispatch_pending(ov->display);
        wl_display_flush(ov->display);

        if (poll(fds, (nfds_t)nfds, -1) < 0) {
            wl_display_cancel_read(ov->display);
            if (errno == EINTR)
                continue;
            log_err("poll failed: %s", strerror(errno));
            return -1;
        }

        if (fds[0].revents & POLLIN) {
            wl_display_read_events(ov->display);
        } else {
            wl_display_cancel_read(ov->display);
        }
        wl_display_dispatch_pending(ov->display);

        if (nfds > 1 && (fds[1].revents & POLLIN)) {
            uint64_t expirations;
            if (read(ov->repeat_fd, &expirations, sizeof(expirations)) > 0 &&
                ov->repeat_key != 0) {
                handle_key_dispatch(ov, ov->repeat_key);
            }
        }
    }

    return 0;
}

void vptr_warp(struct overlay *ov, int x, int y) {
    if (!ov || !ov->vptr)
        return;

    uint32_t ow = (uint32_t)overlay_get_width(ov);
    uint32_t oh = (uint32_t)overlay_get_height(ov);

    log_debug("vptr warp: %d,%d in %ux%u", x, y, ow, oh);

    zwlr_virtual_pointer_v1_motion_absolute(ov->vptr, 0, (uint32_t)x,
                                            (uint32_t)y, ow, oh);
    zwlr_virtual_pointer_v1_frame(ov->vptr);
    ov->cursor_x = x;
    ov->cursor_y = y;
    ov->cursor_position_known = true;
    wl_display_roundtrip(ov->display);
}

/* Map keynav button numbers to Linux input event codes. */
static uint32_t keynav_btn(int button) {
    switch (button) {
    case 1:
        return BTN_LEFT;
    case 2:
        return BTN_MIDDLE;
    case 3:
        return BTN_RIGHT;
    default:
        return 0;
    }
}

void vptr_click(struct overlay *ov, int button) {
    if (!ov || !ov->vptr)
        return;

    /* Buttons 4/5 are scroll up/down. */
    if (button == 4 || button == 5) {
        /* Axis scroll: 4=up (negative), 5=down (positive).
         * Value 15 is ~one notch. */
        int32_t dir = (button == 5) ? 15 : -15;
        zwlr_virtual_pointer_v1_axis(ov->vptr, 0, 0 /* vertical */,
                                     wl_fixed_from_int(dir));
        zwlr_virtual_pointer_v1_frame(ov->vptr);
        wl_display_roundtrip(ov->display);
        return;
    }

    uint32_t btn = keynav_btn(button);
    if (!btn)
        return;

    zwlr_virtual_pointer_v1_button(ov->vptr, 0, btn,
                                   WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(ov->vptr);
    wl_display_roundtrip(ov->display);

    zwlr_virtual_pointer_v1_button(ov->vptr, 0, btn,
                                   WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(ov->vptr);
    wl_display_roundtrip(ov->display);
}

void vptr_button_down(struct overlay *ov, int button) {
    if (!ov || !ov->vptr)
        return;
    uint32_t btn = keynav_btn(button);
    if (!btn)
        return;

    zwlr_virtual_pointer_v1_button(ov->vptr, 0, btn,
                                   WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(ov->vptr);
    wl_display_roundtrip(ov->display);
}

void vptr_button_up(struct overlay *ov, int button) {
    if (!ov || !ov->vptr)
        return;
    uint32_t btn = keynav_btn(button);
    if (!btn)
        return;

    zwlr_virtual_pointer_v1_button(ov->vptr, 0, btn,
                                   WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(ov->vptr);
    wl_display_roundtrip(ov->display);
}
