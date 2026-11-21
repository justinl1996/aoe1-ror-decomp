#include "../include/pnl_edit.h"
#include "../include/surface.h"
#include "../include/game_globals.h"
#include <stdlib.h>
#include <string.h>

#include <mbstring.h>

// TEditPanel is a text box the game draws itself. It used to wrap a Windows
// EDIT control, a real child window that Windows painted over the game's
// frame; with SDL presenting the frame that flickered, and the text never
// showed. The box now keeps its own text, caret and selection, takes keys
// through the panel system (key_down_action, char_action) like TInputPanel,
// and draws with the game's GDI layer (port/sdl/gdi.cpp).
//
// What the EDIT control did and the game relied on is kept: the length limit
// and the character checks of verify_char, Enter and Escape going to the
// parent panel (Escape undoing the edit when the parent does not take it),
// Tab moving focus, select-all on focus for auto_sel boxes, word wrap for
// FormatMultiLine, the clipboard keys, and white text on black while the box
// has focus.

enum { EDIT_CARET_BLINK_MS = 530 };

// pnl_edit.cpp lines 50-75.
TEditPanel::TEditPanel() : TPanel() {
    text = 0;
    fixed_len = 0;
    brush = 0;
    text_color1 = 0;
    text_color2 = 0;
    highlight_text_color1 = 0;
    highlight_text_color2 = 0;
    back_color = 0;
    sel_len = 0;
    bevel_color1 = 0;
    bevel_color2 = 0;
    bevel_color3 = 0;
    bevel_color4 = 0;
    bevel_color5 = 0;
    bevel_color6 = 0;
    bevel_size = 0;
    enable_ime = 0;
    handle_mouse_input = 1;
    bevel_type = 1;
    turn_ime_on = 0;
    caret = 0;
    sel_anchor = 0;
    scroll_pos = 0;
    undo_text = 0;
    caret_time = 0;
    caret_shown = 1;
}

// pnl_edit.cpp lines 83-112.
TEditPanel::~TEditPanel() {
    if (this->text != 0) {
        free(this->text);
        this->text = 0;
    }
    if (this->undo_text != 0) {
        free(this->undo_text);
        this->undo_text = 0;
    }
}

// pnl_edit.cpp lines 121-189.
long TEditPanel::setup(
    TDrawArea* render_area_in, TPanel* parent_panel_in, long x_in, long y_in, long wid_in, long hgt_in, void* font_in,
    short fixed_len_in, char* text_in, TEditPanel::FormatType format_in, int auto_sel_in
) {
    long result;

    this->format = format_in;
    if (fixed_len_in > 0) {
        fixed_len_in++;
    }
    this->fixed_len = fixed_len_in;
    this->font = font_in;
    this->auto_sel = auto_sel_in;

    result = TPanel::setup(render_area_in, parent_panel_in, x_in, y_in, wid_in, hgt_in, 0xFF);
    if (result == 0) {
        return 0;
    }

    this->text = (char*) calloc(this->fixed_len + 2, 1);
    this->undo_text = (char*) calloc(this->fixed_len + 2, 1);
    if (this->text == 0 || this->undo_text == 0) {
        return 0;
    }
    this->text_len = 0;
    if (text_in != 0 && *text_in != 0) {
        this->set_text(text_in);
    }
    this->brush = gdi_get_stock_object(BLACK_BRUSH);
    this->text_color1 = 0xFFFFFF;
    this->highlight_text_color1 = 0xFFFFFF;
    this->back_color = 0;
    this->text_color2 = 0;
    this->highlight_text_color2 = 0;
    this->draw_format = 0x2000;
    if (this->format != TEditPanel::FormatMultiLine)
        this->draw_format = 0x2020;
    else
        this->draw_format = 0x2010;
    return 1;
}

// ---------------------------------------------------------------------------
// Editing

// The longest text the box holds (the EDIT control's limit).
static int edit_max_len(TEditPanel* panel) {
    return panel->fixed_len - 1;
}

static int edit_sel_start(TEditPanel* panel) {
    return panel->caret < panel->sel_anchor ? panel->caret : panel->sel_anchor;
}

static int edit_sel_end(TEditPanel* panel) {
    return panel->caret > panel->sel_anchor ? panel->caret : panel->sel_anchor;
}

static void edit_changed(TEditPanel* panel) {
    panel->text_len = (int) strlen(panel->text);
    panel->sel_len = edit_sel_end(panel) - edit_sel_start(panel);
    panel->caret_shown = 1;
    panel->caret_time = debug_timeGetTime((char*) "pnl_edit.cpp", __LINE__);
    panel->set_redraw(TPanel::RedrawNormal);
}

static void edit_delete_selection(TEditPanel* panel) {
    int start = edit_sel_start(panel);
    int end = edit_sel_end(panel);
    if (start == end)
        return;
    memmove(panel->text + start, panel->text + end, strlen(panel->text + end) + 1);
    panel->caret = start;
    panel->sel_anchor = start;
}

// Inserts length bytes at the caret, replacing the selection. Returns 0
// (and beeps) when they do not fit.
static int edit_insert(TEditPanel* panel, const char* chars, int length) {
    int current = (int) strlen(panel->text) - (edit_sel_end(panel) - edit_sel_start(panel));
    if (current + length > edit_max_len(panel)) {
        MessageBeep((UINT) -1);
        return 0;
    }
    edit_delete_selection(panel);
    memmove(panel->text + panel->caret + length, panel->text + panel->caret, strlen(panel->text + panel->caret) + 1);
    memcpy(panel->text + panel->caret, chars, length);
    panel->caret += length;
    panel->sel_anchor = panel->caret;
    return 1;
}

static void edit_move(TEditPanel* panel, int to, int extend) {
    int length = (int) strlen(panel->text);
    if (to < 0)
        to = 0;
    if (to > length)
        to = length;
    panel->caret = to;
    if (extend == 0)
        panel->sel_anchor = to;
}

// One step left or right, treating "\r\n" as one character.
static int edit_step(TEditPanel* panel, int from, int direction) {
    if (direction < 0) {
        if (from >= 2 && panel->text[from - 1] == '\n' && panel->text[from - 2] == '\r')
            return from - 2;
        return from - 1;
    }
    if (panel->text[from] == '\r' && panel->text[from + 1] == '\n')
        return from + 2;
    return from + (panel->text[from] != 0 ? 1 : 0);
}

static void edit_copy(TEditPanel* panel) {
    int start = edit_sel_start(panel);
    int end = edit_sel_end(panel);
    if (start != end)
        surface_set_clipboard_text(panel->text + start, end - start);
}

static void edit_paste(TEditPanel* panel) {
    char pasted[1024];
    int length = surface_get_clipboard_text(pasted, sizeof(pasted));
    if (panel->format != TEditPanel::FormatMultiLine) {
        // A single line takes the clipboard up to its first line break.
        for (int i = 0; i < length; ++i) {
            if (pasted[i] == '\r' || pasted[i] == '\n') {
                length = i;
                break;
            }
        }
    }
    if (length > 0)
        edit_insert(panel, pasted, length);
}

// ---------------------------------------------------------------------------
// Layout: the lines the box shows, and where each byte is.

struct EditLine {
    int start;  // byte offset of the line's first character
    int end;    // byte offset just past its last shown character
};

enum { EDIT_MAX_LINES = 256 };

static int edit_text_width(HDC dc, const char* text, int length) {
    SIZE size;
    size.cx = 0;
    size.cy = 0;
    if (length > 0)
        gdi_get_text_extent_point32(dc, text, length, &size);
    return size.cx;
}

static int edit_line_height(HDC dc) {
    TEXTMETRICA metrics;
    if (gdi_get_text_metrics(dc, &metrics) == 0 || metrics.tmHeight <= 0)
        return 14;
    return metrics.tmHeight;
}

// Splits the text into lines: at "\r\n", and for multi-line boxes at spaces
// where a line would be wider than width (a longer word stays whole).
static int edit_layout(TEditPanel* panel, HDC dc, int width, EditLine* lines) {
    const char* text = panel->text;
    int length = (int) strlen(text);
    if (panel->format != TEditPanel::FormatMultiLine) {
        lines[0].start = 0;
        lines[0].end = length;
        return 1;
    }
    int count = 0;
    int start = 0;
    while (count < EDIT_MAX_LINES) {
        int end = start;
        while (end < length && text[end] != '\r' && text[end] != '\n')
            ++end;
        // Wrap this paragraph.
        int line_start = start;
        while (count < EDIT_MAX_LINES) {
            int cut = end;
            if (edit_text_width(dc, text + line_start, end - line_start) > width) {
                int last_space = -1;
                for (int i = line_start; i < end; ++i) {
                    if (text[i] != ' ')
                        continue;
                    if (edit_text_width(dc, text + line_start, i - line_start) > width)
                        break;
                    last_space = i;
                }
                if (last_space > line_start)
                    cut = last_space;
            }
            lines[count].start = line_start;
            lines[count].end = cut;
            ++count;
            if (cut >= end)
                break;
            line_start = cut + 1;
        }
        if (end >= length)
            break;
        start = end + ((text[end] == '\r' && text[end + 1] == '\n') ? 2 : 1);
        if (start > length)
            break;
    }
    if (count == 0) {
        lines[0].start = 0;
        lines[0].end = 0;
        count = 1;
    }
    return count;
}

static int edit_line_of(EditLine* lines, int count, int offset) {
    for (int i = count - 1; i > 0; --i)
        if (offset >= lines[i].start)
            return i;
    return 0;
}

// The byte in line nearest to pixel x (relative to the line's start).
static int edit_offset_at(HDC dc, const char* text, const EditLine* line, int x) {
    int best = line->start;
    int best_distance = x < 0 ? -x : x;
    for (int i = line->start + 1; i <= line->end; ++i) {
        int distance = edit_text_width(dc, text + line->start, i - line->start) - x;
        if (distance < 0)
            distance = -distance;
        if (distance <= best_distance) {
            best = i;
            best_distance = distance;
        }
    }
    return best;
}

// Runs fn with a device context holding the box's font, for measuring.
static int edit_with_dc(TEditPanel* panel, HDC* dc_out, HGDIOBJ* old_font) {
    HDC dc = (HDC) panel->render_area->GetDc("pnl_edit::measure");
    if (dc == 0)
        return 0;
    *old_font = gdi_select_object(dc, panel->font);
    *dc_out = dc;
    return 1;
}

static void edit_done_dc(TEditPanel* panel, HDC dc, HGDIOBJ old_font) {
    gdi_select_object(dc, old_font);
    panel->render_area->ReleaseDc("pnl_edit::measure");
}

// Moves the caret one line up or down in a multi-line box.
static void edit_move_line(TEditPanel* panel, int direction, int extend) {
    HDC dc;
    HGDIOBJ old_font;
    if (edit_with_dc(panel, &dc, &old_font) == 0)
        return;
    EditLine lines[EDIT_MAX_LINES];
    int count = edit_layout(panel, dc, panel->draw_rect.right - panel->draw_rect.left, lines);
    int line = edit_line_of(lines, count, panel->caret);
    int target = line + direction;
    if (target >= 0 && target < count) {
        int x = edit_text_width(dc, panel->text + lines[line].start, panel->caret - lines[line].start);
        edit_move(panel, edit_offset_at(dc, panel->text, &lines[target], x), extend);
    }
    edit_done_dc(panel, dc, old_font);
}

// ---------------------------------------------------------------------------
// The panel interface

// pnl_edit.cpp lines 195-215.
void TEditPanel::set_text(char* text_in) {
    if (this->text != 0) {
        if (text_in) {
            memset(this->text, 0, (short) fixed_len);
            strncpy(this->text, text_in, (short) fixed_len - 1);
            this->text[(short) fixed_len - 1] = 0;
            text_len = strlen(this->text);
        } else {
            this->text[0] = 0;
            text_len = 0;
        }
        this->caret = this->text_len;
        this->sel_anchor = this->caret;
        this->scroll_pos = 0;
        sel_len = 0;
        if (this->undo_text != 0)
            strcpy(this->undo_text, this->text);
    }
    this->set_redraw(TPanel::RedrawNormal);
}

// pnl_edit.cpp lines 221-266. The text is the box's own now.
void TEditPanel::update_text() {
    if (this->text != 0) {
        this->text_len = strlen(this->text);
        this->sel_len = edit_sel_end(this) - edit_sel_start(this);
    }
}

// pnl_edit.cpp lines 272-275.
char* TEditPanel::get_text() {
    this->update_text();
    return this->text;
}

// pnl_edit.cpp lines 281-284.
char* TEditPanel::get_input_buffer() {
    this->update_text();
    return this->text;
}

// pnl_edit.cpp lines 290-293.
char* TEditPanel::currentLine() {
    this->update_text();
    return this->text;
}

// pnl_edit.cpp lines 299-306.
void TEditPanel::set_active(int activ_in) {
    TPanel::set_active(activ_in);
}

// pnl_edit.cpp lines 314-395.
void TEditPanel::set_focus(int have_focus_in) {
    int old_focus = this->have_focus;
    TPanel::set_focus(have_focus_in);
    if (this->have_focus != 0 && old_focus == 0 && this->text != 0) {
        // What Escape and Ctrl+Z go back to.
        strcpy(this->undo_text, this->text);
        this->caret = (int) strlen(this->text);
        this->sel_anchor = this->auto_sel != 0 ? 0 : this->caret;
        this->caret_shown = 1;
        this->caret_time = debug_timeGetTime((char*) "pnl_edit.cpp", __LINE__);
    }
    if (old_focus != this->have_focus) {
        this->update_text();
        this->set_redraw(TPanel::RedrawNormal);
    }
    if (this->have_focus != 0) {
        panel_edit_pending_change = 0;
    }
}

// pnl_edit.cpp lines 401-421.
void TEditPanel::set_rect(long x_in, long y_in, long wid_in, long hgt_in) {
    TPanel::set_rect(x_in, y_in, wid_in, hgt_in);
    int format = this->format;
    if (format == 7) {
        this->draw_rect.left = this->render_rect.left + 1 + this->bevel_size;
        this->draw_rect.top = this->render_rect.top + this->bevel_size;
        this->draw_rect.right = this->render_rect.right - this->bevel_size - 1;
        this->draw_rect.bottom = this->render_rect.bottom - this->bevel_size;
    } else {
        this->draw_rect.left = this->render_rect.left + this->bevel_size + 8;
        this->draw_rect.top = this->render_rect.top + this->bevel_size;
        this->draw_rect.right = this->render_rect.right - this->bevel_size;
        this->draw_rect.bottom = this->render_rect.bottom - this->bevel_size;
    }
}

// pnl_edit.cpp lines 427-431.
void TEditPanel::set_redraw(TPanel::RedrawMode redraw_mode) {
    TPanel::set_redraw(TPanel::RedrawNormal);
}

// pnl_edit.cpp lines 437-441.
void TEditPanel::set_text_color(unsigned long val1, unsigned long val2) {
    text_color1 = val1;
    text_color2 = val2;
    set_redraw(TPanel::RedrawNormal);
}

// pnl_edit.cpp line 447.
void TEditPanel::set_highlight_text_color(unsigned long val1, unsigned long val2) {
}

// pnl_edit.cpp line 460.
void TEditPanel::set_back_color(void* brush_in, unsigned long brush_color_in, unsigned char color_in) {
}

// pnl_edit.cpp lines 474-509.
void TEditPanel::set_bevel_info(int bevel_type_in, int c1, int c2, int c3, int c4, int c5, int c6) {
    bevel_type = bevel_type_in;
    bevel_color1 = (unsigned char) c1;
    bevel_color2 = (unsigned char) c2;
    bevel_color3 = (unsigned char) c3;
    bevel_color4 = (unsigned char) c4;
    bevel_color5 = (unsigned char) c5;
    bevel_color6 = (unsigned char) c6;
    switch (bevel_type_in) {
        case 1:
        case 2:
        case 5:
            this->bevel_size = 1;
            break;
        case 3:
        case 6:
            this->bevel_size = 2;
            break;
        case 4:
        case 7:
            this->bevel_size = 3;
            break;
        default:
            this->bevel_size = 0;
            break;
    }
    long btm = this->pnl_hgt;
    long rgt = this->pnl_wid;
    long tp = this->pnl_y;
    long lft = this->pnl_x;
    this->set_rect(lft, tp, rgt, btm);
    this->set_redraw(TPanel::RedrawNormal);
}

// pnl_edit.cpp lines 515-518.
void TEditPanel::set_ime_info(int enable_ime_in, int turn_ime_on_in) {
    this->enable_ime = enable_ime_in;
    this->turn_ime_on = turn_ime_on_in;
}

// The keys the EDIT control handled. Everything except Tab (focus moves) and
// Alt combinations stays with the box while it has focus, as it did when the
// control had Windows' keyboard focus.
long TEditPanel::key_down_action(long key, short count, int alt_key, int ctrl_key, int shift_key) {
    if (this->have_focus == 0 || this->active == 0 || this->text == 0)
        return 0;
    if (alt_key != 0 || key == VK_TAB)
        return 0;
    int multi = this->format == TEditPanel::FormatMultiLine;
    switch (key) {
        case VK_RETURN:
            if (this->parent_panel != 0)
                this->parent_panel->action(this, 0, 0, 0);
            return 1;
        case VK_ESCAPE:
            if (this->parent_panel == 0 || this->parent_panel->action(this, 1, 0, 0) == 0) {
                strcpy(this->text, this->undo_text);
                edit_move(this, (int) strlen(this->text), 0);
                edit_changed(this);
            }
            return 1;
        case VK_LEFT:
            if (ctrl_key != 0) {
                int to = this->caret;
                while (to > 0 && this->text[to - 1] == ' ')
                    --to;
                while (to > 0 && this->text[to - 1] != ' ')
                    --to;
                edit_move(this, to, shift_key);
            } else if (shift_key == 0 && this->caret != this->sel_anchor) {
                edit_move(this, edit_sel_start(this), 0);
            } else {
                edit_move(this, edit_step(this, this->caret, -1), shift_key);
            }
            break;
        case VK_RIGHT:
            if (ctrl_key != 0) {
                int to = this->caret;
                while (this->text[to] != 0 && this->text[to] != ' ')
                    ++to;
                while (this->text[to] == ' ')
                    ++to;
                edit_move(this, to, shift_key);
            } else if (shift_key == 0 && this->caret != this->sel_anchor) {
                edit_move(this, edit_sel_end(this), 0);
            } else {
                edit_move(this, edit_step(this, this->caret, 1), shift_key);
            }
            break;
        case VK_UP:
        case VK_DOWN:
            if (multi != 0)
                edit_move_line(this, key == VK_UP ? -1 : 1, shift_key);
            break;
        case VK_HOME:
            if (multi != 0 && ctrl_key == 0) {
                int to = this->caret;
                while (to > 0 && this->text[to - 1] != '\n')
                    --to;
                edit_move(this, to, shift_key);
            } else {
                edit_move(this, 0, shift_key);
            }
            break;
        case VK_END:
            if (multi != 0 && ctrl_key == 0) {
                int to = this->caret;
                while (this->text[to] != 0 && this->text[to] != '\r' && this->text[to] != '\n')
                    ++to;
                edit_move(this, to, shift_key);
            } else {
                edit_move(this, (int) strlen(this->text), shift_key);
            }
            break;
        case VK_DELETE:
            if (this->caret == this->sel_anchor) {
                this->sel_anchor = edit_step(this, this->caret, 1);
                if (shift_key != 0)
                    edit_copy(this);
            } else if (shift_key != 0) {
                edit_copy(this);
            }
            edit_delete_selection(this);
            break;
        case VK_INSERT:
            if (shift_key != 0)
                edit_paste(this);
            else if (ctrl_key != 0)
                edit_copy(this);
            break;
        default:
            // Letters, digits and the rest arrive as characters.
            return 1;
    }
    edit_changed(this);
    return 1;
}

long TEditPanel::char_action(long key, short count) {
    if (this->have_focus == 0 || this->active == 0 || this->text == 0)
        return 0;
    switch (key) {
        case 1:   // Ctrl+A
            edit_move(this, 0, 0);
            edit_move(this, (int) strlen(this->text), 1);
            edit_changed(this);
            return 1;
        case 3:   // Ctrl+C
            edit_copy(this);
            return 1;
        case 24:  // Ctrl+X
            edit_copy(this);
            edit_delete_selection(this);
            edit_changed(this);
            return 1;
        case 22:  // Ctrl+V
            edit_paste(this);
            edit_changed(this);
            return 1;
        case 26:  // Ctrl+Z
            strcpy(this->text, this->undo_text);
            edit_move(this, (int) strlen(this->text), 0);
            edit_changed(this);
            return 1;
    }
    if (this->verify_char(key) == 0)
        return 1;
    if (key == '\b') {
        if (this->caret == this->sel_anchor)
            this->sel_anchor = edit_step(this, this->caret, -1);
        edit_delete_selection(this);
    } else if (key == '\r') {
        edit_insert(this, "\r\n", 2);
    } else if ((unsigned char) key >= 0x20) {
        char c = (char) key;
        edit_insert(this, &c, 1);
    } else {
        return 1;
    }
    edit_changed(this);
    return 1;
}

long TEditPanel::handle_idle() {
    if (this->have_focus != 0 && this->active != 0 && this->visible != 0) {
        unsigned long now = debug_timeGetTime((char*) "pnl_edit.cpp", __LINE__);
        if (now - this->caret_time >= EDIT_CARET_BLINK_MS) {
            this->caret_shown = !this->caret_shown;
            this->caret_time = now;
            this->set_redraw(TPanel::RedrawNormal);
        }
    }
    return TPanel::handle_idle();
}

// pnl_edit.cpp lines 760-853.
int TEditPanel::verify_char(int key) {
    if (panel_edit_pending_change != 0) {
        panel_edit_pending_change = 0;
        return 0;
    }
    if (key == '\t')
        return 0;
    if (key == '\r' && this->format != 7)
        return 0;
    if (key == '\b')
        return 1;
    this->update_text();
    if (this->sel_len == 0) {
        if (this->text_len >= this->fixed_len - 1) {
            MessageBeep((UINT) -1);
            return 0;
        }
    }
    int validation_result;
    switch (this->format) {
        case 0:
            goto valid;
        case 8:
            if (this->text_len < 2) {
                if (key >= '0' && key <= '9')
                    goto valid;
            }
            if (this->text_len != 2 || key != '0')
                goto invalid;
            validation_result = _mbscmp((const unsigned char*) "10", (const unsigned char*) this->text);
            break;
        case 1:
        case 2:
        case 3:
            this->update_text();
            if (key >= '0' && key <= '9')
                goto valid;
            if (key == '-' && this->text[0] == 0 && this->format != 3)
                goto valid;
            if (key != '.' || _mbschr((const unsigned char*) this->text, '.') == 0)
                goto invalid;
            if (this->format == 1)
                goto valid;
            MessageBeep((UINT) -1);
            return 0;
        case 4:
        case 5:
            this->update_text();
            if (key == '.' && this->format == 4 && _mbschr((const unsigned char*) this->text, '.') == 0) {
                goto valid;
            }
            if (key == '.')
                goto invalid;
            if (key == '\\')
                goto invalid;
            if (key == ':')
                goto invalid;
            goto valid_chars;
        case 6:
            this->update_text();
            if (key == ':' && this->text_len != 1)
                goto invalid;
            goto valid_chars;
        default:
            goto valid;
    }
    if (validation_result == 0)
        goto valid;
    MessageBeep((UINT) -1);
    return 0;
valid_chars:
    if (key != '/' && key != '"' && key != '*' && key != '?' && key != '>' && key != '<' && key != '|')
        goto valid;
    goto invalid;
invalid:
    MessageBeep((UINT) -1);
    return 0;
valid:
    return 1;
}

// Draws the focused box: its lines in highlight_text_color1, the selection
// inverted, and the caret.
static void edit_draw_focused(TEditPanel* panel, HDC dc) {
    RECT area = panel->draw_rect;
    int width = area.right - area.left;
    int line_height = edit_line_height(dc);
    EditLine lines[EDIT_MAX_LINES];
    int count = edit_layout(panel, dc, width, lines);
    int sel_start = edit_sel_start(panel);
    int sel_end = edit_sel_end(panel);
    int caret_line = edit_line_of(lines, count, panel->caret);

    // A single line scrolls sideways to keep the caret in view.
    int first = 0;
    if (panel->format != TEditPanel::FormatMultiLine) {
        if (panel->scroll_pos > panel->caret)
            panel->scroll_pos = panel->caret;
        while (panel->scroll_pos < panel->caret
               && edit_text_width(dc, panel->text + panel->scroll_pos, panel->caret - panel->scroll_pos) > width - 2)
            ++panel->scroll_pos;
        lines[0].start = panel->scroll_pos;
    }
    // Multi-line boxes scroll down to keep the caret's line in view.
    int visible = (area.bottom - area.top) / (line_height > 0 ? line_height : 1);
    if (visible < 1)
        visible = 1;
    if (caret_line >= visible)
        first = caret_line - visible + 1;

    gdi_select_clip_rgn(dc, (HRGN) panel->clip_rgn);
    for (int i = first; i < count && (i - first) * line_height < area.bottom - area.top; ++i) {
        int y = area.top + (i - first) * line_height;
        int x = area.left;
        const char* text = panel->text;
        int start = lines[i].start;
        int end = lines[i].end;
        // The line in three runs: before, inside and after the selection.
        int cuts[4];
        cuts[0] = start;
        cuts[1] = sel_start < start ? start : (sel_start > end ? end : sel_start);
        cuts[2] = sel_end < start ? start : (sel_end > end ? end : sel_end);
        cuts[3] = end;
        for (int run = 0; run < 3; ++run) {
            int length = cuts[run + 1] - cuts[run];
            if (length <= 0)
                continue;
            if (run == 1) {
                gdi_set_bk_mode(dc, OPAQUE);
                gdi_set_bk_color(dc, panel->highlight_text_color1);
                gdi_set_text_color(dc, panel->back_color);
            } else {
                gdi_set_bk_mode(dc, TRANSPARENT);
                gdi_set_text_color(dc, panel->highlight_text_color1);
            }
            gdi_text_out(dc, x, y, text + cuts[run], length);
            x += edit_text_width(dc, text + cuts[run], length);
        }
        if (i == caret_line && panel->caret_shown != 0) {
            int caret_x = area.left + edit_text_width(dc, text + start, panel->caret - start);
            HPEN pen = gdi_create_pen(PS_SOLID, 1, panel->highlight_text_color1);
            HGDIOBJ old_pen = gdi_select_object(dc, pen);
            gdi_move_to_ex(dc, caret_x, y, 0);
            gdi_line_to(dc, caret_x, y + line_height);
            gdi_select_object(dc, old_pen);
            gdi_delete_object(pen);
        }
    }
    gdi_select_clip_rgn(dc, 0);
}

// pnl_edit.cpp lines 859-973.
void TEditPanel::draw() {
    this->need_redraw = 0;
    if (this->visible == 0) {
        return;
    }

    this->draw_setup(0);

    if (this->brush != 0 && this->active != 0 && this->have_focus != 0) {
        unsigned char temp_color =
            (unsigned char) GetNearestPaletteIndex((HPALETTE) this->render_area->DrawSystem->Pal, this->back_color);
        this->render_area->Clear(&this->clip_rect, temp_color);
    } else if (this->parent_panel != 0) {
        if (this->bevel_type >= 2 && this->bevel_type <= 7) {
            this->parent_panel->draw_rect2(&this->clip_rect);
        } else if (this->parent_panel->just_drawn == 0) {
            this->parent_panel->draw_rect(&this->clip_rect);
        }
    } else {
        this->render_area->Clear(&this->clip_rect, this->color);
    }

    if (this->bevel_type != 0) {
        if (this->render_area->Lock("pnl_edit::draw", 1) != 0) {
            switch (this->bevel_type) {
                case 1:
                    this->render_area->DrawRect(
                        this->pnl_x, this->pnl_y, this->pnl_x + this->pnl_wid - 1, this->pnl_y + this->pnl_hgt - 1,
                        this->bevel_color1
                    );
                    break;
                case 2:
                    this->render_area->DrawBevel(
                        this->pnl_x, this->pnl_y, this->pnl_x + this->pnl_wid - 1, this->pnl_y + this->pnl_hgt - 1,
                        this->bevel_color6, this->bevel_color1
                    );
                    break;
                case 3:
                    this->render_area->DrawBevel2(
                        this->pnl_x, this->pnl_y, this->pnl_x + this->pnl_wid - 1, this->pnl_y + this->pnl_hgt - 1,
                        this->bevel_color6, this->bevel_color5, this->bevel_color2, this->bevel_color1
                    );
                    break;
                case 4:
                    this->render_area->DrawBevel3(
                        this->pnl_x, this->pnl_y, this->pnl_x + this->pnl_wid - 1, this->pnl_y + this->pnl_hgt - 1,
                        this->bevel_color6, this->bevel_color5, this->bevel_color4, this->bevel_color3, this->bevel_color2,
                        this->bevel_color1
                    );
                    break;
                case 5:
                    this->render_area->DrawBevel(
                        this->pnl_x, this->pnl_y, this->pnl_x + this->pnl_wid - 1, this->pnl_y + this->pnl_hgt - 1,
                        this->bevel_color1, this->bevel_color6
                    );
                    break;
                case 6:
                    this->render_area->DrawBevel2(
                        this->pnl_x, this->pnl_y, this->pnl_x + this->pnl_wid - 1, this->pnl_y + this->pnl_hgt - 1,
                        this->bevel_color1, this->bevel_color2, this->bevel_color5, this->bevel_color6
                    );
                    break;
                case 7:
                    this->render_area->DrawBevel3(
                        this->pnl_x, this->pnl_y, this->pnl_x + this->pnl_wid - 1, this->pnl_y + this->pnl_hgt - 1,
                        this->bevel_color1, this->bevel_color2, this->bevel_color3, this->bevel_color4, this->bevel_color5,
                        this->bevel_color6
                    );
                    break;
            }
        }
        this->render_area->Unlock("pnl_edit::draw");
    }

    if (this->active != 0 && this->text != 0) {
        HDC dc = (HDC) this->render_area->GetDc("pnl_edit::draw");
        if (dc != 0) {
            HGDIOBJ old_font = gdi_select_object(dc, this->font);
            if (this->have_focus != 0) {
                edit_draw_focused(this, dc);
            } else {
                gdi_select_clip_rgn(dc, (HRGN) this->clip_rgn);
                gdi_set_bk_mode(dc, TRANSPARENT);
                gdi_set_text_color(dc, this->text_color2);

                tagRECT r;
                r.left = this->draw_rect.left - 1;
                r.top = this->draw_rect.top + 1;
                r.right = this->draw_rect.right - 1;
                r.bottom = this->draw_rect.bottom + 1;
                gdi_draw_text_ex(dc, this->text, -1, &r, this->draw_format, 0);

                gdi_set_text_color(dc, this->text_color1);
                gdi_draw_text_ex(dc, this->text, -1, &this->draw_rect, this->draw_format, 0);
                gdi_select_clip_rgn(dc, 0);
            }
            gdi_select_object(dc, old_font);
            this->render_area->ReleaseDc("pnl_edit::draw");
        }
    }

    this->draw_finish();
}

// pnl_edit.cpp line 979.
void TEditPanel::paint() {
}

// pnl_edit.cpp lines 987-1011.
int TEditPanel::is_blank() {
    char* p;
    this->update_text();
    p = this->text;
    if (p == 0)
        goto blank;
    if (*p == 0)
        goto blank;
    while (1) {
        if (*p == '\0')
            goto blank;
        if (_ismbcspace((unsigned char) *p) == 0)
            return 0;
        p = (char*) _mbsinc((const unsigned char*) p);
    }
blank:
    return 1;
}

// pnl_edit.cpp lines 1017-1054.
char* TEditPanel::get_trimmed_str(char* str, int str_len) {
    char temp_str[256];
    unsigned char* base;
    unsigned char* p;
    unsigned char* cur;
    this->update_text();
    memset(temp_str, 0, sizeof(temp_str));
    strcpy(temp_str, this->text);
    memset(str, 0, str_len);
    base = (unsigned char*) str;
    p = base;
    cur = (unsigned char*) temp_str;
    while (_ismbcspace(*cur & 0xFF))
        cur = _mbsinc(cur);
    while (*cur) {
        _mbsncpy(p, cur, 1);
        p = _mbsinc(p);
        cur = _mbsinc(cur);
    }
    while (1) {
        *p = 0;
        if (p == base)
            break;
        p = _mbsdec(base, p);
        if (_ismbcspace(*p))
            continue;
        break;
    }
    return str;
}

// pnl_edit.cpp lines 1060-1067.
void TEditPanel::handle_reactivate() {
    this->set_redraw(TPanel::RedrawNormal);
}

// pnl_edit.cpp lines 1074-1077. A click in the focused box also moves the
// caret there.
long TEditPanel::handle_mouse_down(unsigned char mouse_button_in, long x, long y, int ctrl_key, int shift_key) {
    long result = TPanel::handle_mouse_down(mouse_button_in, x, y, ctrl_key, shift_key);
    if (this->have_focus != 0 && this->text != 0 && this->is_inside(x, y) != 0) {
        HDC dc;
        HGDIOBJ old_font;
        if (edit_with_dc(this, &dc, &old_font) != 0) {
            EditLine lines[EDIT_MAX_LINES];
            int count = edit_layout(this, dc, this->draw_rect.right - this->draw_rect.left, lines);
            if (this->format != TEditPanel::FormatMultiLine)
                lines[0].start = this->scroll_pos;
            int line = 0;
            if (this->format == TEditPanel::FormatMultiLine) {
                line = (y - this->draw_rect.top) / edit_line_height(dc);
                if (line < 0)
                    line = 0;
                if (line >= count)
                    line = count - 1;
            }
            edit_move(this, edit_offset_at(dc, this->text, &lines[line], x - this->draw_rect.left), shift_key);
            edit_done_dc(this, dc, old_font);
            edit_changed(this);
        }
    }
    return result;
}
