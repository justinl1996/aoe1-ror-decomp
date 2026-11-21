// Cases for the TDrawArea methods from src/Drawarea_asm.asm.
#include "harness.h"
#include "reference.h"

#include "drawarea.h"

#include <stddef.h>
#include <string.h>

typedef char check_area_width[(offsetof(TDrawArea, Width) == 0x18) ? 1 : -1];
typedef char check_area_height[(offsetof(TDrawArea, Height) == 0x1C) ? 1 : -1];
typedef char check_area_offsets[(offsetof(TDrawArea, CurDisplayOffsets) == 0xC0) ? 1 : -1];
typedef char check_system_type[(offsetof(TDrawSystem, DrawType) == 0x478) ? 1 : -1];
typedef char check_span_start[(offsetof(VSpan_Node, StartPx) == 8) ? 1 : -1];
typedef char check_span_end[(offsetof(VSpan_Node, EndPx) == 12) ? 1 : -1];

// Test doubles. The game links its real Lock and Unlock from drawarea.cpp;
// here they record each call and can refuse the lock.
static int g_lock_fails;

unsigned char* TDrawArea::Lock(char* caller_name, int wait)
{
    record_ptr("Lock.this", this);
    record_text("Lock.name", caller_name);
    record("Lock.wait", wait);
    return g_lock_fails ? 0 : (unsigned char*)this;
}

void TDrawArea::Unlock(char* caller_name)
{
    record_ptr("Unlock.this", this);
    record_text("Unlock.name", caller_name);
}

static TDrawSystem* make_system(Rng* rng)
{
    TDrawSystem* system;
    if (rng_chance(rng, 20)) return 0;
    system = (TDrawSystem*)arena_alloc(sizeof(TDrawSystem), 8, "draw_system");
    memset(system, 0, sizeof(TDrawSystem));
    system->DrawType = (unsigned char)rng_range(rng, 0, 3);
    if (rng_chance(rng, 50)) system->DrawType = 2;
    return system;
}

// An area whose rows sit in one random-filled surface with a random pitch
// and alignment. Rows may be listed in a shuffled order.
static TDrawArea* make_area(Rng* rng, int width, int height, int pad, const char* label)
{
    TDrawArea* area = (TDrawArea*)arena_alloc(sizeof(TDrawArea), 8, label);
    int pitch = width + pad + rng_range(rng, 0, 9);
    int align = rng_range(rng, 0, 3);
    unsigned char* surface = (unsigned char*)arena_alloc(pitch * height + pad * 2 + 8, 4, "surface");
    void** rows = (void**)arena_alloc(sizeof(void*) * height, 4, "row_offsets");
    int y;
    memset(area, 0, sizeof(TDrawArea));
    rng_fill(rng, surface, pitch * height + pad * 2 + 8);
    for (y = 0; y < height; ++y) rows[y] = surface + pad + align + y * pitch;
    if (height > 1 && rng_chance(rng, 15)) {
        int a = rng_range(rng, 0, height - 1), b = rng_range(rng, 0, height - 1);
        void* swap = rows[a];
        rows[a] = rows[b];
        rows[b] = swap;
    }
    area->Width = width;
    area->Height = height;
    area->CurDisplayOffsets = rows;
    area->DisplayOffsets = rows;
    area->DrawSystem = make_system(rng);
    return area;
}

// ---------------------------------------------------------------- PtrClear

struct ClearCase {
    TDrawArea* area;
    tagRECT* rect;
    int color;
    int lock_fails;
};

static void clear_body(void* context, int port)
{
    ClearCase* c = (ClearCase*)context;
    g_lock_fails = c->lock_fails;
    if (port)
        c->area->PtrClear(c->rect, c->color);
    else
        REF_PtrClear(c->area, 0, c->rect, c->color);
}

static void clear_cases()
{
    int count = cases_for(4000), i;
    if (!wants_function("TDrawArea::PtrClear")) return;
    for (i = 0; i < count; ++i) {
        u32 seed = suite_options()->seed ^ 0x1100000UL ^ (u32)i * 2654435761UL;
        Rng rng;
        ClearCase c;
        int width, height;
        rng_seed(&rng, seed);
        arena_reset();
        width = rng_range(&rng, 1, 48);
        height = rng_range(&rng, 1, 24);
        c.area = make_area(&rng, width, height, 16, "area");
        c.rect = 0;
        if (!rng_chance(&rng, 12)) {
            c.rect = (tagRECT*)arena_alloc(sizeof(tagRECT), 4, "rect");
            c.rect->left = rng_range(&rng, -12, width + 12);
            c.rect->top = rng_range(&rng, -6, height + 6);
            c.rect->right = c.rect->left + rng_range(&rng, -4, width + 8);
            c.rect->bottom = c.rect->top + rng_range(&rng, -3, height + 4);
        }
        c.color = (int)rng_next(&rng);
        c.lock_fails = rng_chance(&rng, 15);
        run_case("TDrawArea::PtrClear", i, seed, clear_body, &c);
    }
}

// ---------------------------------------------------------------- PtrSpanCopy

struct SpanCopyCase {
    TDrawArea* source;
    TDrawArea* dest;
    long x, y;
    tagRECT* rect;
    unsigned char** spans;
    int lock_fails;
};

static void span_copy_body(void* context, int port)
{
    SpanCopyCase* c = (SpanCopyCase*)context;
    g_lock_fails = c->lock_fails;
    if (port)
        c->source->PtrSpanCopy(c->dest, c->x, c->y, c->rect, c->spans);
    else
        REF_PtrSpanCopy(c->source, 0, c->dest, c->x, c->y, c->rect, c->spans);
}

static void span_copy_cases()
{
    int count = cases_for(4000), i;
    if (!wants_function("TDrawArea::PtrSpanCopy")) return;
    for (i = 0; i < count; ++i) {
        u32 seed = suite_options()->seed ^ 0x2200000UL ^ (u32)i * 2654435761UL;
        Rng rng;
        SpanCopyCase c;
        int sw, sh, dw, dh, row, span_width;
        rng_seed(&rng, seed);
        arena_reset();
        sw = rng_range(&rng, 1, 48);
        sh = rng_range(&rng, 1, 20);
        c.source = make_area(&rng, sw, sh, 64, "source_area");
        if (rng_chance(&rng, 15)) {
            c.dest = c.source;
        } else {
            dw = rng_range(&rng, 1, 48);
            dh = rng_range(&rng, 1, 20);
            c.dest = make_area(&rng, dw, dh, 64, "dest_area");
        }
        dw = c.dest->Width;
        dh = c.dest->Height;
        c.rect = (tagRECT*)arena_alloc(sizeof(tagRECT), 4, "rect");
        c.rect->left = rng_range(&rng, -2, sw);
        c.rect->top = rng_range(&rng, -2, sh);
        c.rect->right = c.rect->left + rng_range(&rng, -1, sw);
        c.rect->bottom = c.rect->top + rng_range(&rng, -1, sh);
        c.x = rng_range(&rng, -2, dw);
        c.y = rng_range(&rng, -2, dh);
        if (c.dest == c.source && rng_chance(&rng, 60)) {
            // Overlapping rows a few bytes apart, where DWORD steps show.
            c.x = c.rect->left + rng_range(&rng, -3, 3);
            c.y = c.rect->top;
        }
        // Spans are relative to rect.left and stay inside the copied width,
        // so even an unclipped span lands inside the padded rows.
        span_width = c.rect->right - c.rect->left + 1;
        if (span_width < 1) span_width = 1;
        if (span_width > 40) span_width = 40;
        c.spans = (unsigned char**)arena_alloc(sizeof(unsigned char*) * sh, 4, "span_heads");
        for (row = 0; row < sh; ++row) {
            int nodes = rng_range(&rng, 0, 3), n;
            VSpan_Node* previous = 0;
            c.spans[row] = 0;
            for (n = 0; n < nodes; ++n) {
                VSpan_Node* node = (VSpan_Node*)arena_alloc(sizeof(VSpan_Node), 4, "span_node");
                node->StartPx = rng_range(&rng, 0, span_width - 1);
                node->EndPx = rng_range(&rng, node->StartPx, span_width - 1);
                node->Next = 0;
                node->Prev = previous;
                if (previous)
                    previous->Next = node;
                else
                    c.spans[row] = (unsigned char*)node;
                previous = node;
            }
        }
        c.lock_fails = rng_chance(&rng, 15);
        run_case("TDrawArea::PtrSpanCopy", i, seed, span_copy_body, &c);
    }
}

// ---------------------------------------------------------------- OverlayMemCopy

struct OverlayCase {
    TDrawArea* area;
    tagRECT* src_rect;
    tagRECT* dest_rect;
    int dx, dy, x_offset, y_offset;
    int lock_fails;
};

static void overlay_body(void* context, int port)
{
    OverlayCase* c = (OverlayCase*)context;
    g_lock_fails = c->lock_fails;
    if (port)
        c->area->OverlayMemCopy(*c->src_rect, *c->dest_rect, c->dx, c->dy, c->x_offset, c->y_offset);
    else
        REF_OverlayMemCopy(c->area, 0, c->src_rect, c->dest_rect, c->dx, c->dy, c->x_offset, c->y_offset);
}

static void overlay_cases()
{
    int count = cases_for(4000), i;
    if (!wants_function("TDrawArea::OverlayMemCopy")) return;
    for (i = 0; i < count; ++i) {
        u32 seed = suite_options()->seed ^ 0x3300000UL ^ (u32)i * 2654435761UL;
        Rng rng;
        OverlayCase c;
        rng_seed(&rng, seed);
        arena_reset();
        // 64 rows of 160 bytes; rectangles and offsets stay well inside, so
        // every row index and DWORD the copy touches is in the surface.
        c.area = make_area(&rng, 96, 64, 32, "area");
        c.src_rect = (tagRECT*)arena_alloc(sizeof(tagRECT), 4, "src_rect");
        c.dest_rect = (tagRECT*)arena_alloc(sizeof(tagRECT), 4, "dest_rect");
        c.src_rect->left = rng_range(&rng, 4, 40);
        c.src_rect->right = c.src_rect->left + rng_range(&rng, -3, 40);
        c.src_rect->top = rng_range(&rng, 6, 24);
        c.src_rect->bottom = c.src_rect->top + rng_range(&rng, 0, 20);
        c.dx = rng_range(&rng, -1, 1) * rng_range(&rng, 0, 9);
        c.dy = rng_range(&rng, -1, 1) * rng_range(&rng, 0, 5);
        if (rng_chance(&rng, 5)) c.dx = c.dy = 0;
        c.dest_rect->left = c.src_rect->left + c.dx;
        c.dest_rect->right = c.src_rect->right + c.dx;
        c.dest_rect->top = c.src_rect->top + c.dy;
        c.dest_rect->bottom = c.src_rect->bottom + c.dy;
        c.x_offset = rng_range(&rng, -4, 8);
        c.y_offset = rng_range(&rng, 0, 8);
        c.lock_fails = rng_chance(&rng, 15);
        run_case("TDrawArea::OverlayMemCopy", i, seed, overlay_body, &c);
    }
}

void suite_drawarea()
{
    clear_cases();
    span_copy_cases();
    overlay_cases();
}
