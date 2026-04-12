/*
 * waynav — Wayland keynav: grid-based keyboard mouse navigation.
 *
 * Reads waynavrc, shows a grid overlay via layer-shell,
 * warps the pointer and clicks via wlr-virtual-pointer.
 */

#include "log.h"
#include "waynav.h"

#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <unistd.h>

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
        if (f) {
            fclose(f);
            return buf;
        }
    }

    const char *home = getenv("HOME");
    if (home) {
        snprintf(buf, sizeof(buf), "%s/.config/waynav/waynavrc", home);
        FILE *f = fopen(buf, "r");
        if (f) {
            fclose(f);
            return buf;
        }
    }

    return NULL;
}

/* Try to acquire an exclusive lock. Returns fd on success,
 * -1 if another instance is running. */
static int acquire_lock(void) {
    static char path[512];
    const char *run = getenv("XDG_RUNTIME_DIR");
    if (!run)
        run = "/tmp";
    snprintf(path, sizeof(path), "%s/waynav.lock", run);

    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;

    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv) {
    int lock_fd = acquire_lock();
    if (lock_fd < 0) {
        /* Already running — exit silently. */
        return 0;
    }

    const char *config_path = NULL;
    const char *log_level_str = NULL;

    static struct option long_options[] = {
        {"config", required_argument, 0, 'c'},
        {"log", required_argument, 0, 'l'},
        {"version", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {NULL, 0, 0, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:l:vh", long_options, NULL)) !=
           -1) {
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

    log_info("loaded %d bindings from %s", cfg.num_bindings, config_path);
    log_debug("start commands: %d", cfg.num_start_commands);

    struct overlay *ov = overlay_create();
    if (!ov) {
        log_err("failed to create overlay");
        return 1;
    }

    int scr_w = overlay_get_width(ov);
    int scr_h = overlay_get_height(ov);
    log_info("screen: %dx%d", scr_w, scr_h);

    struct region_state rs;
    region_init(&rs, scr_w, scr_h);

    if (cfg.num_start_commands > 0) {
        execute_commands(ov, &rs, cfg.start_commands, cfg.num_start_commands);
    }

    int ret = overlay_run(ov, &cfg, &rs);

    overlay_destroy(ov);
    log_info("exiting");
    return ret < 0 ? 1 : 0;
}
