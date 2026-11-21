#ifndef AOE1_PNL_EDIT_H
#define AOE1_PNL_EDIT_H

// This header owns TEditPanel: a text box the game draws itself (it used to
// wrap a Windows EDIT control).
#include "panel.h"
#include "drawarea.h"
#include "basegame.h"

class TEditPanel : public TPanel {
  public:
    enum FormatType {
        FormatText = 0,
        FormatNumber = 1,
        FormatInteger = 2,
        FormatUnsignedInt = 3,
        FormatFile = 4,
        FormatFileNoExt = 5,
        FormatPath = 6,
        FormatMultiLine = 7,
        FormatPercent = 8
    };
    TEditPanel();
    virtual ~TEditPanel();
    short fixed_len;
    unsigned char pad_FA[2];
    char* text;
    int text_len;
    HBRUSH brush;
    int stock_brush;
    unsigned long text_color1;
    unsigned long text_color2;
    unsigned long highlight_text_color1;
    unsigned long highlight_text_color2;
    unsigned long back_color;
    FormatType format;
    HGDIOBJ font;
    int multi_line;
    tagRECT draw_rect;
    unsigned int draw_format;
    int auto_sel;
    int sel_len;
    int bevel_type;
    unsigned char bevel_color1;
    unsigned char bevel_color2;
    unsigned char bevel_color3;
    unsigned char bevel_color4;
    unsigned char bevel_color5;
    unsigned char bevel_color6;
    unsigned char pad_15A[2];
    int bevel_size;
    int enable_ime;
    int turn_ime_on;
    // Editing state: byte offsets into text.
    int caret;
    int sel_anchor;       // the selection runs between sel_anchor and caret
    int scroll_pos;       // first byte shown, for a single line
    char* undo_text;      // the text when the box got focus, for Escape and Ctrl+Z
    unsigned long caret_time;
    int caret_shown;
    long setup(TDrawArea*, TPanel*, long, long, long, long, void*, short, char*, FormatType, int);
    void set_text(char*);
    void update_text();
    char* get_text();
    char* get_input_buffer();
    char* currentLine();
    virtual void set_active(int);
    virtual void set_focus(int);
    int verify_char(int);
    virtual void draw();
    virtual void paint();
    int is_blank();
    char* get_trimmed_str(char*, int);
    virtual void handle_reactivate();
    virtual long handle_mouse_down(unsigned char, long, long, int, int);
    virtual long handle_idle();
    virtual long char_action(long, short);
    virtual long key_down_action(long, short, int, int, int);
    virtual void set_rect(long, long, long, long);
    virtual void set_redraw(RedrawMode);
    void set_text_color(unsigned long, unsigned long);
    void set_highlight_text_color(unsigned long, unsigned long);
    void set_back_color(void*, unsigned long, unsigned char);
    void set_bevel_info(int, int, int, int, int, int, int);
    void set_ime_info(int, int);
};

#endif
