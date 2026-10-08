// Direct C equivalents of the Zig test blocks in layout.zig (which
// were direct equivalents of the C test_layout.c cases, which were
// direct equivalents of scrollwm's test_layout.py).
//
// No X server needed: `make test`.

#include "../src/layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;
static const char *current_test = "";

static void check(bool ok, const char *expr, int line) {
    if (!ok) {
        tests_failed++;
        fprintf(stderr, "FAIL %s (line %d): %s\n", current_test, line, expr);
    }
}

#define EXPECT(cond) check((cond), #cond, __LINE__)

#define EXPECT_EQ_I(expected, actual)                                          \
    do {                                                                       \
        long long e_ = (long long)(expected);                                  \
        long long a_ = (long long)(actual);                                    \
        if (e_ != a_) {                                                        \
            tests_failed++;                                                    \
            fprintf(stderr, "FAIL %s (line %d): expected %lld, got %lld (%s)\n", \
                current_test, __LINE__, e_, a_, #actual);                      \
        }                                                                      \
    } while (0)

static void run(const char *name, void (*fn)(void)) {
    current_test = name;
    tests_run++;
    fn();
}

// ---- tests ---------------------------------------------------------

static void test_single_window_fills_left(void) {
    ScrollLayout sl;
    layout_init(&sl, 1360, 768);
    EXPECT(layout_add_window(&sl, 1, true));

    GeomEntry g[MAX_COLUMNS];
    size_t n = layout_geometry(&sl, g, MAX_COLUMNS);

    EXPECT_EQ_I(1, n);
    EXPECT_EQ_I(1, g[0].window_id);
    EXPECT_EQ_I(DEFAULT_GAP, g[0].x);
}

static void test_top_inset_drops_windows_below_the_bar(void) {
    ScrollLayout sl;
    layout_init(&sl, 1360, 768);
    sl.top_inset = 26;
    EXPECT(layout_add_window(&sl, 1, true));

    GeomEntry g[MAX_COLUMNS];
    size_t n = layout_geometry(&sl, g, MAX_COLUMNS);

    EXPECT_EQ_I(1, n);
    EXPECT_EQ_I(DEFAULT_GAP + 26, g[0].y);
    EXPECT_EQ_I(768 - 26 - 2 * DEFAULT_GAP, g[0].h);
}

static void test_zero_inset_is_the_historical_layout(void) {
    ScrollLayout sl;
    layout_init(&sl, 1360, 768);
    EXPECT(layout_add_window(&sl, 1, true));

    GeomEntry g[MAX_COLUMNS];
    size_t n = layout_geometry(&sl, g, MAX_COLUMNS);

    EXPECT_EQ_I(1, n);
    EXPECT_EQ_I(DEFAULT_GAP, g[0].y);
    EXPECT_EQ_I(768 - 2 * DEFAULT_GAP, g[0].h);
}

static void test_scroll_follows_focus_right(void) {
    ScrollLayout sl;
    layout_init(&sl, 1360, 768);
    for (uint64_t i = 1; i <= 4; i++)
        EXPECT(layout_add_window(&sl, i, true));
    // focused_index is now on the last-added window (4)
    layout_focus_next(&sl); // wraps back to column 0
    layout_focus_prev(&sl); // back to column 3 (window 4)
    layout_focus_prev(&sl); // column 2 (window 3)

    // viewport should have scrolled enough that the focused column's
    // right edge is on screen
    Column *f = layout_focused(&sl);
    EXPECT(f != NULL);

    GeomEntry g[MAX_COLUMNS];
    size_t n = layout_geometry(&sl, g, MAX_COLUMNS);
    bool visible = false;
    for (size_t k = 0; k < n; k++) {
        if (g[k].window_id == f->window_id) {
            visible = (g[k].x >= 0 && g[k].x + g[k].w <= sl.screen_width) ||
                (g[k].x < sl.screen_width && g[k].x + g[k].w > 0);
        }
    }
    EXPECT(visible);
}

static void test_focus_prev_scrolls_left(void) {
    ScrollLayout sl;
    layout_init(&sl, 1360, 768);
    for (uint64_t i = 1; i <= 4; i++)
        EXPECT(layout_add_window(&sl, i, true));
    // scroll forward then back to the first column
    for (int k = 0; k < 3; k++)
        layout_focus_prev(&sl);
    EXPECT_EQ_I(0, sl.workspaces[0].viewport_x);
}

static void test_move_focused_right_swaps_order(void) {
    ScrollLayout sl;
    layout_init(&sl, 1360, 768);
    EXPECT(layout_add_window(&sl, 1, true));
    EXPECT(layout_add_window(&sl, 2, true)); // order: 1, 2 -- focus on 2
    layout_focus_prev(&sl);                  // focus back to 1 (index 0)
    layout_move_focused_right(&sl);

    EXPECT_EQ_I(2, sl.workspaces[0].columns[0].window_id);
    EXPECT_EQ_I(1, sl.workspaces[0].columns[1].window_id);
    EXPECT_EQ_I(1, sl.workspaces[0].focused_index);
}

static void test_remove_window_reindexes(void) {
    ScrollLayout sl;
    layout_init(&sl, 1360, 768);
    EXPECT(layout_add_window(&sl, 1, true));
    EXPECT(layout_add_window(&sl, 2, true));
    EXPECT(layout_add_window(&sl, 3, true));
    EXPECT(layout_remove_window(&sl, 2));

    EXPECT_EQ_I(2, sl.workspaces[0].ncolumns);
    EXPECT_EQ_I(1, sl.workspaces[0].columns[0].window_id);
    EXPECT_EQ_I(3, sl.workspaces[0].columns[1].window_id);
}

static void test_resize_focused_respects_min_width(void) {
    ScrollLayout sl;
    layout_init(&sl, 1360, 768);
    EXPECT(layout_add_window(&sl, 1, true));
    layout_resize_focused(&sl, -100000);

    EXPECT_EQ_I(DEFAULT_MIN_COLUMN_WIDTH, sl.workspaces[0].columns[0].width);
}

static void test_maximize_toggle_restores_previous_width(void) {
    ScrollLayout sl;
    layout_init(&sl, 1360, 768);
    EXPECT(layout_add_window(&sl, 1, true));
    int32_t original_width = sl.workspaces[0].columns[0].width;

    layout_toggle_maximize_focused(&sl);
    int32_t maximized_width = sl.workspaces[0].columns[0].width;

    layout_toggle_maximize_focused(&sl);
    int32_t restored_width = sl.workspaces[0].columns[0].width;

    EXPECT_EQ_I(sl.screen_width - 2 * DEFAULT_GAP, maximized_width);
    EXPECT_EQ_I(original_width, restored_width);
}

static void test_new_window_goes_to_active_workspace(void) {
    ScrollLayout sl;
    layout_init(&sl, 1360, 768);
    layout_switch_workspace(&sl, 2);
    EXPECT(layout_add_window(&sl, 1, true));

    EXPECT_EQ_I(1, sl.workspaces[2].ncolumns);
    EXPECT_EQ_I(0, sl.workspaces[0].ncolumns);
    EXPECT_EQ_I(0, sl.workspaces[1].ncolumns);
}

static void test_switching_workspace_hides_other_workspace_windows(void) {
    ScrollLayout sl;
    layout_init(&sl, 1360, 768);
    EXPECT(layout_add_window(&sl, 1, true)); // workspace 0
    layout_switch_workspace(&sl, 1);
    EXPECT(layout_add_window(&sl, 2, true)); // workspace 1

    GeomEntry g[MAX_COLUMNS];
    size_t n = layout_geometry(&sl, g, MAX_COLUMNS);

    EXPECT_EQ_I(1, n);
    EXPECT_EQ_I(2, g[0].window_id);
}

static void test_switching_back_restores_scroll_and_focus(void) {
    ScrollLayout sl;
    layout_init(&sl, 1360, 768);
    for (uint64_t i = 1; i <= 4; i++)
        EXPECT(layout_add_window(&sl, i, true));
    int32_t focus_before = sl.workspaces[0].focused_index;
    int32_t vx_before = sl.workspaces[0].viewport_x;

    layout_switch_workspace(&sl, 3);
    EXPECT(layout_add_window(&sl, 100, true));
    layout_switch_workspace(&sl, 0);

    EXPECT_EQ_I(focus_before, sl.workspaces[0].focused_index);
    EXPECT_EQ_I(vx_before, sl.workspaces[0].viewport_x);
}

static void test_move_focused_to_workspace_follows_window(void) {
    ScrollLayout sl;
    layout_init(&sl, 1360, 768);
    EXPECT(layout_add_window(&sl, 1, true)); // workspace 0
    layout_move_focused_to_workspace(&sl, 4);

    EXPECT_EQ_I(4, sl.active_workspace);
    EXPECT_EQ_I(1, sl.workspaces[4].ncolumns);
    EXPECT_EQ_I(1, sl.workspaces[4].columns[0].window_id);
    EXPECT_EQ_I(0, sl.workspaces[0].ncolumns);
}

static void test_remove_window_finds_it_on_inactive_workspace(void) {
    ScrollLayout sl;
    layout_init(&sl, 1360, 768);
    EXPECT(layout_add_window(&sl, 1, true)); // workspace 0
    layout_switch_workspace(&sl, 5);         // now looking at an empty workspace
    bool removed = layout_remove_window(&sl, 1);

    EXPECT(removed);
    EXPECT_EQ_I(0, sl.workspaces[0].ncolumns);
}

static void test_workspace_index_clamped(void) {
    ScrollLayout sl;
    layout_init(&sl, 1360, 768);
    layout_switch_workspace(&sl, 999);
    int32_t clamped_high = sl.active_workspace;
    layout_switch_workspace(&sl, -5);
    int32_t clamped_low = sl.active_workspace;

    EXPECT_EQ_I(MAX_WORKSPACES - 1, clamped_high);
    EXPECT_EQ_I(0, clamped_low);
}

int main(void) {
    run("single window fills left", test_single_window_fills_left);
    run("top inset drops windows below the bar", test_top_inset_drops_windows_below_the_bar);
    run("zero inset is the historical layout", test_zero_inset_is_the_historical_layout);
    run("scroll follows focus right", test_scroll_follows_focus_right);
    run("focus prev scrolls left", test_focus_prev_scrolls_left);
    run("move focused right swaps order", test_move_focused_right_swaps_order);
    run("remove window reindexes", test_remove_window_reindexes);
    run("resize focused respects min width", test_resize_focused_respects_min_width);
    run("maximize toggle restores previous width", test_maximize_toggle_restores_previous_width);
    run("new window goes to active workspace", test_new_window_goes_to_active_workspace);
    run("switching workspace hides other workspace windows", test_switching_workspace_hides_other_workspace_windows);
    run("switching back restores scroll and focus", test_switching_back_restores_scroll_and_focus);
    run("move focused to workspace follows window", test_move_focused_to_workspace_follows_window);
    run("remove window finds it on inactive workspace", test_remove_window_finds_it_on_inactive_workspace);
    run("workspace index clamped", test_workspace_index_clamped);

    if (tests_failed == 0) {
        printf("OK: %d test(s) passed\n", tests_run);
        return 0;
    }
    printf("FAILED: %d check(s) failed across %d test(s)\n", tests_failed, tests_run);
    return 1;
}
