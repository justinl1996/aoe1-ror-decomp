// Cases for the drawing engine in src/Asm_draw.asm.
//
// Each case builds a surface (row pointers into one random-filled buffer),
// per-line span lists (with Prev links and tail pointers for mirrored
// drawing), and, for sprites, a frame in the engine's command format. The
// body sets the engine state through the public setters, then draws one or
// more times. Pixels outside the spans and frames are compared too, so a
// stray write is a failure.
#include "harness.h"
#include "reference.h"

#include "asm_draw.h"
#include "drawarea.h"

#include <string.h>

extern "C" void __cdecl ASMSet_ShadowingEx(void* masks_and_values);
extern "C" void* __cdecl ASMGet_Xlate_Table(void);

// ---------------------------------------------------------------- engine calls

static void set_shadowing(int port, u32 m1, u32 v1, u32 m2, u32 v2)
{
    if (port)
        ASMSet_Shadowing(m1, v1, m2, v2);
    else
        REF_ASMSet_Shadowing(m1, v1, m2, v2);
}

static void set_shadowing_ex(int port, void* table)
{
    if (port)
        ASMSet_ShadowingEx(table);
    else
        REF_ASMSet_ShadowingEx(table);
}

static void set_surface(int port, void* offsets, void* heads, void* tails, long min_span, long min_line, long max_span,
                        long max_line)
{
    if (port)
        ASMSet_Surface_Info(offsets, heads, tails, min_span, min_line, max_span, max_line);
    else
        REF_ASMSet_Surface_Info(offsets, heads, tails, min_span, min_line, max_span, max_line);
}

static void set_color(int port, int color)
{
    if (port)
        ASMSet_Color_Xform(color);
    else
        REF_ASMSet_Color_Xform(color);
}

static int get_color(int port)
{
    return port ? ASMGet_Color_Xform() : REF_ASMGet_Color_Xform();
}

static void set_xlate(int port, void* table)
{
    if (port)
        ASMSet_Xlate_Table(table);
    else
        REF_ASMSet_Xlate_Table(table);
}

static void* get_xlate(int port)
{
    return port ? ASMGet_Xlate_Table() : REF_ASMGet_Xlate_Table();
}

static void draw_hline(int port, long x1, long x2, long y, u32 color)
{
    if (port)
        ASMDraw_HLine(x1, x2, y, color);
    else
        REF_ASMDraw_HLine(x1, x2, y, color);
}

static void draw_sprite(int port, void* base, long x, long y, long width, long height, long data_offsets,
                        long outline_offset, int flags)
{
    if (port)
        ASMDraw_Sprite(base, x, y, width, height, data_offsets, outline_offset, flags);
    else
        REF_ASMDraw_Sprite(base, x, y, width, height, data_offsets, outline_offset, flags);
}

// ---------------------------------------------------------------- surface

struct Surface {
    int width;  // pixels per line that spans may cover
    int height; // lines
    u8** rows;
    VSpan_Node** heads;
    VSpan_Node** tails;
    long min_span, min_line, max_span, max_line;
};

// Spans per line: sorted, disjoint, inside [0, width-1]. `style` picks the
// shape: 0 random, 1 one span over the whole line (so sprites take the
// unclipped path), 2 many narrow spans.
static void make_surface(Rng* rng, Surface* s, int width, int height, int style)
{
    int pad = 48, pitch = width + 2 * pad + rng_range(rng, 0, 7), align = rng_range(rng, 0, 3), y;
    u8* pixels = (u8*)arena_alloc(pitch * height + 16, 4, "surface");
    rng_fill(rng, pixels, pitch * height + 16);
    s->width = width;
    s->height = height;
    s->rows = (u8**)arena_alloc(sizeof(u8*) * height, 4, "row_offsets");
    s->heads = (VSpan_Node**)arena_alloc(sizeof(VSpan_Node*) * height, 4, "span_heads");
    s->tails = (VSpan_Node**)arena_alloc(sizeof(VSpan_Node*) * height, 4, "span_tails");
    VSpan_Node* pool = (VSpan_Node*)arena_alloc(sizeof(VSpan_Node) * height * (width / 2 + 2), 4, "span_nodes");
    for (y = 0; y < height; ++y) {
        int x = 0, count = 0;
        VSpan_Node* previous = 0;
        s->rows[y] = pixels + pad + align + y * pitch;
        s->heads[y] = s->tails[y] = 0;
        if (style == 0 && rng_chance(rng, 8)) continue; // no spans on this line
        for (;;) {
            int gap, length;
            VSpan_Node* node;
            if (style == 1) {
                if (count) break;
                gap = 0;
                length = width;
            } else if (style == 2) {
                gap = rng_range(rng, count ? 1 : 0, 3);
                length = rng_range(rng, 1, 4);
            } else {
                gap = rng_range(rng, count ? 1 : 0, width / 3);
                length = rng_range(rng, 1, width / 2 + 1);
            }
            if (x + gap >= width) break;
            x += gap;
            if (x + length > width) length = width - x;
            node = pool++;
            node->StartPx = x;
            node->EndPx = x + length - 1;
            node->Next = 0;
            node->Prev = previous;
            if (previous)
                previous->Next = node;
            else
                s->heads[y] = node;
            s->tails[y] = node;
            previous = node;
            x += length;
            ++count;
            if (style == 0 && count >= 4) break;
        }
    }
    // The engine trusts these limits; keep them inside the surface. Line
    // clipping with min_line > 0 has an assembly quirk (sprites rows restart
    // at line 0), so cover both.
    s->min_line = rng_chance(rng, 70) ? 0 : rng_range(rng, 0, height / 3);
    s->max_line = rng_chance(rng, 70) ? height - 1 : rng_range(rng, height / 2, height - 1);
    s->min_span = rng_chance(rng, 70) ? 0 : rng_range(rng, 0, width / 3);
    s->max_span = rng_chance(rng, 70) ? width - 1 : rng_range(rng, width / 2, width - 1);
}

static void apply_surface(int port, const Surface* s)
{
    set_surface(port, s->rows, s->heads, s->tails, s->min_span, s->min_line, s->max_span, s->max_line);
}

// ---------------------------------------------------------------- setters

struct ColorCase {
    int colors[8];
    void* tables[8];
};

static void color_body(void* context, int port)
{
    ColorCase* c = (ColorCase*)context;
    int i;
    for (i = 0; i < 8; ++i) {
        set_color(port, c->colors[i]);
        record("color", get_color(port));
    }
}

static void xlate_body(void* context, int port)
{
    ColorCase* c = (ColorCase*)context;
    int i;
    for (i = 0; i < 8; ++i) {
        set_xlate(port, c->tables[i]);
        record("xlate", (long)get_xlate(port));
    }
}

static void setter_cases()
{
    int count = cases_for(500), i, k;
    for (i = 0; i < count; ++i) {
        u32 seed = suite_options()->seed ^ 0x5500000UL ^ (u32)i * 2654435761UL;
        Rng rng;
        ColorCase c;
        rng_seed(&rng, seed);
        arena_reset();
        for (k = 0; k < 8; ++k) {
            c.colors[k] = (int)rng_next(&rng);
            c.tables[k] = (void*)rng_next(&rng);
        }
        if (wants_function("ASMSet_Color_Xform")) run_case("ASMSet_Color_Xform", i, seed, color_body, &c);
        if (wants_function("ASMGet_Color_Xform")) run_case("ASMGet_Color_Xform", i, seed, color_body, &c);
        if (wants_function("ASMSet_Xlate_Table")) run_case("ASMSet_Xlate_Table", i, seed, xlate_body, &c);
        if (wants_function("ASMGet_Xlate_Table")) run_case("ASMGet_Xlate_Table", i, seed, xlate_body, &c);
    }
}

// ---------------------------------------------------------------- HLine

struct HLineCase {
    Surface surface;
    int lines;
    long x1[16], x2[16], y[16];
    u32 color[16];
};

static void hline_body(void* context, int port)
{
    HLineCase* c = (HLineCase*)context;
    int i;
    apply_surface(port, &c->surface);
    for (i = 0; i < c->lines; ++i) draw_hline(port, c->x1[i], c->x2[i], c->y[i], c->color[i]);
}

static void make_hlines(Rng* rng, HLineCase* c)
{
    int i, w = c->surface.width, h = c->surface.height;
    c->lines = rng_range(rng, 1, 16);
    for (i = 0; i < c->lines; ++i) {
        c->x1[i] = rng_range(rng, -20, w + 20);
        c->x2[i] = rng_chance(rng, 20) ? c->x1[i] : rng_range(rng, -20, w + 20);
        c->y[i] = rng_range(rng, -3, h + 2);
        c->color[i] = rng_next(rng);
    }
}

static void hline_cases(const char* function, int salt)
{
    int count = cases_for(3000), i;
    if (!wants_function(function)) return;
    for (i = 0; i < count; ++i) {
        u32 seed = suite_options()->seed ^ (u32)salt ^ (u32)i * 2654435761UL;
        Rng rng;
        HLineCase c;
        rng_seed(&rng, seed);
        arena_reset();
        make_surface(&rng, &c.surface, rng_range(&rng, 1, 300), rng_range(&rng, 1, 24), rng_range(&rng, 0, 2));
        make_hlines(&rng, &c);
        run_case(function, i, seed, hline_body, &c);
    }
}

// ---------------------------------------------------------------- sprites

// Frame layout, relative to `base`:
//   outline_offset: per row, u16 left margin and u16 right margin; 0x8000 in
//                   the left margin (normal) or right margin (mirrored) skips
//                   the row;
//   data_offsets:   per row, u32 offset from base to the row's commands.
// Commands (low nibble selects): xxxxxx00 copy (len = byte>>2), xxxxxx01 skip,
// L2 nn big copy, L3 nn big skip (len = L<<8 | nn), L6 player copy and L7
// fill and LA player fill and LB shadow (len = L, or the next byte when L is
// 0), 1E/2E nn outline (skipped: the engine never enables outlines), 3E
// no-op and 4E <command> skip the next command's pixels (swapped when
// mirrored), 0F end of row.
struct SpriteGen {
    u8* data;
    u32 used;
    u32 capacity;
    int allow_extended; // 3E (and 4E unless mirrored): only on unclipped rows
    int mirrored;
    int zero_lengths;
};

static void emit(SpriteGen* g, u32 byte)
{
    if (g->used < g->capacity) g->data[g->used] = (u8)byte;
    ++g->used;
}

static void emit_pixels(SpriteGen* g, Rng* rng, int count)
{
    int i;
    for (i = 0; i < count; ++i) emit(g, rng_next(rng) >> 9);
}

// A count encoded in the high nibble, or 0 and a following byte.
static int emit_nibble_count(SpriteGen* g, Rng* rng, u32 low, int left)
{
    int length;
    if (left < 16 && rng_chance(rng, 60)) {
        length = rng_range(rng, 1, left < 15 ? left : 15);
        emit(g, (u32)(length << 4) | low);
    } else {
        length = rng_range(rng, 1, left < 255 ? left : 255);
        if (rng_chance(rng, 50) && length < 16) {
            emit(g, (u32)(length << 4) | low);
        } else {
            emit(g, low);
            emit(g, (u32)length);
        }
    }
    return length;
}

// One command covering 1..left pixels; returns the pixels covered.
static int emit_command(SpriteGen* g, Rng* rng, int left, int nested)
{
    int kind = rng_range(rng, 0, nested ? 7 : 11), length;
    switch (kind) {
    case 0: // copy
    case 1:
        length = rng_range(rng, 1, left < 63 ? left : 63);
        emit(g, (u32)(length << 2));
        emit_pixels(g, rng, length);
        return length;
    case 2: // skip
        length = rng_range(rng, 1, left < 63 ? left : 63);
        emit(g, (u32)(length << 2) | 1);
        return length;
    case 3: // big copy
        length = rng_range(rng, 1, left < 600 ? left : 600);
        emit(g, (u32)((length >> 8) << 4) | 2);
        emit(g, (u32)length & 0xFF);
        emit_pixels(g, rng, length);
        return length;
    case 4: // big skip
        length = rng_range(rng, 1, left < 600 ? left : 600);
        emit(g, (u32)((length >> 8) << 4) | 3);
        emit(g, (u32)length & 0xFF);
        return length;
    case 5: // player copy
        length = emit_nibble_count(g, rng, 6, left);
        emit_pixels(g, rng, length);
        return length;
    case 6: // fill
        length = emit_nibble_count(g, rng, 7, left);
        emit(g, rng_next(rng) >> 7);
        return length;
    case 7: // player fill
        length = emit_nibble_count(g, rng, 0xA, left);
        emit(g, rng_next(rng) >> 7);
        return length;
    case 8: // shadow
    case 9:
        return emit_nibble_count(g, rng, 0xB, left);
    case 10: // outline: one pixel, or a counted span
        if (rng_chance(rng, 50)) {
            emit(g, 0x1E);
            return 1;
        }
        length = rng_range(rng, 1, left < 255 ? left : 255);
        emit(g, 0x2E);
        emit(g, (u32)length);
        return length;
    default:
        if (!g->allow_extended) return emit_command(g, rng, left, 0);
        // Unclipped rows: normally 3E is a no-op and 4E draws the next
        // command as transparent. Mirrored, the two swap, and the skip moves
        // right instead of left (an assembly bug the port keeps); the shifted
        // pixels could leave the surface, so mirrored frames only get the
        // no-op.
        if (g->mirrored) {
            emit(g, 0x4E);
            return 0;
        }
        if (rng_chance(rng, 50)) {
            emit(g, 0x3E);
            return 0;
        }
        emit(g, 0x4E);
        return emit_command(g, rng, left, 1);
    }
}

struct SpriteDraw {
    long x, y, width, height;
    int flags;
    u8* base;
    long data_offsets, outline_offset;
    int color;
};

struct SpriteCase {
    Surface surface;
    u8* xlate;
    u32 shadow_table[8];
    u32 shadow[4];
    int use_shadow_ex;
    int draws;
    SpriteDraw draw[4];
};

static void make_frame(Rng* rng, SpriteDraw* d, int width, int height, int allow_extended, int mirrored)
{
    u32 capacity = 64 + (u32)height * 8 + (u32)height * (u32)(width * 3 + 64);
    u8* base = (u8*)arena_alloc(capacity, 4, "frame");
    SpriteGen g;
    int row;
    memset(base, 0xEE, capacity);
    d->base = base;
    d->width = width;
    d->height = height;
    d->outline_offset = 16;
    d->data_offsets = 16 + height * 4;
    g.data = base;
    g.capacity = capacity;
    g.used = (u32)(d->data_offsets + height * 4);
    g.allow_extended = allow_extended;
    g.mirrored = mirrored;
    g.zero_lengths = 0;
    for (row = 0; row < height; ++row) {
        u16* outline = (u16*)(base + d->outline_offset + row * 4);
        u32* offset = (u32*)(base + d->data_offsets + row * 4);
        int left, right, pixels;
        if (rng_chance(rng, 6)) {
            outline[0] = outline[1] = 0x8000;
            *offset = 0;
            continue;
        }
        left = rng_chance(rng, 30) ? 0 : rng_range(rng, 0, width / 2);
        right = rng_chance(rng, 30) ? 0 : rng_range(rng, 0, width - left - 1);
        outline[0] = (u16)left;
        outline[1] = (u16)right;
        *offset = g.used;
        pixels = width - left - right;
        while (pixels > 0) pixels -= emit_command(&g, rng, pixels, 0);
        emit(&g, 0x0F);
    }
    if (g.used > capacity) {
        // Never expected: the capacity bound covers the worst case.
        d->height = 0;
    }
}

static void sprite_body(void* context, int port)
{
    SpriteCase* c = (SpriteCase*)context;
    int i;
    apply_surface(port, &c->surface);
    set_xlate(port, c->xlate);
    if (c->use_shadow_ex)
        set_shadowing_ex(port, c->shadow_table);
    else
        set_shadowing(port, c->shadow[0], c->shadow[1], c->shadow[2], c->shadow[3]);
    for (i = 0; i < c->draws; ++i) {
        SpriteDraw* d = &c->draw[i];
        set_color(port, d->color);
        draw_sprite(port, d->base, d->x, d->y, d->width, d->height, d->data_offsets, d->outline_offset, d->flags);
    }
}

// Shadow masks: the game's own patterns, and arbitrary ones.
static void pick_shadow(Rng* rng, SpriteCase* c)
{
    static const u32 game[][4] = {
        {0x00ff00ff, 0, 0xff00ff00, 0},
        {0x00ffffff, 0, 0xffff00ff, 0},
        {0xff00ff00, 0x00140014, 0x00ff00ff, 0x14001400},
        {0x00ff00ff, 0x28002800, 0xff00ff00, 0x00280028},
        {0x00ff00ff, 0x14001400, 0xff00ff00, 0x14001400},
    };
    int i;
    if (rng_chance(rng, 50)) {
        int k = rng_range(rng, 0, 4);
        for (i = 0; i < 4; ++i) c->shadow[i] = game[k][i];
    } else {
        for (i = 0; i < 4; ++i) c->shadow[i] = rng_next(rng);
    }
    c->use_shadow_ex = rng_chance(rng, 30);
    for (i = 0; i < 8; ++i) c->shadow_table[i] = rng_next(rng);
}

static void sprite_cases(const char* function, int salt, int shadow_mode)
{
    int count = cases_for(6000), i, k;
    if (!wants_function(function)) return;
    for (i = 0; i < count; ++i) {
        u32 seed = suite_options()->seed ^ (u32)salt ^ (u32)i * 2654435761UL;
        Rng rng;
        SpriteCase c;
        int style;
        rng_seed(&rng, seed);
        arena_reset();
        style = rng_range(&rng, 0, 2);
        make_surface(&rng, &c.surface, rng_range(&rng, 8, 320), rng_range(&rng, 4, 40), style);
        c.xlate = (u8*)arena_alloc(256, 4, "xlate_table");
        rng_fill(&rng, c.xlate, 256);
        pick_shadow(&rng, &c);
        if (shadow_mode) c.use_shadow_ex = shadow_mode == 2;
        c.draws = rng_range(&rng, 1, 4);
        for (k = 0; k < c.draws; ++k) {
            SpriteDraw* d = &c.draw[k];
            int width = rng_range(&rng, 1, rng_chance(&rng, 20) ? 700 : 80);
            int height = rng_range(&rng, 1, 30);
            // Extended commands crash the assembly's clipped path, so only
            // emit them when every row is drawn unclipped: one span per line
            // covering everything, and a sprite inside the limits.
            int unclipped = style == 1 && c.surface.min_span == 0 && c.surface.max_span == c.surface.width - 1 &&
                            width <= c.surface.width && rng_chance(&rng, 50);
            d->flags = rng_range(&rng, 0, 3);
            if (shadow_mode) d->flags |= 1;
            make_frame(&rng, d, width, height, unclipped, d->flags & 2);
            if (unclipped) {
                d->x = rng_range(&rng, 0, c.surface.width - width);
                d->y = rng_range(&rng, (int)c.surface.min_line - 3, (int)c.surface.max_line + 3);
            } else {
                d->x = rng_range(&rng, -width - 4, c.surface.width + 4);
                d->y = rng_range(&rng, -height - 2, c.surface.height + 2);
            }
            d->color = (int)rng_next(&rng);
        }
        run_case(function, i, seed, sprite_body, &c);
    }
}

void suite_asm_draw()
{
    setter_cases();
    hline_cases("ASMDraw_HLine", 0x6600000);
    hline_cases("ASMSet_Surface_Info", 0x6700000);
    sprite_cases("ASMDraw_Sprite", 0x7700000, 0);
    sprite_cases("ASMSet_Shadowing", 0x7800000, 1);
    sprite_cases("ASMSet_ShadowingEx", 0x7900000, 2);
}
