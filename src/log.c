/*
 * Logging implementation.
 *
 * Level is set once at startup from WAYNAV_LOG env var.
 * Color is auto-detected from stderr being a terminal.
 */

#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum log_level log_threshold = LOG_LEVEL_INFO;
static int use_color = 0;

void log_init(void) {
    const char *env = getenv("WAYNAV_LOG");
    if (env) {
        if (strcmp(env, "error") == 0)
            log_threshold = LOG_LEVEL_ERROR;
        else if (strcmp(env, "warn") == 0)
            log_threshold = LOG_LEVEL_WARN;
        else if (strcmp(env, "info") == 0)
            log_threshold = LOG_LEVEL_INFO;
        else if (strcmp(env, "debug") == 0)
            log_threshold = LOG_LEVEL_DEBUG;
    }

    const char *color_env = getenv("WAYNAV_LOG_COLOR");
    if (color_env) {
        use_color = (color_env[0] == '1');
    } else {
        use_color = isatty(STDERR_FILENO);
    }
}

static const char *level_str(enum log_level level) {
    switch (level) {
    case LOG_LEVEL_ERROR: return "error";
    case LOG_LEVEL_WARN:  return "warn";
    case LOG_LEVEL_INFO:  return "info";
    case LOG_LEVEL_DEBUG: return "debug";
    }
    return "?";
}

/* ANSI color codes: red, yellow, blue, magenta. */
static const char *level_color(enum log_level level) {
    switch (level) {
    case LOG_LEVEL_ERROR: return "\x1b[31m";
    case LOG_LEVEL_WARN:  return "\x1b[33m";
    case LOG_LEVEL_INFO:  return "\x1b[34m";
    case LOG_LEVEL_DEBUG: return "\x1b[35m";
    }
    return "";
}

void log_msg(enum log_level level, const char *file, int line,
             const char *fmt, ...) {
    if (level > log_threshold)
        return;

    if (use_color) {
        fprintf(stderr, "%s%-5s\x1b[0m ", level_color(level),
                level_str(level));
    } else {
        fprintf(stderr, "%-5s ", level_str(level));
    }

    if (log_threshold >= LOG_LEVEL_DEBUG) {
        /* Strip leading path components for readability. */
        const char *base = strrchr(file, '/');
        base = base ? base + 1 : file;
        fprintf(stderr, "[%s:%d] ", base, line);
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fputc('\n', stderr);
}
