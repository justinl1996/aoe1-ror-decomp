// GDI for the game's 8-bit surfaces, drawn with SDL_ttf
// (workflow/docs/sdl_port_tasks.md, "Text without GDI").
//
// The game draws text, and a few lines, through GDI calls on a device context
// it gets from a DirectDraw surface (TDrawArea::GetDc). Those surfaces are
// plain 8-bit memory now (port/sdl/ddraw_memory.cpp), and their device
// contexts are objects of this file. The gdi_* functions stand in for the
// GDI calls the game makes: on one of these contexts they draw into the
// surface's pixels; on any other device context (a window's, the EDIT
// boxes') they are the Win32 calls.
//
// Every GDI object the game creates goes through here too (fonts, clip
// regions, pens, stock pens and brushes), so the drawing code can read them
// without GDI. On Windows each object can also produce a real GDI twin, for
// when the game hands it to a real window or control (gdi_win32_object).
//
// Drawing follows GDI on a palettized surface: text is not anti-aliased,
// colours become the nearest palette entry (lowest index on a tie), OPAQUE
// fills the text cell with the background colour, LineTo leaves out the end
// point, and everything is clipped to the selected clip rectangle.

#include "../../include/surface.h"
#include "sdl_internal.h"

#include <SDL_ttf.h>
#include <stdlib.h>
#include <string.h>

enum GdiKind { GDI_DC = 1, GDI_FONT, GDI_PEN, GDI_BRUSH, GDI_REGION, GDI_PALETTE };

struct GdiObject {
    int kind;
    int stock;                // stock objects are never freed
    HGDIOBJ win32;            // the real GDI twin, made on demand (Windows)
    GdiObject* next;          // every live object, to tell ours from GDI's
};

struct GdiFont {
    GdiObject head;
    LOGFONTA logfont;
    TTF_Font* ttf;
};

struct GdiPen {
    GdiObject head;
    int style;
    int width;
    COLORREF color;
};

struct GdiBrush {
    GdiObject head;
    int style;                // BS_SOLID or BS_HOLLOW
    COLORREF color;
};

struct GdiRegion {
    GdiObject head;
    RECT rect;
};

struct GdiPalette {
    GdiObject head;
    int count;
    PALETTEENTRY* entries;
};

struct GdiDc {
    GdiObject head;
    unsigned char* bits;
    long pitch;
    long width;
    long height;
    const PALETTEENTRY* palette;  // 256 entries, or 0
    GdiFont* font;
    GdiPen* pen;
    GdiBrush* brush;
    COLORREF text_color;
    COLORREF bk_color;
    int bk_mode;
    int has_clip;
    RECT clip;
    POINT position;
};

static GdiObject* gdi_objects = 0;

static void track(GdiObject* object, int kind) {
    object->kind = kind;
    object->stock = 0;
    object->win32 = 0;
    object->next = gdi_objects;
    gdi_objects = object;
}

static void untrack(GdiObject* object) {
    GdiObject** link = &gdi_objects;
    while (*link != 0) {
        if (*link == object) {
            *link = object->next;
            return;
        }
        link = &(*link)->next;
    }
}

static GdiObject* lookup(const void* handle, int kind) {
    if (handle == 0)
        return 0;
    for (GdiObject* object = gdi_objects; object != 0; object = object->next)
        if (object == handle)
            return object->kind == kind || kind == 0 ? object : 0;
    return 0;
}

static GdiDc* our_dc(HDC dc) {
    return (GdiDc*) lookup(dc, GDI_DC);
}

// ---------------------------------------------------------------------------
// Fonts

static int ttf_ready = 0;

static int ttf_init(void) {
    if (ttf_ready == 0)
        ttf_ready = TTF_Init() == 0 ? 1 : -1;
    return ttf_ready == 1;
}

// Font files for a face: the real Windows font first, then the free
// stand-in shipped in fonts/ next to the executable.
struct FontChoice {
    const char* face;
    const char* regular[2];
    const char* bold[2];
};

static const FontChoice font_choices[] = {
    {"Arial", {"arial.ttf", "LiberationSans-Regular.ttf"}, {"arialbd.ttf", "LiberationSans-Bold.ttf"}},
    {"Copperplate Gothic Light", {"COPRGTL.TTF", "LiberationSans-Regular.ttf"}, {"COPRGTB.TTF", "LiberationSans-Bold.ttf"}},
    {"Times New Roman", {"times.ttf", "LiberationSerif-Regular.ttf"}, {"timesbd.ttf", "LiberationSerif-Bold.ttf"}},
    {"Comic Sans MS", {"comic.ttf", "LiberationSans-Regular.ttf"}, {"comicbd.ttf", "LiberationSans-Bold.ttf"}},
};

static TTF_Font* open_font_file(const char* name, int pixels) {
    char path[MAX_PATH];
    char* base = SDL_GetBasePath();
    if (base != 0) {
        SDL_snprintf(path, sizeof(path), "%sfonts%c%s", base, base[SDL_strlen(base) - 1], name);
        SDL_free(base);
        TTF_Font* font = TTF_OpenFont(path, pixels);
        if (font != 0)
            return font;
    }
#ifdef _WIN32
    char windows[MAX_PATH];
    if (GetWindowsDirectoryA(windows, sizeof(windows)) != 0) {
        SDL_snprintf(path, sizeof(path), "%s\\Fonts\\%s", windows, name);
        return TTF_OpenFont(path, pixels);
    }
#endif
    return 0;
}

// Opens the font for a face and weight; *synthetic_bold is set when only the
// regular file was found for a bold request.
static TTF_Font* open_font(const char* face, int bold, int pixels, int* synthetic_bold) {
    const FontChoice* choice = &font_choices[0];
    for (unsigned i = 0; i < sizeof(font_choices) / sizeof(font_choices[0]); ++i)
        if (SDL_strcasecmp(face, font_choices[i].face) == 0)
            choice = &font_choices[i];

    *synthetic_bold = 0;
    const char* const* files = bold != 0 ? choice->bold : choice->regular;
#ifdef _WIN32
    // On Windows the real font wins; elsewhere only the shipped ones exist.
    for (int i = 0; i < 2; ++i) {
        TTF_Font* font = open_font_file(files[i], pixels);
        if (font != 0)
            return font;
    }
#else
    TTF_Font* found = open_font_file(files[1], pixels);
    if (found != 0)
        return found;
#endif
    if (bold != 0) {
        *synthetic_bold = 1;
        for (int i = 0; i < 2; ++i) {
            TTF_Font* font = open_font_file(choice->regular[i], pixels);
            if (font != 0)
                return font;
        }
    }
    return 0;
}

static GdiFont* make_font(const LOGFONTA* logfont) {
    if (ttf_init() == 0)
        return 0;
    // A negative height is the character (em) height in pixels, which is
    // what TTF_OpenFont's point size means at 72 dpi; a positive one is the
    // cell height, a little larger.
    int pixels = logfont->lfHeight < 0 ? -logfont->lfHeight : logfont->lfHeight * 7 / 8;
    if (pixels <= 0)
        pixels = 13;
    int bold = logfont->lfWeight >= FW_BOLD;
    int synthetic_bold;
    TTF_Font* ttf = open_font(logfont->lfFaceName, bold, pixels, &synthetic_bold);
    if (ttf == 0)
        return 0;
    int style = TTF_STYLE_NORMAL;
    if (synthetic_bold != 0)
        style |= TTF_STYLE_BOLD;
    if (logfont->lfItalic != 0)
        style |= TTF_STYLE_ITALIC;
    if (logfont->lfUnderline != 0)
        style |= TTF_STYLE_UNDERLINE;
    if (logfont->lfStrikeOut != 0)
        style |= TTF_STYLE_STRIKETHROUGH;
    TTF_SetFontStyle(ttf, style);
    TTF_SetFontHinting(ttf, TTF_HINTING_MONO);
    TTF_SetFontKerning(ttf, 0); // GDI's TextOut does not kern

    GdiFont* font = (GdiFont*) calloc(1, sizeof(GdiFont));
    if (font == 0) {
        TTF_CloseFont(ttf);
        return 0;
    }
    font->logfont = *logfont;
    font->ttf = ttf;
    track(&font->head, GDI_FONT);
    return font;
}

// The font a context draws with: the selected one, or GDI's default stand-in.
static GdiFont* dc_font(GdiDc* dc) {
    static GdiFont* fallback = 0;
    if (dc->font != 0)
        return dc->font;
    if (fallback == 0) {
        LOGFONTA logfont;
        memset(&logfont, 0, sizeof(logfont));
        logfont.lfHeight = -13;
        logfont.lfWeight = FW_BOLD;
        strcpy(logfont.lfFaceName, "Arial");
        fallback = make_font(&logfont);
        if (fallback != 0)
            fallback->head.stock = 1;
    }
    return fallback;
}

HFONT gdi_create_font_indirect(const LOGFONTA* logfont) {
    return (HFONT) make_font(logfont);
}

// ---------------------------------------------------------------------------
// Pens, brushes, regions, stock objects

static GdiPen* make_pen(int style, int width, COLORREF color) {
    GdiPen* pen = (GdiPen*) calloc(1, sizeof(GdiPen));
    if (pen == 0)
        return 0;
    pen->style = style;
    pen->width = width;
    pen->color = color;
    track(&pen->head, GDI_PEN);
    return pen;
}

static GdiBrush* make_brush(int style, COLORREF color) {
    GdiBrush* brush = (GdiBrush*) calloc(1, sizeof(GdiBrush));
    if (brush == 0)
        return 0;
    brush->style = style;
    brush->color = color;
    track(&brush->head, GDI_BRUSH);
    return brush;
}

HPEN gdi_create_pen(int style, int width, COLORREF color) {
    return (HPEN) make_pen(style, width, color);
}

HRGN gdi_create_rect_rgn(int left, int top, int right, int bottom) {
    GdiRegion* region = (GdiRegion*) calloc(1, sizeof(GdiRegion));
    if (region == 0)
        return 0;
    region->rect.left = left < right ? left : right;
    region->rect.right = left < right ? right : left;
    region->rect.top = top < bottom ? top : bottom;
    region->rect.bottom = top < bottom ? bottom : top;
    track(&region->head, GDI_REGION);
    return (HRGN) region;
}

HGDIOBJ gdi_get_stock_object(int index) {
    static GdiObject* stock[DEFAULT_PALETTE + 1];
    if (index < 0 || index > DEFAULT_PALETTE)
#ifdef _WIN32
        return GetStockObject(index);
#else
        return 0;
#endif
    if (stock[index] == 0) {
        GdiObject* object = 0;
        switch (index) {
            case WHITE_BRUSH: object = (GdiObject*) make_brush(BS_SOLID, RGB(255, 255, 255)); break;
            case LTGRAY_BRUSH: object = (GdiObject*) make_brush(BS_SOLID, RGB(192, 192, 192)); break;
            case GRAY_BRUSH: object = (GdiObject*) make_brush(BS_SOLID, RGB(128, 128, 128)); break;
            case DKGRAY_BRUSH: object = (GdiObject*) make_brush(BS_SOLID, RGB(64, 64, 64)); break;
            case BLACK_BRUSH: object = (GdiObject*) make_brush(BS_SOLID, RGB(0, 0, 0)); break;
            case NULL_BRUSH: object = (GdiObject*) make_brush(BS_HOLLOW, 0); break;
            case WHITE_PEN: object = (GdiObject*) make_pen(PS_SOLID, 1, RGB(255, 255, 255)); break;
            case BLACK_PEN: object = (GdiObject*) make_pen(PS_SOLID, 1, RGB(0, 0, 0)); break;
            case NULL_PEN: object = (GdiObject*) make_pen(PS_NULL, 1, 0); break;
#ifdef _WIN32
            default: return GetStockObject(index); // fonts and palettes stay GDI's
#else
                        default: return 0;
#endif
        }
        if (object == 0)
            return 0;
        object->stock = 1;
        stock[index] = object;
    }
    return (HGDIOBJ) stock[index];
}

// The real GDI object standing for one of ours (Windows only): for the EDIT
// boxes, which are real controls, and for real device contexts.
HGDIOBJ gdi_win32_object(HGDIOBJ handle) {
#ifndef _WIN32
    return handle;
#else
    GdiObject* object = lookup(handle, 0);
    if (object == 0 || object->kind == GDI_DC)
        return handle;
    if (object->win32 == 0) {
        switch (object->kind) {
            case GDI_FONT:
                object->win32 = CreateFontIndirectA(&((GdiFont*) object)->logfont);
                break;
            case GDI_PEN: {
                GdiPen* pen = (GdiPen*) object;
                object->win32 = CreatePen(pen->style, pen->width, pen->color);
                break;
            }
            case GDI_BRUSH: {
                GdiBrush* brush = (GdiBrush*) object;
                object->win32 = brush->style == BS_HOLLOW ? GetStockObject(NULL_BRUSH) : CreateSolidBrush(brush->color);
                break;
            }
            case GDI_REGION:
                object->win32 = CreateRectRgnIndirect(&((GdiRegion*) object)->rect);
                break;
        }
    }
    return object->win32;
#endif
}

BOOL gdi_delete_object(HGDIOBJ handle) {
    GdiObject* object = lookup(handle, 0);
    if (object == 0)
#ifdef _WIN32
        return DeleteObject(handle);
#else
        return FALSE;
#endif
    if (object->stock != 0 || object->kind == GDI_DC)
        return TRUE;
    // A context still pointing at it falls back to its default.
    for (GdiObject* other = gdi_objects; other != 0; other = other->next) {
        if (other->kind != GDI_DC)
            continue;
        GdiDc* dc = (GdiDc*) other;
        if ((GdiObject*) dc->font == object)
            dc->font = 0;
        if ((GdiObject*) dc->pen == object)
            dc->pen = 0;
        if ((GdiObject*) dc->brush == object)
            dc->brush = 0;
    }
    untrack(object);
#ifdef _WIN32
    if (object->win32 != 0 && object->kind != GDI_BRUSH)
        DeleteObject(object->win32);
    else if (object->win32 != 0 && ((GdiBrush*) object)->style != BS_HOLLOW)
        DeleteObject(object->win32);
#endif
    if (object->kind == GDI_FONT && ((GdiFont*) object)->ttf != 0)
        TTF_CloseFont(((GdiFont*) object)->ttf);
    if (object->kind == GDI_PALETTE)
        free(((GdiPalette*) object)->entries);
    free(object);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Palettes. Logical palettes are only colour tables now: the game's frame
// gets its colours from DirectDraw's palette (ddraw_memory.cpp), so
// selecting and realizing a palette into a window's context, which mattered
// on an 8-bit desktop, does nothing.

static GdiPalette* our_palette(const void* handle) {
    return (GdiPalette*) lookup(handle, GDI_PALETTE);
}

static GdiPalette* make_palette(int count) {
    GdiPalette* palette = (GdiPalette*) calloc(1, sizeof(GdiPalette));
    if (palette == 0)
        return 0;
    palette->entries = (PALETTEENTRY*) calloc(count > 0 ? count : 1, sizeof(PALETTEENTRY));
    if (palette->entries == 0) {
        free(palette);
        return 0;
    }
    palette->count = count;
    track(&palette->head, GDI_PALETTE);
    return palette;
}

HPALETTE gdi_create_palette(const LOGPALETTE* logpalette) {
    if (logpalette == 0 || logpalette->palNumEntries == 0)
        return 0;
    GdiPalette* palette = make_palette(logpalette->palNumEntries);
    if (palette == 0)
        return 0;
    memcpy(palette->entries, logpalette->palPalEntry, palette->count * sizeof(PALETTEENTRY));
    return (HPALETTE) palette;
}

// GDI's halftone palette is a colour cube for drawing true-colour images on
// a 256-colour display; this one is a 6x6x6 cube and a grey ramp.
HPALETTE gdi_create_halftone_palette(HDC) {
    GdiPalette* palette = make_palette(256);
    if (palette == 0)
        return 0;
    int i = 0;
    for (int r = 0; r < 6; ++r)
        for (int g = 0; g < 6; ++g)
            for (int b = 0; b < 6; ++b, ++i) {
                palette->entries[i].peRed = (BYTE) (r * 51);
                palette->entries[i].peGreen = (BYTE) (g * 51);
                palette->entries[i].peBlue = (BYTE) (b * 51);
            }
    for (int grey = 0; i < 256; ++i, ++grey) {
        BYTE level = (BYTE) (grey * 255 / 39);
        palette->entries[i].peRed = level;
        palette->entries[i].peGreen = level;
        palette->entries[i].peBlue = level;
    }
    return (HPALETTE) palette;
}

UINT gdi_get_palette_entries(void* handle, UINT start, UINT count, PALETTEENTRY* entries) {
    GdiPalette* palette = our_palette(handle);
    if (palette == 0)
        return 0;
    if (entries == 0)
        return (UINT) palette->count;
    if (start >= (UINT) palette->count)
        return 0;
    if (count > palette->count - start)
        count = palette->count - start;
    memcpy(entries, palette->entries + start, count * sizeof(PALETTEENTRY));
    return count;
}

UINT gdi_set_palette_entries(void* handle, UINT start, UINT count, const PALETTEENTRY* entries) {
    GdiPalette* palette = our_palette(handle);
    if (palette == 0 || entries == 0 || start >= (UINT) palette->count)
        return 0;
    if (count > palette->count - start)
        count = palette->count - start;
    memcpy(palette->entries + start, entries, count * sizeof(PALETTEENTRY));
    return count;
}

BOOL gdi_resize_palette(void* handle, UINT count) {
    GdiPalette* palette = our_palette(handle);
    if (palette == 0 || count == 0)
        return FALSE;
    PALETTEENTRY* entries = (PALETTEENTRY*) realloc(palette->entries, count * sizeof(PALETTEENTRY));
    if (entries == 0)
        return FALSE;
    if (count > (UINT) palette->count)
        memset(entries + palette->count, 0, (count - palette->count) * sizeof(PALETTEENTRY));
    palette->entries = entries;
    palette->count = (int) count;
    return TRUE;
}

// The entry nearest in RGB space; the lowest index on a tie.
UINT gdi_get_nearest_palette_index(void* handle, COLORREF color) {
    GdiPalette* palette = our_palette(handle);
    if (palette == 0)
        return CLR_INVALID;
    int r = GetRValue(color);
    int g = GetGValue(color);
    int b = GetBValue(color);
    long best = 0x7FFFFFFF;
    UINT best_index = 0;
    for (int i = 0; i < palette->count; ++i) {
        long dr = palette->entries[i].peRed - r;
        long dg = palette->entries[i].peGreen - g;
        long db = palette->entries[i].peBlue - b;
        long distance = dr * dr + dg * dg + db * db;
        if (distance < best) {
            best = distance;
            best_index = (UINT) i;
            if (distance == 0)
                break;
        }
    }
    return best_index;
}

// GetObjectA for a palette: its entry count, as a WORD.
int gdi_get_object(void* handle, int size, void* out) {
    GdiPalette* palette = our_palette(handle);
    if (palette == 0 || out == 0 || size < (int) sizeof(WORD))
        return 0;
    *(WORD*) out = (WORD) palette->count;
    return sizeof(WORD);
}

HPALETTE gdi_select_palette(HDC, void*, BOOL) {
    return 0;
}

UINT gdi_realize_palette(HDC) {
    return 0;
}

// A true-colour display has no system palette: GetSystemPaletteEntries
// fails there, and the game then uses its own table of the static colours.
UINT gdi_get_system_palette_entries(HDC, UINT, UINT, PALETTEENTRY*) {
    return 0;
}

// ---------------------------------------------------------------------------
// Surface device contexts (for port/sdl/ddraw_memory.cpp)

HDC gdi_create_surface_dc(void) {
    GdiDc* dc = (GdiDc*) calloc(1, sizeof(GdiDc));
    if (dc == 0)
        return 0;
    dc->text_color = RGB(0, 0, 0);
    dc->bk_color = RGB(255, 255, 255);
    dc->bk_mode = OPAQUE;
    track(&dc->head, GDI_DC);
    return (HDC) dc;
}

void gdi_set_surface_dc_target(HDC handle, unsigned char* bits, long pitch, long width, long height,
                               const PALETTEENTRY* palette) {
    GdiDc* dc = our_dc(handle);
    if (dc == 0)
        return;
    dc->bits = bits;
    dc->pitch = pitch;
    dc->width = width;
    dc->height = height;
    dc->palette = palette;
}

void gdi_delete_surface_dc(HDC handle) {
    GdiDc* dc = our_dc(handle);
    if (dc == 0)
        return;
    untrack(&dc->head);
    free(dc);
}

// ---------------------------------------------------------------------------
// Drawing helpers

// GDI's colour match on a palettized surface: the exact entry if there is
// one, else the nearest in RGB space, lowest index on a tie.
static unsigned char palette_index(GdiDc* dc, COLORREF color) {
    if ((color >> 24) == 1)  // PALETTEINDEX(n)
        return (unsigned char) (color & 0xFF);
    int r = GetRValue(color);
    int g = GetGValue(color);
    int b = GetBValue(color);
    if (dc->palette == 0)
        return r + g + b >= 384 ? 255 : 0;
    long best = 0x7FFFFFFF;
    int best_index = 0;
    for (int i = 0; i < 256; ++i) {
        long dr = dc->palette[i].peRed - r;
        long dg = dc->palette[i].peGreen - g;
        long db = dc->palette[i].peBlue - b;
        long distance = dr * dr + dg * dg + db * db;
        if (distance < best) {
            best = distance;
            best_index = i;
            if (distance == 0)
                break;
        }
    }
    return (unsigned char) best_index;
}

// The rectangle a context may draw in: its surface, cut to the clip region.
static RECT dc_bounds(GdiDc* dc) {
    RECT bounds;
    bounds.left = 0;
    bounds.top = 0;
    bounds.right = dc->width;
    bounds.bottom = dc->height;
    if (dc->has_clip != 0) {
        if (dc->clip.left > bounds.left)
            bounds.left = dc->clip.left;
        if (dc->clip.top > bounds.top)
            bounds.top = dc->clip.top;
        if (dc->clip.right < bounds.right)
            bounds.right = dc->clip.right;
        if (dc->clip.bottom < bounds.bottom)
            bounds.bottom = dc->clip.bottom;
    }
    return bounds;
}

static void intersect(RECT* a, const RECT* b) {
    if (b->left > a->left)
        a->left = b->left;
    if (b->top > a->top)
        a->top = b->top;
    if (b->right < a->right)
        a->right = b->right;
    if (b->bottom < a->bottom)
        a->bottom = b->bottom;
}

static void fill(GdiDc* dc, RECT area, const RECT* limit, unsigned char index) {
    intersect(&area, limit);
    for (long y = area.top; y < area.bottom; ++y)
        if (area.right > area.left)
            memset(dc->bits + y * dc->pitch + area.left, index, area.right - area.left);
}

// A counted string as a C string, in a buffer the caller frees.
static char* copy_text(const char* text, int length) {
    if (text == 0)
        length = 0;
    else if (length < 0)
        length = (int) strlen(text);
    char* copy = (char*) malloc(length + 1);
    if (copy == 0)
        return 0;
    if (length > 0)
        memcpy(copy, text, length);
    copy[length] = 0;
    return copy;
}

static int text_width(GdiFont* font, const char* text) {
    int width = 0;
    int height = 0;
    if (font == 0 || text[0] == 0)
        return 0;
    TTF_SizeText(font->ttf, text, &width, &height);
    return width;
}

static int font_height(GdiFont* font) {
    return font != 0 ? TTF_FontHeight(font->ttf) : 0;
}

// Draws one line with its cell's top-left at (x, y), inside limit.
static void draw_line_of_text(GdiDc* dc, int x, int y, const char* text, const RECT* limit) {
    GdiFont* font = dc_font(dc);
    if (font == 0 || dc->bits == 0)
        return;
    int width = text_width(font, text);
    int height = font_height(font);
    if (dc->bk_mode == OPAQUE) {
        RECT cell;
        cell.left = x;
        cell.top = y;
        cell.right = x + width;
        cell.bottom = y + height;
        fill(dc, cell, limit, palette_index(dc, dc->bk_color));
    }
    if (text[0] == 0)
        return;
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface* glyphs = TTF_RenderText_Solid(font->ttf, text, white);
    if (glyphs == 0)
        return;
    unsigned char ink = palette_index(dc, dc->text_color);
    if (SDL_LockSurface(glyphs) == 0) {
        for (int row = 0; row < glyphs->h; ++row) {
            int out_y = y + row;
            if (out_y < limit->top || out_y >= limit->bottom)
                continue;
            const unsigned char* in = (const unsigned char*) glyphs->pixels + row * glyphs->pitch;
            unsigned char* out = dc->bits + out_y * dc->pitch;
            for (int col = 0; col < glyphs->w; ++col) {
                int out_x = x + col;
                if (in[col] != 0 && out_x >= limit->left && out_x < limit->right)
                    out[out_x] = ink;
            }
        }
        SDL_UnlockSurface(glyphs);
    }
    SDL_FreeSurface(glyphs);
}

// ---------------------------------------------------------------------------
// The GDI calls

HGDIOBJ gdi_select_object(HDC handle, HGDIOBJ object) {
    GdiDc* dc = our_dc(handle);
    if (dc == 0)
#ifdef _WIN32
        return SelectObject(handle, gdi_win32_object(object));
#else
        return 0;
#endif
    GdiObject* ours = lookup(object, 0);
    if (ours == 0)
        return 0;
    HGDIOBJ previous = 0;
    switch (ours->kind) {
        case GDI_FONT:
            previous = (HGDIOBJ) dc->font;
            dc->font = (GdiFont*) ours;
            break;
        case GDI_PEN:
            previous = (HGDIOBJ) dc->pen;
            dc->pen = (GdiPen*) ours;
            break;
        case GDI_BRUSH:
            previous = (HGDIOBJ) dc->brush;
            dc->brush = (GdiBrush*) ours;
            break;
        case GDI_REGION:
            dc->clip = ((GdiRegion*) ours)->rect;
            dc->has_clip = 1;
            return (HGDIOBJ) SIMPLEREGION;
    }
    return previous;
}

int gdi_select_clip_rgn(HDC handle, HRGN region) {
    GdiDc* dc = our_dc(handle);
    if (dc == 0)
#ifdef _WIN32
        return SelectClipRgn(handle, (HRGN) gdi_win32_object(region));
#else
        return ERROR;
#endif
    GdiRegion* ours = (GdiRegion*) lookup(region, GDI_REGION);
    if (ours == 0) {
        dc->has_clip = 0;
        return NULLREGION;
    }
    dc->clip = ours->rect;
    dc->has_clip = 1;
    return SIMPLEREGION;
}

COLORREF gdi_set_text_color(HDC handle, COLORREF color) {
    GdiDc* dc = our_dc(handle);
    if (dc == 0)
#ifdef _WIN32
        return SetTextColor(handle, color);
#else
        return CLR_INVALID;
#endif
    COLORREF previous = dc->text_color;
    dc->text_color = color;
    return previous;
}

COLORREF gdi_set_bk_color(HDC handle, COLORREF color) {
    GdiDc* dc = our_dc(handle);
    if (dc == 0)
#ifdef _WIN32
        return SetBkColor(handle, color);
#else
        return CLR_INVALID;
#endif
    COLORREF previous = dc->bk_color;
    dc->bk_color = color;
    return previous;
}

int gdi_set_bk_mode(HDC handle, int mode) {
    GdiDc* dc = our_dc(handle);
    if (dc == 0)
#ifdef _WIN32
        return SetBkMode(handle, mode);
#else
        return 0;
#endif
    int previous = dc->bk_mode;
    dc->bk_mode = mode;
    return previous;
}

BOOL gdi_text_out(HDC handle, int x, int y, LPCSTR text, int length) {
    GdiDc* dc = our_dc(handle);
    if (dc == 0)
#ifdef _WIN32
        return TextOutA(handle, x, y, text, length);
#else
        return FALSE;
#endif
    char* line = copy_text(text, length);
    if (line == 0)
        return FALSE;
    RECT limit = dc_bounds(dc);
    draw_line_of_text(dc, x, y, line, &limit);
    free(line);
    return TRUE;
}

BOOL gdi_get_text_extent_point32(HDC handle, LPCSTR text, int length, SIZE* size) {
    GdiDc* dc = our_dc(handle);
    if (dc == 0)
#ifdef _WIN32
        return GetTextExtentPoint32A(handle, text, length, size);
#else
        return FALSE;
#endif
    char* line = copy_text(text, length);
    if (line == 0)
        return FALSE;
    GdiFont* font = dc_font(dc);
    size->cx = text_width(font, line);
    size->cy = font_height(font);
    free(line);
    return TRUE;
}

BOOL gdi_get_text_metrics(HDC handle, TEXTMETRICA* metrics) {
    GdiDc* dc = our_dc(handle);
    if (dc == 0)
#ifdef _WIN32
        return GetTextMetricsA(handle, metrics);
#else
        return FALSE;
#endif
    GdiFont* font = dc_font(dc);
    memset(metrics, 0, sizeof(*metrics));
    if (font == 0)
        return FALSE;
    int min_x;
    int max_x;
    int min_y;
    int max_y;
    int advance = 0;
    metrics->tmAscent = TTF_FontAscent(font->ttf);
    metrics->tmDescent = -TTF_FontDescent(font->ttf);
    metrics->tmHeight = TTF_FontHeight(font->ttf);
    int skip = TTF_FontLineSkip(font->ttf);
    metrics->tmExternalLeading = skip > metrics->tmHeight ? skip - metrics->tmHeight : 0;
    if (TTF_GlyphMetrics(font->ttf, 'x', &min_x, &max_x, &min_y, &max_y, &advance) == 0)
        metrics->tmAveCharWidth = advance;
    if (TTF_GlyphMetrics(font->ttf, 'W', &min_x, &max_x, &min_y, &max_y, &advance) == 0)
        metrics->tmMaxCharWidth = advance;
    metrics->tmWeight = font->logfont.lfWeight;
    metrics->tmItalic = font->logfont.lfItalic;
    metrics->tmUnderlined = font->logfont.lfUnderline;
    metrics->tmStruckOut = font->logfont.lfStrikeOut;
    metrics->tmFirstChar = 0x20;
    metrics->tmLastChar = 0xFF;
    metrics->tmDefaultChar = '?';
    metrics->tmBreakChar = ' ';
    metrics->tmPitchAndFamily = TMPF_VECTOR | TMPF_TRUETYPE | (font->logfont.lfPitchAndFamily & 0xF0);
    metrics->tmCharSet = font->logfont.lfCharSet;
    return TRUE;
}

// Splits text into the lines DrawText would show: at line breaks unless
// DT_SINGLELINE, and at spaces where a line is wider than max_width with
// DT_WORDBREAK. Calls out(line, ctx) for each.
static void each_line(GdiFont* font, const char* text, UINT format, int max_width, void (*out)(const char*, void*),
                      void* context) {
    size_t length = strlen(text);
    char* line = (char*) malloc(length + 1);
    if (line == 0)
        return;
    const char* p = text;
    for (;;) {
        // One paragraph: up to a line break, or the whole text.
        const char* end = p;
        if ((format & DT_SINGLELINE) != 0)
            end = p + strlen(p);
        else
            while (*end != 0 && *end != '\n' && *end != '\r')
                ++end;
        size_t paragraph = end - p;
        memcpy(line, p, paragraph);
        line[paragraph] = 0;
        if ((format & DT_SINGLELINE) != 0)
            for (size_t i = 0; i < paragraph; ++i)
                if (line[i] == '\r' || line[i] == '\n')
                    line[i] = ' ';

        if ((format & DT_WORDBREAK) == 0 || text_width(font, line) <= max_width) {
            out(line, context);
        } else {
            // Greedy word wrap; a word wider than the line stays whole.
            char* start = line;
            while (*start != 0) {
                char* cut = 0;
                for (char* q = start; *q != 0; ++q) {
                    if (*q != ' ')
                        continue;
                    char saved = *q;
                    *q = 0;
                    int fits = text_width(font, start) <= max_width;
                    *q = saved;
                    if (fits == 0)
                        break;
                    cut = q;
                }
                if (cut == 0 || text_width(font, start) <= max_width) {
                    out(start, context);
                    break;
                }
                *cut = 0;
                out(start, context);
                start = cut + 1;
                while (*start == ' ')
                    ++start;
            }
        }

        if (*end == 0)
            break;
        p = end;
        if (*p == '\r' && p[1] == '\n')
            p += 2;
        else
            ++p;
    }
    free(line);
}

struct TextLayout {
    GdiDc* dc;
    GdiFont* font;
    RECT rect;
    RECT limit;
    UINT format;
    int y;
    int line_height;
};

static void draw_text_line(const char* line, void* context) {
    TextLayout* state = (TextLayout*) context;
    int width = text_width(state->font, line);
    int x = state->rect.left;
    if ((state->format & DT_CENTER) != 0)
        x = state->rect.left + (state->rect.right - state->rect.left - width) / 2;
    else if ((state->format & DT_RIGHT) != 0)
        x = state->rect.right - width;
    draw_line_of_text(state->dc, x, state->y, line, &state->limit);
    state->y += state->line_height;
}

static void count_line(const char* line, void* context) {
    ++*(int*) context;
}

int gdi_draw_text(HDC handle, LPCSTR text, int length, RECT* rect, UINT format) {
    GdiDc* dc = our_dc(handle);
    if (dc == 0)
#ifdef _WIN32
        return DrawTextA(handle, text, length, rect, format);
#else
        return 0;
#endif
    GdiFont* font = dc_font(dc);
    char* copy = copy_text(text, length);
    if (copy == 0 || font == 0) {
        free(copy);
        return 0;
    }
    TextLayout state;
    state.dc = dc;
    state.font = font;
    state.rect = *rect;
    state.format = format;
    state.line_height = font_height(font);
    state.limit = dc_bounds(dc);
    if ((format & DT_NOCLIP) == 0)
        intersect(&state.limit, rect);

    int lines = 0;
    each_line(font, copy, format, rect->right - rect->left, count_line, &lines);
    int total = lines * state.line_height;
    state.y = rect->top;
    if ((format & DT_SINGLELINE) != 0) {
        if ((format & DT_VCENTER) != 0)
            state.y = rect->top + (rect->bottom - rect->top - state.line_height) / 2;
        else if ((format & DT_BOTTOM) != 0)
            state.y = rect->bottom - state.line_height;
    }
    if ((format & DT_CALCRECT) != 0) {
        int widest = 0;
        // DT_CALCRECT only measures: the rectangle's bottom becomes the
        // text's, and for a single line its right edge the text's.
        if ((format & DT_SINGLELINE) != 0)
            widest = text_width(font, copy);
        rect->bottom = rect->top + total;
        if ((format & DT_SINGLELINE) != 0)
            rect->right = rect->left + widest;
    } else {
        each_line(font, copy, format, rect->right - rect->left, draw_text_line, &state);
    }
    free(copy);
    return total;
}

int gdi_draw_text_ex(HDC handle, LPSTR text, int length, RECT* rect, UINT format, DRAWTEXTPARAMS* params) {
    if (our_dc(handle) == 0)
#ifdef _WIN32
        return DrawTextExA(handle, text, length, rect, format, params);
#else
        return 0;
#endif
    return gdi_draw_text(handle, text, length, rect, format);
}

BOOL gdi_move_to_ex(HDC handle, int x, int y, POINT* previous) {
    GdiDc* dc = our_dc(handle);
    if (dc == 0)
#ifdef _WIN32
        return MoveToEx(handle, x, y, previous);
#else
        return FALSE;
#endif
    if (previous != 0)
        *previous = dc->position;
    dc->position.x = x;
    dc->position.y = y;
    return TRUE;
}

// A one-pixel line from the current position up to, not including, (x, y).
BOOL gdi_line_to(HDC handle, int x, int y) {
    GdiDc* dc = our_dc(handle);
    if (dc == 0)
#ifdef _WIN32
        return LineTo(handle, x, y);
#else
        return FALSE;
#endif
    GdiPen* pen = dc->pen;
    int x0 = dc->position.x;
    int y0 = dc->position.y;
    dc->position.x = x;
    dc->position.y = y;
    if (dc->bits == 0 || (pen != 0 && pen->style == PS_NULL))
        return TRUE;
    unsigned char ink = palette_index(dc, pen != 0 ? pen->color : RGB(0, 0, 0));
    RECT limit = dc_bounds(dc);
    int dx = abs(x - x0);
    int dy = -abs(y - y0);
    int sx = x0 < x ? 1 : -1;
    int sy = y0 < y ? 1 : -1;
    int error = dx + dy;
    while (x0 != x || y0 != y) {
        if (x0 >= limit.left && x0 < limit.right && y0 >= limit.top && y0 < limit.bottom)
            dc->bits[y0 * dc->pitch + x0] = ink;
        int twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
    return TRUE;
}

int gdi_fill_rect(HDC handle, const RECT* rect, HBRUSH brush) {
    GdiDc* dc = our_dc(handle);
    if (dc == 0)
#ifdef _WIN32
        return FillRect(handle, rect, (HBRUSH) gdi_win32_object(brush));
#else
        return 0;
#endif
    GdiBrush* ours = (GdiBrush*) lookup(brush, GDI_BRUSH);
    if (ours == 0 || ours->style == BS_HOLLOW || dc->bits == 0)
        return TRUE;
    RECT limit = dc_bounds(dc);
    fill(dc, *rect, &limit, palette_index(dc, ours->color));
    return TRUE;
}
