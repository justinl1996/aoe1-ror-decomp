// C++ port of src/Asm_draw.asm: the sprite and span drawing engine.
//
// The assembly dispatches every span piece to an unrolled codelet chosen by
// length and destination alignment. Those codelets only change how bytes are
// stored, never which bytes, so the port writes each piece with a plain loop.
// Everything else follows the assembly: clipping against the line and span
// limits, the per-line span lists, the command encoding, and the shadow
// dither state.
//
// tests/asm_port/ compares these functions with the assembly; see
// tools/asm_port_harness.py.
#include "asm_draw.h"

#include "drawarea.h"

#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;

// Engine state set by the ASMSet_* entry points (THIS_DAT in the assembly).
static u8** surface_offsets;
static VSpan_Node** span_heads;
static VSpan_Node** span_tails;
static u32 color_xform; // the color byte replicated into all four bytes
static u8* xlate_table;
static u32 shadow_mask[4];
static u32 shadow_value[4];
static long min_line, max_line, min_span, max_span;

extern "C" void __cdecl ASMSet_Shadowing(unsigned long mask1, unsigned long value1, unsigned long mask2, unsigned long value2)
{
    shadow_mask[0] = mask1;
    shadow_mask[2] = mask1;
    shadow_value[0] = value1;
    shadow_value[2] = value1;
    shadow_mask[1] = mask2;
    shadow_mask[3] = mask2;
    shadow_value[1] = value2;
    shadow_value[3] = value2;
}

// Four masks, then four values, one per line modulo 4.
extern "C" void __cdecl ASMSet_ShadowingEx(void* masks_and_values)
{
    const u32* words = (const u32*)masks_and_values;
    int i;
    for (i = 0; i < 4; ++i) {
        shadow_mask[i] = words[i];
        shadow_value[i] = words[4 + i];
    }
}

extern "C" void __cdecl ASMSet_Surface_Info(
    void* offsets, void* heads, void* tails, long min_span_in, long min_line_in, long max_span_in, long max_line_in
)
{
    surface_offsets = (u8**)offsets;
    span_heads = (VSpan_Node**)heads;
    span_tails = (VSpan_Node**)tails;
    min_line = min_line_in;
    max_line = max_line_in;
    min_span = min_span_in;
    max_span = max_span_in;
}

extern "C" void __cdecl ASMSet_Color_Xform(int color)
{
    u32 c = (u8)color;
    color_xform = c | (c << 8) | (c << 16) | (c << 24);
}

extern "C" int __cdecl ASMGet_Color_Xform(void)
{
    return (int)(color_xform & 0xFF);
}

extern "C" void __cdecl ASMSet_Xlate_Table(void* table)
{
    xlate_table = (u8*)table;
}

extern "C" void* __cdecl ASMGet_Xlate_Table(void)
{
    return xlate_table;
}

// Fill [x1, x2] of line y with color, inside the line's spans.
extern "C" void __cdecl ASMDraw_HLine(long x1, long x2, long y, unsigned long color)
{
    VSpan_Node* node;
    u8* pixels;
    if (y < min_line || y > max_line) return;
    if (x1 > x2) {
        long swap = x1;
        x1 = x2;
        x2 = swap;
    }
    if (x2 < min_span || x1 > max_span) return;
    if (x1 < min_span) x1 = min_span;
    if (x2 > max_span) x2 = max_span;
    pixels = surface_offsets[y] + x1;
    for (node = span_heads[y]; node; node = node->Next) {
        long start, end;
        if (x2 < node->StartPx) return;
        if (x1 > node->EndPx) continue;
        start = x1 < node->StartPx ? node->StartPx : x1;
        end = x2 > node->EndPx ? node->EndPx : x2;
        memset(pixels + (start - x1), (u8)color, end - start + 1);
        if (x2 <= node->EndPx) return;
    }
}

// ---------------------------------------------------------------- sprites
//
// Frame format, relative to `base`:
//   outline_offset: per row, u16 left margin, u16 right margin. 0x8000 in the
//                   margin read first (left normally, right when mirrored)
//                   marks an empty row.
//   data_offsets:   per row, u32 offset from base to the row's commands.
// Commands, by low nibble of the command byte b:
//   xxxxxx00 copy b>>2 pixels        xxxxxx01 skip b>>2 pixels
//   L2 nn    copy L<<8|nn pixels     L3 nn    skip L<<8|nn pixels
//   L6       player-color copy       L7 c     fill with c
//   LA c     player-color fill       LB       shadow (translate dest pixels)
//            (L6/L7/LA/LB: L pixels, or the next byte when L is 0)
//   1E, 2E nn  outline pixel / span: the engine never enables outlines, so
//              these only skip
//   3E, 4E   see the unclipped row drivers
//   0F       end of row
// flags: 1 shadow (dither) mode, 2 mirrored.
//
// A row whose drawn extent lies inside one span is drawn unclipped. Other
// rows are clipped against the span list, piece by piece, following the
// assembly's span walk exactly: a piece that runs past a span's right edge
// is restarted, whole, against the span after the remembered one, so a piece
// that started beyond that span is drawn into its own span twice. Copies and
// fills do not show it; shadow translation is applied twice.

#include "Asm_draw_codelets.inc"

enum {
    TABLE_COPY = 0,
    TABLE_FILL = 1,
    TABLE_PLAYER = 2,
    TABLE_XLATE = 3,
    TABLE_MIRRORED = 4,
};

// Per-draw and per-row state (state_88C02C/030/034/048 in the assembly).
static u32 fill_mask;    // and-mask for the row: a shadow mask, or all ones
static u32 fill_value;   // or-value for fills and copies: a shadow value, or 0
static u32 player_value; // or-value for player colors
static int shadow_mode;

static u8 byte_of(u32 word, int position)
{
    return (u8)(word >> (8 * position));
}

static u32 replicate(u8 value)
{
    u32 v = value;
    return v | (v << 8) | (v << 16) | (v << 24);
}

// Draw one span piece of `count` pixels, as the codelet for this table,
// length and alignment would. `dest` is the first pixel drawn: the leftmost,
// or the rightmost when mirrored. Copies read `source` forward either way.
static void draw_piece(int table, u8* dest, const u8* source, long count, u32 fill_word)
{
    int mirrored = table >= TABLE_MIRRORED, operation = table & 3;
    long index, j;
    int row, align = (int)((unsigned long)dest & 3);
    const CodeletShape* shape;
    int step = mirrored ? -1 : 1;
    if (count <= 0) return;
    index = (count & 0xFF) | (count >= 256 ? 0x80 : 0);
    row = index < 16 ? (int)index : 16 + (int)(index & 15);
    shape = &kShadowShapes[table][row][align];
    for (j = 0; j < count; ++j) {
        u8* pixel = dest + j * step;
        int position = (int)((unsigned long)pixel & 3);
        long source_index = j;
        u8 value;
        if (shadow_mode) {
            if (j < shape->head)
                position = kShadowPositions[shape->offset + j];
            else if (j >= count - shape->tail)
                position = (position + kShadowPositions[shape->offset + shape->head + (j - (count - shape->tail))]) & 3;
        } else if (table == (TABLE_PLAYER | TABLE_MIRRORED) && count == 7 && align == 3 && j == 6) {
            // The 7-pixel mirrored player-color codelet (L567770) stores
            // source byte 4 again where byte 6 belongs.
            source_index = 4;
        }
        switch (operation) {
        case TABLE_COPY:
            value = source[source_index];
            if (shadow_mode) value = (u8)((value & byte_of(fill_mask, position)) | byte_of(fill_value, position));
            break;
        case TABLE_PLAYER:
            value = source[source_index];
            if (shadow_mode)
                value = (u8)((value & byte_of(fill_mask, position)) | byte_of(player_value, position));
            else
                value = (u8)(value | byte_of(color_xform, 0));
            break;
        case TABLE_FILL:
            value = byte_of(fill_word, shadow_mode ? position : 0);
            break;
        default:
            value = xlate_table[*pixel];
            if (shadow_mode) {
                // Normal codelets add the value, then mask; mirrored ones
                // mask first.
                if (mirrored)
                    value = (u8)((value & byte_of(fill_mask, position)) | byte_of(fill_value, position));
                else
                    value = (u8)((value | byte_of(fill_value, position)) & byte_of(fill_mask, position));
            }
            break;
        }
        *pixel = value;
    }
}

// Count from the high nibble, or from the next byte when the nibble is 0.
static long nibble_count(u32 command, const u8** cursor)
{
    long count = (long)(command >> 4);
    if (!count) count = *(*cursor)++;
    return count;
}

static long big_count(u32 command, const u8** cursor)
{
    return (long)((command >> 4) << 8) | *(*cursor)++;
}

// L56DB60: step over one command without drawing it; returns its pixels.
static long skip_command(const u8** cursor)
{
    u32 command = *(*cursor)++;
    long count;
    switch (command & 15) {
    case 0: case 4: case 8: case 12:
        count = (long)(command >> 2);
        *cursor += count;
        return count;
    case 1: case 5: case 9: case 13:
        return (long)(command >> 2);
    case 2:
        count = big_count(command, cursor);
        *cursor += count;
        return count;
    case 3:
        return big_count(command, cursor);
    case 6:
        count = nibble_count(command, cursor);
        *cursor += count;
        return count;
    case 7: case 10:
        count = nibble_count(command, cursor);
        ++*cursor;
        return count;
    case 11:
        return nibble_count(command, cursor);
    case 14:
        if ((command >> 4) == 1) return 1;
        if ((command >> 4) == 2) return *(*cursor)++;
        return 0; // int 3 in the assembly
    default:
        return 0;
    }
}

static u32 fill_word_for(u32 command, u8 color)
{
    u32 word = replicate(color) & fill_mask;
    return word | ((command & 15) == 10 ? player_value : fill_value);
}

// Unclipped row, left to right (L56D0A0).
static void row_unclipped(u8* dest, const u8* cursor)
{
    for (;;) {
        u32 command = *cursor++;
        long count;
        switch (command & 15) {
        case 0: case 4: case 8: case 12:
            count = (long)(command >> 2);
            draw_piece(TABLE_COPY, dest, cursor, count, 0);
            cursor += count;
            dest += count;
            break;
        case 1: case 5: case 9: case 13:
            dest += command >> 2;
            break;
        case 2:
            count = big_count(command, &cursor);
            draw_piece(TABLE_COPY, dest, cursor, count, 0);
            cursor += count;
            dest += count;
            break;
        case 3:
            dest += big_count(command, &cursor);
            break;
        case 6:
            count = nibble_count(command, &cursor);
            draw_piece(TABLE_PLAYER, dest, cursor, count, 0);
            cursor += count;
            dest += count;
            break;
        case 7: case 10:
            count = nibble_count(command, &cursor);
            draw_piece(TABLE_FILL, dest, 0, count, fill_word_for(command, *cursor));
            ++cursor;
            dest += count;
            break;
        case 11:
            count = nibble_count(command, &cursor);
            draw_piece(TABLE_XLATE, dest, 0, count, 0);
            dest += count;
            break;
        case 14:
            switch (command >> 4) {
            case 1: dest += 1; break;          // outline pixel
            case 2: dest += *cursor++; break;  // outline span
            case 3: break;                     // no-op
            case 4: dest += skip_command(&cursor); break; // draw the next command as transparent
            default: return;                   // int 3 in the assembly
            }
            break;
        default:
            return;
        }
    }
}

// Unclipped row, right to left (L56D420). Here 3E skips the next command and
// 4E is the no-op, and the skip moves right, not left, as in the assembly.
static void row_unclipped_mirrored(u8* dest, const u8* cursor)
{
    for (;;) {
        u32 command = *cursor++;
        long count;
        switch (command & 15) {
        case 0: case 4: case 8: case 12:
            count = (long)(command >> 2);
            draw_piece(TABLE_COPY | TABLE_MIRRORED, dest, cursor, count, 0);
            cursor += count;
            dest -= count;
            break;
        case 1: case 5: case 9: case 13:
            dest -= command >> 2;
            break;
        case 2:
            count = big_count(command, &cursor);
            draw_piece(TABLE_COPY | TABLE_MIRRORED, dest, cursor, count, 0);
            cursor += count;
            dest -= count;
            break;
        case 3:
            dest -= big_count(command, &cursor);
            break;
        case 6:
            count = nibble_count(command, &cursor);
            draw_piece(TABLE_PLAYER | TABLE_MIRRORED, dest, cursor, count, 0);
            cursor += count;
            dest -= count;
            break;
        case 7: case 10:
            count = nibble_count(command, &cursor);
            draw_piece(TABLE_FILL | TABLE_MIRRORED, dest, 0, count, fill_word_for(command, *cursor));
            ++cursor;
            dest -= count;
            break;
        case 11:
            count = nibble_count(command, &cursor);
            draw_piece(TABLE_XLATE | TABLE_MIRRORED, dest, 0, count, 0);
            dest -= count;
            break;
        case 14:
            switch (command >> 4) {
            case 1: dest -= 1; break;
            case 2: dest -= *cursor++; break;
            case 3: dest += skip_command(&cursor); break;
            case 4: break;
            default: return;
            }
            break;
        default:
            return;
        }
    }
}

// Clipped rows (L56CC20 and L56D6C0). `span` is the remembered span
// (draw_span); each command starts its search there, and only a restart
// advances it.
static void row_clipped(u8* row_base, u8* dest, const u8* cursor, VSpan_Node* span, int mirrored)
{
    for (;;) {
        const u8* command_start = cursor;
        u8* command_dest = dest;
        VSpan_Node* node = span;
        long x = (long)(dest - row_base);
        u32 command = *cursor++;
        int table, kind = (int)(command & 15);
        long count, skip;
        u32 fill = 0;
        int restart;

        // Skips, ends and extended commands.
        switch (kind) {
        case 1: case 5: case 9: case 13:
            dest = mirrored ? command_dest - (command >> 2) : command_dest + (command >> 2);
            continue;
        case 3:
            count = big_count(command, &cursor);
            dest = mirrored ? command_dest - count : command_dest + count;
            continue;
        case 14:
            switch (command >> 4) {
            case 1:
                dest = mirrored ? command_dest - 1 : command_dest + 1;
                continue;
            case 2:
                count = *cursor++;
                dest = mirrored ? command_dest - count : command_dest + count;
                continue;
            default:
                // 3E/4E: the assembly continues with a span-relative x as the
                // destination pointer and faults on the next write; int 3
                // otherwise. Stop the row instead.
                return;
            }
        case 15:
            return;
        }

        // Drawing commands: count, data, and codelet table.
        switch (kind) {
        case 0: case 4: case 8: case 12:
            count = (long)(command >> 2);
            table = TABLE_COPY;
            break;
        case 2:
            count = big_count(command, &cursor);
            table = TABLE_COPY;
            break;
        case 6:
            count = nibble_count(command, &cursor);
            table = TABLE_PLAYER;
            break;
        case 7: case 10:
            count = nibble_count(command, &cursor);
            table = TABLE_FILL;
            break;
        default: // 11
            count = nibble_count(command, &cursor);
            table = TABLE_XLATE;
            break;
        }

        restart = -1; // -1: piece skipped, 0: drawn, 1: drawn and clipped on the trailing edge
        for (;;) {
            long low, high;
            if (mirrored) {
                high = x;
                low = x - count + 1;
                if (low > node->EndPx) break; // entirely right of this span
                if (high < node->StartPx) {
                    node = node->Prev;
                    if (!node) return;
                    continue;
                }
            } else {
                low = x;
                high = x + count - 1;
                if (high < node->StartPx) break; // entirely left of this span
                if (low > node->EndPx) {
                    node = node->Next;
                    if (!node) return;
                    continue;
                }
            }
            // Visible: clip the leading edge (consuming source pixels for
            // copies), then the trailing edge.
            skip = 0;
            if (mirrored ? high > node->EndPx : low < node->StartPx) {
                skip = mirrored ? high - node->EndPx : node->StartPx - low;
                if (table == TABLE_COPY || table == TABLE_PLAYER) cursor += skip;
                count -= skip;
            }
            restart = 0;
            if (mirrored ? low < node->StartPx : high > node->EndPx) {
                count -= mirrored ? node->StartPx - low : high - node->EndPx;
                restart = 1;
            }
            dest = mirrored ? command_dest - skip : command_dest + skip;
            if (table == TABLE_FILL) {
                fill = fill_word_for(command, *cursor);
                ++cursor;
            }
            draw_piece(mirrored ? table | TABLE_MIRRORED : table, dest, cursor, count, fill);
            if (table == TABLE_COPY || table == TABLE_PLAYER) cursor += count;
            dest = mirrored ? dest - count : dest + count;
            break;
        }
        if (restart < 0) {
            // Skipped piece: step over its data.
            if (table == TABLE_COPY || table == TABLE_PLAYER) cursor += count;
            if (table == TABLE_FILL) ++cursor;
            dest = mirrored ? command_dest - count : command_dest + count;
        } else if (restart) {
            // L56CBC0 / L56D660: run the same command again against the span
            // after the remembered one, which becomes the remembered one.
            VSpan_Node* next = mirrored ? span->Prev : span->Next;
            if (!next) return;
            span = next;
            cursor = command_start;
            dest = command_dest;
        }
    }
}

extern "C" void __cdecl ASMDraw_Sprite(
    void* base, long x, long y, long width, long height, long data_offsets, long outline_offset, int flags
)
{
    long rows = height, bottom = y + height - 1, skipped = 0, right = x + width - 1, row;
    const u8* commands;
    const u16* outlines;
    int mirrored = (flags & 2) != 0;
    if (bottom < min_line || y > max_line) return;
    if (y < min_line) {
        skipped = min_line - y;
        rows -= skipped;
        y = 0; // the assembly stores 0 here, not min_line
    }
    if (bottom > max_line) rows -= bottom - max_line;
    if (x > max_span || right < min_span) return;
    commands = (const u8*)base + data_offsets + skipped * 4;
    outlines = (const u16*)((const u8*)base + outline_offset + skipped * 4);
    shadow_mode = flags & 1;
    if (!shadow_mode) {
        fill_value = 0;
        fill_mask = 0xFFFFFFFFUL;
        player_value = color_xform;
    }

    row = 0;
    do {
        long line = y + row;
        VSpan_Node* node = mirrored ? span_tails[line] : span_heads[line];
        const u16* outline = outlines + row * 2;
        const u8* cursor;
        u8* row_base;
        long start, end;
        if (!node) continue;
        if (shadow_mode) {
            fill_value = shadow_value[line & 3];
            fill_mask = shadow_mask[line & 3];
            player_value = (color_xform & fill_mask) | fill_value;
        }
        if (outline[mirrored ? 1 : 0] & 0x8000) continue;
        if (mirrored) {
            start = x + outline[1];
            end = right - outline[0];
        } else {
            start = x + outline[0];
            end = right - outline[1];
        }
        row_base = surface_offsets[line];
        cursor = (const u8*)base + *(const u32*)(commands + row * 4);
        if (mirrored) {
            for (;;) {
                if (start > node->EndPx) break;
                if (end >= node->StartPx) break;
                node = node->Prev;
                if (!node) break;
            }
            if (!node || start > node->EndPx) continue;
            if (start >= node->StartPx && end <= node->EndPx)
                row_unclipped_mirrored(row_base + end, cursor);
            else
                row_clipped(row_base, row_base + end, cursor, node, 1);
        } else {
            for (;;) {
                if (end < node->StartPx) break;
                if (start <= node->EndPx) break;
                node = node->Next;
                if (!node) break;
            }
            if (!node || end < node->StartPx) continue;
            if (start >= node->StartPx && end <= node->EndPx)
                row_unclipped(row_base + start, cursor);
            else
                row_clipped(row_base, row_base + start, cursor, node, 0);
        }
    } while (++row < rows);
}
