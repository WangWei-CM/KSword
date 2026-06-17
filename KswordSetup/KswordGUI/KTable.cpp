#include "KWidgets.h"

#include "KPaintDebug.h"

#include "fl_draw.H"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace {
constexpr int kTableFontSize = 12;

// DrawTableText renders non-empty table text inside a clipped cell rectangle.
void DrawTableText(const char* text, int x, int y, int w, int h, Fl_Color color, Fl_Align align) {
    if (!text || text[0] == '\0') {
        return;
    }
    fl_color(color);
    fl_font(FL_HELVETICA, kTableFontSize);
    fl_draw(text, x, y, w, h, align);
}

// DrawCellGrid paints the right and bottom dividers for one table rectangle.
void DrawCellGrid(int x, int y, int w, int h, Fl_Color color) {
    fl_color(color);
    fl_line(x + w - 1, y, x + w - 1, y + h);
    fl_line(x, y + h - 1, x + w, y + h - 1);
}

// SumColumnPixels returns the logical width occupied by all model columns.
// The input is the live table because FLTK may hold user-resized column widths;
// the output is clamped to non-negative pixels and has no side effects.
int SumColumnPixels(KTable* table) {
    if (!table) {
        return 0;
    }

    int total = 0;
    for (int col = 0; col < table->cols(); ++col) {
        total += std::max(0, table->col_width(col));
    }
    return total;
}

// SumRowPixels returns the logical height occupied by all model rows.
// It mirrors SumColumnPixels for row resize safety and returns zero for a null table.
int SumRowPixels(KTable* table) {
    if (!table) {
        return 0;
    }

    int total = 0;
    for (int row = 0; row < table->rows(); ++row) {
        total += std::max(0, table->row_height(row));
    }
    return total;
}

// FillRectIfVisible paints a rectangular fragment only when dimensions are valid.
// Input coordinates are already table-local screen coordinates; there is no return value.
void FillRectIfVisible(int x, int y, int w, int h, Fl_Color color) {
    if (w <= 0 || h <= 0) {
        return;
    }
    fl_color(color);
    fl_rectf(x, y, w, h);
}

// DrawTableDeadZone fills only the area not covered by real cells.
// CONTEXT_TABLE may be delivered during selection/focus updates before FLTK
// repaints just the changed cells. Clearing the whole context rectangle here
// erases other cells, so this helper restricts painting to bottom/right blank
// space or the whole body only when there is no cell model to draw.
void DrawTableDeadZone(KTable* table, int x, int y, int w, int h, Fl_Color color) {
    if (!table || w <= 0 || h <= 0) {
        return;
    }

    fl_push_clip(x, y, w, h);
    if (table->rows() <= 0 || table->cols() <= 0) {
        FillRectIfVisible(x, y, w, h, color);
        fl_pop_clip();
        return;
    }

    const int used_w = std::min(w, SumColumnPixels(table));
    const int used_h = std::min(h, SumRowPixels(table));

    // Right-side and bottom-side fills keep empty table space stable without
    // painting over any existing cell, header, or grid line.
    FillRectIfVisible(x + used_w, y, w - used_w, h, color);
    FillRectIfVisible(x, y + used_h, w, h - used_h, color);
    fl_pop_clip();
}
}

KTable::ContextMenuBinding::ContextMenuBinding()
    : callback(nullptr), user_data(nullptr) {
}

KTable::KTable(int x, int y, int w, int h, const char* label)
    : Fl_Table(x, y, w, h, label),
      col_labels_(),
      cells_(),
      row_context_menu_callbacks_(),
      cell_context_menu_callbacks_(),
      row_height_(24),
      col_width_(120),
      context_menu_x_(0),
      context_menu_y_(0) {
    cols(0);
    rows(0);
    col_header(1);
    row_header(0);
    col_resize(1);
    row_resize(0);
    col_header_height(26);
    row_height_all(row_height_);
    col_width_all(col_width_);
    const KTheme& theme = KThemeManager::instance().theme();
    // Match the original FlatTable wrapper: the table owns all background and
    // grid painting, so FLTK should not draw an extra box that can leave stale
    // pixels during partial selection redraws.
    box(FL_NO_BOX);
    color(COLOR_WINDOW_BG);
    selection_color(theme.selection);
    labelfont(FL_HELVETICA);
    labelsize(kTableFontSize);
    labelcolor(COLOR_TEXT);
}

void KTable::draw() {
    KPaintDebugTraceDraw("KTable", this);
    ensure_size(rows(), cols());
    Fl_Table::draw();
}

void KTable::draw_cell(TableContext context, int R, int C, int X, int Y, int W, int H) {
    const KTheme& theme = KThemeManager::instance().theme();
    switch (context) {
    case CONTEXT_STARTPAGE:
        fl_font(FL_HELVETICA, kTableFontSize);
        return;
    case CONTEXT_TABLE:
        // Never clear the whole table body from CONTEXT_TABLE. On focus or
        // selection damage, FLTK may call this context for a large rectangle
        // and then redraw only the changed cells; broad clearing is the direct
        // cause of "selected cell erases other cells".
        DrawTableDeadZone(this, X, Y, W, H, theme.windowBg);
        return;
    case CONTEXT_COL_HEADER:
        fl_push_clip(X, Y, W, H);
        fl_color(theme.primary);
        fl_rectf(X, Y, W, H);
        DrawTableText(col_header_label(C), X + 6, Y, W - 12, H, FL_WHITE, FL_ALIGN_LEFT | FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
        DrawCellGrid(X, Y, W, H, theme.border);
        fl_pop_clip();
        return;
    case CONTEXT_ROW_HEADER: {
        char row_label[32] = {};
        std::snprintf(row_label, sizeof(row_label), "%d", R + 1);
        fl_push_clip(X, Y, W, H);
        fl_color(theme.controlAltBg);
        fl_rectf(X, Y, W, H);
        DrawTableText(row_label, X + 4, Y, W - 8, H, theme.mutedText, FL_ALIGN_RIGHT | FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
        DrawCellGrid(X, Y, W, H, theme.border);
        fl_pop_clip();
        return;
    }
    case CONTEXT_CELL:
        fl_push_clip(X, Y, W, H);
        // Use the widget's current selection_color() like the historical
        // FlatTable implementation, so runtime theme refreshes and callers that
        // customize selection color both remain respected.
        fl_color(is_selected(R, C) ? selection_color() : (R % 2 == 0 ? theme.controlBg : theme.controlAltBg));
        fl_rectf(X, Y, W, H);
        DrawTableText(cell(R, C), X + 6, Y, W - 12, H, theme.text, FL_ALIGN_LEFT | FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
        DrawCellGrid(X, Y, W, H, theme.border);
        fl_pop_clip();
        return;
    default:
        break;
    }
    Fl_Table::draw_cell(context, R, C, X, Y, W, H);
}

int KTable::handle(int event) {
    if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
        context_menu_x_ = Fl::event_x_root();
        context_menu_y_ = Fl::event_y_root();
        int row = -1;
        int col = -1;
        ResizeFlag resize_flag = RESIZE_NONE;
        TableContext context = cursor2rowcol(row, col, resize_flag);
        if (context == CONTEXT_CELL && trigger_context_menu_callback(row, col)) {
            redraw();
            return 1;
        }
        if (context == CONTEXT_ROW_HEADER && trigger_context_menu_callback(row, -1)) {
            redraw();
            return 1;
        }
    }
    return Fl_Table::handle(event);
}

void KTable::set_size(int rows_value, int cols_value) {
    ensure_size(rows_value, cols_value);
    row_height_all(row_height_);
    col_width_all(col_width_);
    redraw();
}

void KTable::set_col_header_label(int col, const char* label) {
    if (col < 0) {
        return;
    }
    ensure_size(rows(), std::max(cols(), col + 1));
    col_labels_[static_cast<std::size_t>(col)] = label ? label : "";
    redraw();
}

const char* KTable::col_header_label(int col) const {
    if (col >= 0 && col < static_cast<int>(col_labels_.size())) {
        return col_labels_[static_cast<std::size_t>(col)].c_str();
    }
    return "";
}

void KTable::set_cell(int row, int col, const char* text) {
    if (row < 0 || col < 0) {
        return;
    }
    ensure_size(std::max(rows(), row + 1), std::max(cols(), col + 1));
    cells_[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)] = text ? text : "";
    redraw();
}

const char* KTable::cell(int row, int col) const {
    if (row >= 0 && row < static_cast<int>(cells_.size()) && col >= 0 && col < static_cast<int>(cells_[static_cast<std::size_t>(row)].size())) {
        return cells_[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)].c_str();
    }
    return "";
}

void KTable::set_row_height(int h) {
    if (h <= 0) {
        return;
    }
    row_height_ = h;
    row_height_all(row_height_);
    redraw();
}

void KTable::set_col_width(int width) {
    if (width <= 0) {
        return;
    }
    col_width_ = width;
    col_width_all(col_width_);
    redraw();
}

void KTable::set_row_context_menu_callback(int row, ContextMenuCallback callback, void* user_data) {
    if (row < 0) {
        return;
    }
    ensure_size(std::max(rows(), row + 1), cols());
    row_context_menu_callbacks_[static_cast<std::size_t>(row)].callback = callback;
    row_context_menu_callbacks_[static_cast<std::size_t>(row)].user_data = user_data;
}

void KTable::set_cell_context_menu_callback(int row, int col, ContextMenuCallback callback, void* user_data) {
    if (row < 0 || col < 0) {
        return;
    }
    ensure_size(std::max(rows(), row + 1), std::max(cols(), col + 1));
    cell_context_menu_callbacks_[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)].callback = callback;
    cell_context_menu_callbacks_[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)].user_data = user_data;
}

void KTable::context_menu_position(int& x_out, int& y_out) const {
    x_out = context_menu_x_;
    y_out = context_menu_y_;
}

void KTable::ensure_size(int rows_value, int cols_value) {
    if (rows_value < 0 || cols_value < 0) {
        return;
    }
    if (rows() != rows_value) {
        rows(rows_value);
    }
    if (cols() != cols_value) {
        cols(cols_value);
    }
    col_labels_.resize(static_cast<std::size_t>(cols_value));
    cells_.resize(static_cast<std::size_t>(rows_value));
    for (auto& row : cells_) {
        row.resize(static_cast<std::size_t>(cols_value));
    }
    row_context_menu_callbacks_.resize(static_cast<std::size_t>(rows_value));
    cell_context_menu_callbacks_.resize(static_cast<std::size_t>(rows_value));
    for (auto& callbacks : cell_context_menu_callbacks_) {
        callbacks.resize(static_cast<std::size_t>(cols_value));
    }
}

bool KTable::trigger_context_menu_callback(int row, int col) {
    if (row < 0 || row >= rows()) {
        return false;
    }
    ensure_size(rows(), cols());
    if (col >= 0 && col < cols()) {
        ContextMenuBinding& cell_binding = cell_context_menu_callbacks_[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)];
        if (cell_binding.callback) {
            cell_binding.callback(this, row, col, cell_binding.user_data);
            return true;
        }
    }
    ContextMenuBinding& row_binding = row_context_menu_callbacks_[static_cast<std::size_t>(row)];
    if (row_binding.callback) {
        row_binding.callback(this, row, col, row_binding.user_data);
        return true;
    }
    return false;
}
