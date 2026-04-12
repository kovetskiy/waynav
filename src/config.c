/*
 * Parse waynavrc into bindings.
 *
 * Syntax:  keysequence cmd1,cmd2,cmd3
 * Example: shift+h cut-left,warp
 */

#include "log.h"
#include "waynav.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static xkb_keysym_t parse_keysym(const char *name) {
    xkb_keysym_t sym = xkb_keysym_from_name(name, 0);
    if (sym == XKB_KEY_NoSymbol) {
        sym = xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
    }
    return sym;
}

/* Parse "shift+ctrl+h" into keysym + modifier mask.
 * Modifiers: shift, ctrl, alt, super.
 * Last token is the key name. */
static int parse_keysequence(const char *seq, xkb_keysym_t *sym,
                             uint32_t *mods) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", seq);

    *mods = 0;
    *sym = XKB_KEY_NoSymbol;

    char *save = NULL;
    char *tok = strtok_r(buf, "+", &save);
    char *last = NULL;

    while (tok) {
        if (last) {
            if (strcasecmp(last, "shift") == 0)
                *mods |= MOD_SHIFT;
            else if (strcasecmp(last, "ctrl") == 0 ||
                     strcasecmp(last, "control") == 0)
                *mods |= MOD_CTRL;
            else if (strcasecmp(last, "alt") == 0)
                *mods |= MOD_ALT;
            else if (strcasecmp(last, "super") == 0)
                *mods |= MOD_SUPER;
            else {
                /* Not a known modifier; treat as key name. */
                *sym = parse_keysym(last);
                if (*sym == XKB_KEY_NoSymbol)
                    return -1;
            }
        }
        last = tok;
        tok = strtok_r(NULL, "+", &save);
    }

    if (last) {
        *sym = parse_keysym(last);
        if (*sym == XKB_KEY_NoSymbol)
            return -1;
    }

    return 0;
}

/* Simple commands: keyword maps directly to type, no args. */
static const struct {
    const char *name;
    enum command_type type;
} simple_commands[] = {
    {"start", CMD_START},
    {"end", CMD_END},
    {"cut-left", CMD_CUT_LEFT},
    {"cut-right", CMD_CUT_RIGHT},
    {"cut-up", CMD_CUT_UP},
    {"cut-down", CMD_CUT_DOWN},
    {"move-left", CMD_MOVE_LEFT},
    {"move-right", CMD_MOVE_RIGHT},
    {"move-up", CMD_MOVE_UP},
    {"move-down", CMD_MOVE_DOWN},
    {"warp", CMD_WARP},
    {"history-back", CMD_HISTORY_BACK},
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static bool try_simple_command(const char *str, struct command *cmd) {
    for (size_t i = 0; i < ARRAY_LEN(simple_commands); i++) {
        const char *name = simple_commands[i].name;
        size_t len = strlen(name);
        if (strncmp(str, name, len) == 0 &&
            !isalpha((unsigned char)str[len])) {
            cmd->type = simple_commands[i].type;
            return true;
        }
    }
    return false;
}

static int parse_shell(const char *str, struct command *cmd) {
    cmd->type = CMD_SHELL;
    const char *arg = str + (str[2] == 'e' ? 5 : 2);
    while (isspace((unsigned char)*arg))
        arg++;
    size_t len = strlen(arg);
    if (len >= 2 && arg[0] == '\'' && arg[len - 1] == '\'') {
        cmd->arg.shell_cmd = strndup(arg + 1, len - 2);
    } else {
        cmd->arg.shell_cmd = strdup(arg);
    }
    return 0;
}

/* Parse a single command string like "click 1" or "grid 4x4"
 * into a struct command. Returns 0 on success. */
static int parse_command(const char *str, struct command *cmd) {
    while (isspace((unsigned char)*str))
        str++;

    if (try_simple_command(str, cmd))
        return 0;

    if (strncmp(str, "grid", 4) == 0 &&
        !isalpha((unsigned char)str[4])) {
        cmd->type = CMD_GRID;
        int cols = 0, rows = 0;
        if (sscanf(str + 4, " %dx%d", &cols, &rows) == 2) {
            cmd->arg.grid.cols = cols;
            cmd->arg.grid.rows = rows;
        } else {
            int n = atoi(str + 5);
            cmd->arg.grid.cols = n;
            cmd->arg.grid.rows = n;
        }
    } else if (strncmp(str, "cell-select", 11) == 0) {
        cmd->type = CMD_CELL_SELECT;
        cmd->arg.cell = atoi(str + 11);
    } else if (strncmp(str, "click", 5) == 0) {
        cmd->type = CMD_CLICK;
        cmd->arg.button = atoi(str + 5);
    } else if (strncmp(str, "drag", 4) == 0) {
        cmd->type = CMD_DRAG;
        cmd->arg.button = atoi(str + 4);
    } else if (strncmp(str, "cursorzoom", 10) == 0) {
        cmd->type = CMD_CURSORZOOM;
        int w = 0, h = 0;
        if (sscanf(str + 10, " %d %d", &w, &h) == 2) {
            cmd->arg.zoom.w = w;
            cmd->arg.zoom.h = h;
        } else {
            int s = atoi(str + 10);
            cmd->arg.zoom.w = s;
            cmd->arg.zoom.h = s;
        }
    } else if (strncmp(str, "shell", 5) == 0 ||
               strncmp(str, "sh", 2) == 0) {
        return parse_shell(str, cmd);
    } else {
        return -1;
    }
    return 0;
}

/* Parse a comma-separated command chain into a binding's
 * command array. Returns the number of commands parsed. */
static int parse_command_chain(const char *chain, struct command *cmds,
                               int max) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", chain);
    int count = 0;
    char *save = NULL;
    const char *tok = strtok_r(buf, ",", &save);
    while (tok && count < max) {
        if (parse_command(tok, &cmds[count]) == 0)
            count++;
        tok = strtok_r(NULL, ",", &save);
    }
    return count;
}

static void store_start_commands(struct config *cfg,
                                 const struct command *cmds,
                                 int ncmds) {
    cfg->num_start_commands = 0;
    for (int i = 1; i < ncmds; i++)
        cfg->start_commands[cfg->num_start_commands++] = cmds[i];
    log_debug("start binding: %d chained commands",
              cfg->num_start_commands);
}

static int store_binding(struct config *cfg, const char *path,
                         int lineno, xkb_keysym_t sym,
                         uint32_t mods, const struct command *cmds,
                         int ncmds) {
    if (cfg->num_bindings >= MAX_BINDINGS) {
        for (int i = 0; i < ncmds; i++) {
            if (cmds[i].type == CMD_SHELL)
                free(cmds[i].arg.shell_cmd);
        }
        log_warn("%s:%d: too many bindings (max %d)", path,
                 lineno, MAX_BINDINGS);
        return -1;
    }

    struct binding *b = &cfg->bindings[cfg->num_bindings];
    b->keysym = sym;
    b->mods = mods;
    b->num_commands = ncmds;
    memcpy(b->commands, cmds, ncmds * sizeof(struct command));
    cfg->num_bindings++;

    log_debug("bind: sym=0x%x mods=0x%x cmds=%d", sym, mods,
              ncmds);
    return 0;
}

static int parse_line(struct config *cfg, const char *path,
                      int lineno, char *line) {
    char *comment = strchr(line, '#');
    if (comment)
        *comment = '\0';

    while (isspace((unsigned char)*line))
        line++;

    if (*line == '\0')
        return 0;

    if (strcmp(line, "clear") == 0) {
        cfg->num_bindings = 0;
        log_debug("clear: reset bindings");
        return 0;
    }

    char *space = line;
    while (*space && !isspace((unsigned char)*space))
        space++;
    if (*space == '\0')
        return 0;

    *space = '\0';
    const char *keyseq = line;
    const char *chain = space + 1;

    xkb_keysym_t sym;
    uint32_t mods;
    if (parse_keysequence(keyseq, &sym, &mods) != 0) {
        log_warn("%s:%d: unknown key '%s'", path, lineno, keyseq);
        return -1;
    }

    struct command cmds[MAX_COMMANDS];
    int ncmds = parse_command_chain(chain, cmds, MAX_COMMANDS);
    if (ncmds <= 0)
        return 0;

    if (cmds[0].type == CMD_START) {
        store_start_commands(cfg, cmds, ncmds);
        return 0;
    }

    return store_binding(cfg, path, lineno, sym, mods, cmds,
                         ncmds);
}

int config_load(struct config *cfg, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        log_err("cannot open %s", path);
        return -1;
    }

    memset(cfg, 0, sizeof(*cfg));

    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (parse_line(cfg, path, lineno, line) != 0)
            log_warn("parse error at %s:%d", path, lineno);
    }

    fclose(f);
    return 0;
}

const struct binding *config_find_binding(const struct config *cfg,
                                          xkb_keysym_t sym, uint32_t mods) {
    for (int i = 0; i < cfg->num_bindings; i++) {
        if (cfg->bindings[i].keysym == sym && cfg->bindings[i].mods == mods)
            return &cfg->bindings[i];
    }
    return NULL;
}
