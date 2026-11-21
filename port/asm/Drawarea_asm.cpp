// C++ port of src/Drawarea_asm.asm: the three assembly-owned TDrawArea methods.
//
// Behavior follows the assembly, including its quirks:
//   - PtrClear clips "whole area" requests against Width/Height, not Width-1.
//   - PtrSpanCopy locks the destination when the *source* area's draw system
//     is type 2, and never unlocks it.
//   - OverlayMemCopy copies whole DWORDs (width rounds up), one DWORD at a
//     time, forward or backward, so overlapping rows smear as `rep movsd` does.
//   - Row and span counts are not checked for zero; callers never pass empty
//     rectangles, and the assembly would run away on them.
// tests/asm_port/ compares these with the assembly; see tools/asm_port_harness.py.
#include "drawarea.h"

#include <string.h>

static char ptr_clear_name[] = "PtrClear";
static char span_copy_name[] = "PtrSpanCopy";
static char overlay_copy_name[] = "OverlayMemCopy";

static int uses_locked_surface(TDrawArea* area)
{
    return area->DrawSystem && area->DrawSystem->DrawType == 2;
}

// One `movsd`: read four bytes, then write them.
static void move_dword(unsigned char* dest, const unsigned char* source)
{
    unsigned char value[4];
    memcpy(value, source, 4);
    memcpy(dest, value, 4);
}

// `rep movsb` up to DWORD alignment of dest, `rep movsd`, then `rep movsb`,
// as the assembly's inline copy does. The DWORD steps matter when source and
// destination overlap.
static void copy_aligned(unsigned char* dest, const unsigned char* source, long count)
{
    long head = (long)(-(long)(unsigned long)dest & 3);
    long rest = count - head;
    long i;
    if (rest <= 0) {
        for (i = 0; i < count; ++i) dest[i] = source[i];
        return;
    }
    for (i = 0; i < head; ++i) *dest++ = *source++;
    for (i = rest >> 2; i > 0; --i) {
        move_dword(dest, source);
        dest += 4;
        source += 4;
    }
    for (i = rest & 3; i > 0; --i) *dest++ = *source++;
}

void TDrawArea::PtrClear(tagRECT* rect, int color)
{
    long left, top, right, bottom;
    long rows, width, y;
    if (rect) {
        left = rect->left;
        top = rect->top;
        right = rect->right;
        bottom = rect->bottom;
    } else {
        left = 0;
        top = 0;
        right = this->Width;
        bottom = this->Height;
    }
    if (right < 0 || left > right || left >= this->Width) return;
    if (bottom < 0 || top > bottom || top >= this->Height) return;
    if (left < 0) left = 0;
    if (right >= this->Width) right = this->Width - 1;
    if (top < 0) top = 0;
    if (bottom >= this->Height) bottom = this->Height - 1;
    rows = bottom - top + 1;
    width = right - left + 1;
    if (uses_locked_surface(this) && !this->Lock(ptr_clear_name, 1)) return;

    y = top;
    do {
        unsigned char* row = (unsigned char*)this->CurDisplayOffsets[y] + left;
        memset(row, (unsigned char)color, width);
        ++y;
    } while (--rows != 0);

    if (uses_locked_surface(this)) this->Unlock(ptr_clear_name);
}

void TDrawArea::PtrSpanCopy(TDrawArea* dest, long x, long y, tagRECT* rect, unsigned char** spans)
{
    long rows, source_row, dest_row;
    if (rect->left < 0 || rect->left > rect->right || rect->right >= this->Width) return;
    if (rect->top < 0 || rect->top > rect->bottom || rect->bottom >= this->Height) return;
    rows = rect->bottom - rect->top + 1;
    if (x < 0 || x >= dest->Width) return;
    if (y < 0 || y >= dest->Height) return;
    if (rows + y > dest->Height) rows = dest->Height - y;
    if (uses_locked_surface(this) && !dest->Lock(span_copy_name, 1)) return;

    source_row = rect->top;
    dest_row = y;
    do {
        unsigned char* source = (unsigned char*)this->CurDisplayOffsets[source_row] + rect->left;
        unsigned char* target = (unsigned char*)dest->CurDisplayOffsets[dest_row] + x;
        VSpan_Node* node = (VSpan_Node*)spans[source_row];
        for (; node; node = node->Next)
            copy_aligned(target + node->StartPx, source + node->StartPx, node->EndPx - node->StartPx + 1);
        ++source_row;
        ++dest_row;
    } while (--rows != 0);
}

void TDrawArea::OverlayMemCopy(tagRECT& src_rect, tagRECT& dest_rect, int dx, int dy, int x_offset, int y_offset)
{
    long rows, dwords, source_x, dest_x, source_row, dest_row, step;
    int reverse = 0;
    void** offsets;
    if (dx == 0 && dy == 0) return;
    if (uses_locked_surface(this) && !this->Lock(overlay_copy_name, 1)) return;

    rows = src_rect.bottom - src_rect.top + 1;
    dwords = (src_rect.right - src_rect.left + 4) >> 2;
    source_x = src_rect.left + x_offset;
    dest_x = dest_rect.left + x_offset;
    // The assembly leaves these unset for dy == 0 until the horizontal case.
    source_row = 0;
    dest_row = 0;
    step = 0;
    if (dy < 0) {
        step = -1;
        source_row = src_rect.bottom + y_offset;
        dest_row = dest_rect.bottom + y_offset;
    } else if (dy > 0) {
        step = 1;
        source_row = src_rect.top + y_offset;
        dest_row = dest_rect.top + y_offset;
    } else {
        step = 1;
        source_row = src_rect.top + y_offset;
        dest_row = dest_rect.top + y_offset;
        if (dx < 0) {
            reverse = 1;
            source_x = x_offset + src_rect.right - 3;
            dest_x = dest_rect.right + x_offset - 3;
        }
    }

    offsets = this->CurDisplayOffsets;
    do {
        unsigned char* source = (unsigned char*)offsets[source_row] + source_x;
        unsigned char* target = (unsigned char*)offsets[dest_row] + dest_x;
        long i;
        for (i = 0; i < dwords; ++i) {
            move_dword(target, source);
            if (reverse) {
                target -= 4;
                source -= 4;
            } else {
                target += 4;
                source += 4;
            }
        }
        source_row += step;
        dest_row += step;
    } while (--rows != 0);

    if (uses_locked_surface(this)) this->Unlock(overlay_copy_name);
}
