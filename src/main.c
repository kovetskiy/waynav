/*
 * waynav — Wayland keynav: grid-based keyboard mouse navigation.
 *
 * Reads waynavrc, shows a grid overlay via layer-shell,
 * warps the pointer and clicks via wlr-virtual-pointer.
 */

#include "log.h"
#include "waynav.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  -c, --config PATH   config file "
            "(default: ~/.config/waynav/waynavrc)\n"
        "  -l, --log LEVEL     "
            "error, warn, info (default), debug\n"
        "  -v, --version       print version and exit\n"
        "  -h, --help          show this help\n",
        prog);
}

static void print_version(void) {
    fprintf(stderr, "waynav %s\n", VERSION);
}

static const char *find_config_path(void) {
    static char buf[512];

    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg) {
        snprintf(buf, sizeof(buf), "%s/waynav/waynavrc", xdg);
        FILE *f = fopen(buf, "r");
        if (f) { fclose(f); return buf; }
    }

    const char *home = getenv("HOME");
    if (home) {
        snprintf(buf, sizeof(buf),
                 "%s/.config/waynav/waynavrc", home);
        FILE *f = fopen(buf, "r");
        if (f) { fclose(f); return buf; }

        snprintf(buf, sizeof(buf), "%s/.keynavrc", home);
        FILE *f2 = fopen(buf, "r");
        if (f2) { fclose(f2); return buf; }
    }

    return NULL;
}

int main(int argc, char **argv) {
    const char *config_path = NULL;
    const char *log_level_str = NULL;

    static struct option long_options[] = {
        {"config",  required_argument, 0, 'c'},
        {"log",     required_argument, 0, 'l'},
        {"version", no_argument,       0, 'v'},
        {"help",    no_argument,       0, 'h'},
        {NULL,      0,                 0,  0 },
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:l:vh",
                              long_options, NULL)) != -1) {
        switch (opt) {
        case 'c':
            config_path = optarg;
            break;
        case 'l':
            log_level_str = optarg;
            break;
        case 'v':
            print_version();
            return 0;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Init logging. CLI flag overrides env var. */
    if (log_level_str)
        setenv("WAYNAV_LOG", log_level_str, 1);
    log_init();

    if (!config_path)
        config_path = find_config_path();
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

    /* Connect to Wayland and create the overlay. */
    struct overlay *ov = overlay_create();
    if (!ov) {
        log_err("failed to create overlay");
        return 1;
    }

    /* Initialise region to full screen. */
    int scr_w = overlay_get_width(ov);
    int scr_h = overlay_get_height(ov);
    log_info("screen: %dx%d", scr_w, scr_h);

    struct region_state rs;
    region_init(&rs, scr_w, scr_h);

    /* Apply startup commands (e.g. "grid 4x4"). */
    if (cfg.num_start_commands > 0) {
        execute_commands(ov, &rs,
                         cfg.start_commands,
                         cfg.num_start_commands);
    }

    /* Enter the event loop. Blocks until CMD_END or error. */
    int ret = overlay_run(ov, &cfg, &rs);

    overlay_destroy(ov);
    log_info("exiting");
    return ret < 0 ? 1 : 0;
}
