/*
 * Tests for config parser.
 */

#include "../src/waynav.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_tmp_config(const char *content, const char *path) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}

static void test_basic_parse(void) {
    const char *path = "/tmp/waynav_test_config";
    write_tmp_config(
        "clear\n"
        "super+semicolon start,grid 4x4\n"
        "h move-left,warp\n"
        "shift+h cut-left,warp\n"
        "space warp,click 1\n"
        "semicolon end\n",
        path
    );

    struct config cfg;
    assert(config_load(&cfg, path) == 0);

    /* start binding is stored separately. */
    assert(cfg.num_start_commands == 1);
    assert(cfg.start_commands[0].type == CMD_GRID);
    assert(cfg.start_commands[0].arg.grid.cols == 4);
    assert(cfg.start_commands[0].arg.grid.rows == 4);

    /* 4 normal bindings: h, shift+h, space, semicolon */
    assert(cfg.num_bindings == 4);

    /* h -> move-left, warp */
    const struct binding *b = config_find_binding(
        &cfg, XKB_KEY_h, 0);
    assert(b);
    assert(b->num_commands == 2);
    assert(b->commands[0].type == CMD_MOVE_LEFT);
    assert(b->commands[1].type == CMD_WARP);

    /* shift+h -> cut-left, warp */
    b = config_find_binding(&cfg, XKB_KEY_h, MOD_SHIFT);
    assert(b);
    assert(b->num_commands == 2);
    assert(b->commands[0].type == CMD_CUT_LEFT);
    assert(b->commands[1].type == CMD_WARP);

    /* space -> warp, click 1 */
    b = config_find_binding(&cfg, XKB_KEY_space, 0);
    assert(b);
    assert(b->num_commands == 2);
    assert(b->commands[0].type == CMD_WARP);
    assert(b->commands[1].type == CMD_CLICK);
    assert(b->commands[1].arg.button == 1);

    /* semicolon -> end */
    b = config_find_binding(&cfg, XKB_KEY_semicolon, 0);
    assert(b);
    assert(b->num_commands == 1);
    assert(b->commands[0].type == CMD_END);

    unlink(path);
}

static void test_cell_select(void) {
    const char *path = "/tmp/waynav_test_config2";
    write_tmp_config(
        "clear\n"
        "1 cell-select 1,warp\n"
        "v cell-select 16,warp\n",
        path
    );

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    assert(cfg.num_bindings == 2);

    const struct binding *b = config_find_binding(
        &cfg, XKB_KEY_1, 0);
    assert(b);
    assert(b->commands[0].type == CMD_CELL_SELECT);
    assert(b->commands[0].arg.cell == 1);

    b = config_find_binding(&cfg, XKB_KEY_v, 0);
    assert(b);
    assert(b->commands[0].type == CMD_CELL_SELECT);
    assert(b->commands[0].arg.cell == 16);

    unlink(path);
}

static void test_shell_command(void) {
    const char *path = "/tmp/waynav_test_config3";
    write_tmp_config(
        "clear\n"
        "grave shell 'notify deprecated'\n",
        path
    );

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    assert(cfg.num_bindings == 1);

    const struct binding *b = config_find_binding(
        &cfg, XKB_KEY_grave, 0);
    assert(b);
    assert(b->commands[0].type == CMD_SHELL);
    assert(strcmp(b->commands[0].arg.shell_cmd,
                 "notify deprecated") == 0);

    free(b->commands[0].arg.shell_cmd);
    unlink(path);
}

static void test_cursorzoom(void) {
    const char *path = "/tmp/waynav_test_config4";
    write_tmp_config(
        "clear\n"
        "i grid 1x1,cursorzoom 10 10\n",
        path
    );

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    assert(cfg.num_bindings == 1);

    const struct binding *b = config_find_binding(
        &cfg, XKB_KEY_i, 0);
    assert(b);
    assert(b->num_commands == 2);
    assert(b->commands[0].type == CMD_GRID);
    assert(b->commands[0].arg.grid.cols == 1);
    assert(b->commands[0].arg.grid.rows == 1);
    assert(b->commands[1].type == CMD_CURSORZOOM);
    assert(b->commands[1].arg.zoom.w == 10);
    assert(b->commands[1].arg.zoom.h == 10);

    unlink(path);
}

static void test_drag(void) {
    const char *path = "/tmp/waynav_test_config5";
    write_tmp_config(
        "clear\n"
        "shift+space warp,drag 1\n"
        "shift+minus warp,drag 3\n",
        path
    );

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    assert(cfg.num_bindings == 2);

    const struct binding *b = config_find_binding(
        &cfg, XKB_KEY_space, MOD_SHIFT);
    assert(b);
    assert(b->commands[1].type == CMD_DRAG);
    assert(b->commands[1].arg.button == 1);

    b = config_find_binding(&cfg, XKB_KEY_minus, MOD_SHIFT);
    assert(b);
    assert(b->commands[1].type == CMD_DRAG);
    assert(b->commands[1].arg.button == 3);

    unlink(path);
}

int main(void) {
    test_basic_parse();
    test_cell_select();
    test_shell_command();
    test_cursorzoom();
    test_drag();

    printf("All config tests passed.\n");
    return 0;
}
