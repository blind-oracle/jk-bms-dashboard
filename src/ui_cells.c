// Cells page: spread header, then up to 24 cells in column-major order
// (2 columns up to 16 cells, 3 columns for 17-24). Min cell blue, max red.
#include "esp_log.h"
#include "ui_internal.h"

static const char *TAG = "ui_cells";

#define HDR_X       4
#define HDR_Y       18
#define HDR_Y_14    (HDR_Y + 5) // 14 px fallback, on the 20 px baseline
#define HDR_W       232
#define ROWS        8
#define ROW_Y0      42
#define ROW_PITCH   24
#define MARK_MIN_DELTA_MV 5     // color min/max only from this spread

// 2 columns (n <= 16): 20 px index right-aligned in 26 px, value 32 px further
#define C2_X0       4
#define C2_PITCH    120
#define C2_IDX_W    26
#define C2_VAL_DX   32
#define C2_VAL_W    80
// 3 columns (n 17-24): 14 px index right-aligned in 18 px, value at x+21
#define C3_X0       1
#define C3_PITCH    80
#define C3_IDX_W    18
#define C3_IDX_DY   5           // puts the 14 px index on the 20 px value baseline
#define C3_VAL_DX   21
#define C3_VAL_W    57

#define EMPTY_Y     110

static struct {
    lv_obj_t *header;
    lv_obj_t *idx[JK_MAX_CELLS];
    lv_obj_t *val[JK_MAX_CELLS];
    lv_obj_t *empty;
    unsigned cols;              // current layout, 0 = none yet
} s;

static void layout(unsigned cols)
{
    for (unsigned i = 0; i < JK_MAX_CELLS; i++) {
        int32_t col = (int32_t)(i / ROWS);
        int32_t y = ROW_Y0 + ROW_PITCH * (int32_t)(i % ROWS);
        if (cols == 3) {
            int32_t x = C3_X0 + C3_PITCH * col;
            ui_set_font(s.idx[i], UI_FONT_14);
            lv_obj_set_pos(s.idx[i], x, y + C3_IDX_DY);
            lv_obj_set_size(s.idx[i], C3_IDX_W, lv_font_get_line_height(UI_FONT_14));
            lv_obj_set_pos(s.val[i], x + C3_VAL_DX, y);
            lv_obj_set_width(s.val[i], C3_VAL_W);
        } else if (i < 2 * ROWS) {      // cells 17-24 stay hidden in this layout
            int32_t x = C2_X0 + C2_PITCH * col;
            ui_set_font(s.idx[i], UI_FONT_20);
            lv_obj_set_pos(s.idx[i], x, y);
            lv_obj_set_size(s.idx[i], C2_IDX_W, lv_font_get_line_height(UI_FONT_20));
            lv_obj_set_pos(s.val[i], x + C2_VAL_DX, y);
            lv_obj_set_width(s.val[i], C2_VAL_W);
        }
    }
    s.cols = cols;
    ESP_LOGD(TAG, "layout: %u columns", cols);
}

lv_obj_t *ui_cells_create(void)
{
    lv_obj_t *scr = ui_screen_create();

    s.header = ui_label_create(scr, UI_FONT_20, HDR_X, HDR_Y, HDR_W, LV_TEXT_ALIGN_LEFT);
    lv_label_set_recolor(s.header, true);   // " BAL" in blue

    for (unsigned i = 0; i < JK_MAX_CELLS; i++) {
        // Positions and the index font are set by layout()
        s.idx[i] = ui_label_create(scr, UI_FONT_20, 0, 0, C2_IDX_W, LV_TEXT_ALIGN_RIGHT);
        ui_label_setf(s.idx[i], "%02u", i + 1);
        ui_set_text_color(s.idx[i], UI_COLOR_DIM);
        s.val[i] = ui_label_create(scr, UI_FONT_20, 0, 0, C2_VAL_W, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_hidden(s.idx[i], true);
        lv_obj_set_hidden(s.val[i], true);
    }

    s.empty = ui_label_create(scr, UI_FONT_20, 0, EMPTY_Y, UI_SCREEN_W, LV_TEXT_ALIGN_CENTER);
    ui_label_set(s.empty, "No cell data");
    ui_set_text_color(s.empty, UI_COLOR_DIM);

    s.cols = 0;
    layout(2);
    return scr;
}

// 20 px, or 14 px when the text does not fit: a spread of 100 mV or more
// while balancing is 237+ px at 20 px, and digits must never be clipped.
static void header_fit(const char *plain)
{
    lv_point_t size;
    lv_text_get_size(&size, plain, UI_FONT_20, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const lv_font_t *font = size.x <= HDR_W ? UI_FONT_20 : UI_FONT_14;
    if (lv_obj_get_style_text_font(s.header, LV_PART_MAIN) == font) {
        return;
    }
    lv_obj_set_style_text_font(s.header, font, LV_PART_MAIN);
    lv_obj_set_y(s.header, font == UI_FONT_20 ? HDR_Y : HDR_Y_14);
    lv_obj_set_height(s.header, lv_font_get_line_height(font));
}

void ui_cells_update(const ui_view_t *v)
{
    const jk_data_t *d = v->d;
    unsigned n = 0;
    if (v->has_data && (d->present & JK_HAS_CELLS)) {
        n = d->cell_count > JK_MAX_CELLS ? JK_MAX_CELLS : d->cell_count;
    }
    unsigned cols = n > 2 * ROWS ? 3 : 2;
    if (cols != s.cols) {
        layout(cols);
    }

    // "3.281-3.312 diff 31" plus " BAL" while balancing (blue via recolor)
    char plain[64];
    char shown[80];
    ui_str_t str;
    ui_str_init(&str, plain, sizeof(plain));
    bool range = n > 0 && ui_str_add_cell_range(&str, d);
    if (range) {
        ui_str_add(&str, " diff %u", (unsigned)d->cell_delta_mv);
    } else {
        ui_str_add(&str, UI_NA);
    }
    bool bal = v->has_data && (d->present & JK_HAS_STATUS) && (d->status & JK_STATUS_BALANCING);
    ui_str_t sh;
    ui_str_init(&sh, shown, sizeof(shown));
    ui_str_add(&sh, "%s%s", plain, (bal && v->live) ? " " UI_RECOLOR_BLUE "BAL#" : "");
    if (bal) {
        ui_str_add(&str, " BAL");
        if (!v->live) {
            ui_str_add(&sh, " BAL");    // greyed with the rest
        }
    }
    if (ui_label_set(s.header, shown)) {
        header_fit(plain);
    }
    ui_set_text_color(s.header, range ? ui_live_color(v, UI_COLOR_TEXT) : UI_COLOR_DIM);

    bool mark = v->live && range && d->cell_delta_mv >= MARK_MIN_DELTA_MV;
    for (unsigned i = 0; i < JK_MAX_CELLS; i++) {
        bool visible = i < n;
        ui_set_hidden(s.idx[i], !visible);
        ui_set_hidden(s.val[i], !visible);
        if (!visible) {
            continue;
        }
        unsigned mv = d->cell_mv[i];
        uint32_t color = UI_COLOR_DIM;
        if (mv == 0) {
            ui_label_set(s.val[i], UI_NA);
        } else {
            ui_label_setf(s.val[i], "%u.%03u", mv / 1000, mv % 1000);
            color = UI_COLOR_TEXT;
            if (mark && i == d->cell_max_idx) {
                color = UI_COLOR_RED;
            } else if (mark && i == d->cell_min_idx) {
                color = UI_COLOR_BLUE;
            }
            color = ui_live_color(v, color);
        }
        ui_set_text_color(s.val[i], color);
    }
    ui_set_hidden(s.empty, n > 0);
}
