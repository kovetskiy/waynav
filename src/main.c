/*
 * waynav — Wayland keynav: grid-based keyboard mouse navigation.
 *
 * Reads waynavrc, shows a grid overlay via layer-shell,
 * warps the pointer and clicks via wlr-virtual-pointer.
 */

#include "log.h"
#include "waynav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *find_config_path(void) {
    static char buf[512];

    /* XDG_CONFIG_HOME/keynav/keynavrc */
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg) {
        snprintf(buf, sizeof(buf), "%s/waynav/waynavrc", xdg);
        FILE *f = fopen(buf, "r");
        if (f) { fclose(f); return buf; }
    }

    /* ~/.config/keynav/keynavrc */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(buf, sizeof(buf),
                 "%s/.config/waynav/waynavrc", home);
        FILE *f = fopen(buf, "r");
        if (f) { fclose(f); return buf; }

        /* ~/.keynavrc */
        snprintf(buf, sizeof(buf), "%s/.keynavrc", home);
        FILE *f2 = fopen(buf, "r");
        if (f2) { fclose(f2); return buf; }
    }

    return NULL;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    log_init();

    const char *config_path = find_config_path();
    if (!config_path) {
        log_err("no waynavrc found");
        return 1;
    }

    struct config cfg;
    if (config_load(&cfg, config_path) != 0) {
        log_err("failed to load %s", config_path);
        return 1;
    }

    log_info("loaded %d bindings from %s", cfg.num_bindings,
             config_path);
    log_debug("start commands: %d", cfg.num_start_commands);

    /* TODO:
     * 1. overlay_create() — connect to Wayland, set up layer
     *    surface, virtual pointer, xkb keyboard.
     * 2. Apply start_commands (grid setup).
     * 3. Enter event loop: receive keyboard events, dispatch
     *    through config bindings, execute command chains.
     * 4. On CMD_END, overlay_hide() and exit.
     */

    return 0;
}
