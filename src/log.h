/*
 * Logging.
 *
 * Levels: error, warn, info, debug. Controlled by WAYNAV_LOG
 * environment variable (error|warn|info|debug). Default: info.
 *
 * All output goes to stderr. Color is auto-detected (isatty)
 * and can be forced with WAYNAV_LOG_COLOR=1|0.
 *
 * Usage:
 *   log_info("loaded %d bindings", n);
 *   log_err("failed to connect: %s", strerror(errno));
 *   log_debug("key event: sym=0x%x mods=0x%x", sym, mods);
 */

#ifndef WAYNAV_LOG_H
#define WAYNAV_LOG_H

#include <stdio.h>

enum log_level {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN = 1,
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_DEBUG = 3,
};

void log_init(void);
void log_msg(enum log_level level, const char *file, int line, const char *fmt,
             ...) __attribute__((format(printf, 4, 5)));

extern enum log_level log_threshold;

#define log_err(...) log_msg(LOG_LEVEL_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...) log_msg(LOG_LEVEL_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...) log_msg(LOG_LEVEL_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define log_debug(...)                                                         \
    do {                                                                       \
        if (log_threshold >= LOG_LEVEL_DEBUG)                                  \
            log_msg(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __VA_ARGS__);         \
    } while (0)

#endif /* WAYNAV_LOG_H */
