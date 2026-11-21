// Reference entry points: the repository's assembly, assembled with REF_
// names by tools/asm_port_harness.py.
#ifndef ASM_PORT_REFERENCE_H
#define ASM_PORT_REFERENCE_H

struct tagRECT;
class TDrawArea;

extern "C" {
void __cdecl REF_ASMSet_Shadowing(unsigned long mask1, unsigned long value1, unsigned long mask2, unsigned long value2);
void __cdecl REF_ASMSet_ShadowingEx(void* masks_and_values);
void __cdecl REF_ASMSet_Surface_Info(
    void* offsets, void* heads, void* tails, long min_span, long min_line, long max_span, long max_line
);
void __cdecl REF_ASMSet_Color_Xform(int color);
int __cdecl REF_ASMGet_Color_Xform(void);
void __cdecl REF_ASMSet_Xlate_Table(void* table);
void* __cdecl REF_ASMGet_Xlate_Table(void);
void __cdecl REF_ASMDraw_HLine(long x1, long x2, long y, unsigned long color);
void __cdecl REF_ASMDraw_Sprite(
    void* base, long x, long y, long width, long height, long data_offsets, long outline_offset, int flags
);

int __cdecl REF_Inf32BufSize();
int __cdecl REF_Inf32Decode(
    void* input, int input_offset, void* input_size, void* output, int output_offset, void* output_size, void* state,
    int final_input
);

// The TDrawArea methods are __thiscall. Their reference copies are named as
// __fastcall functions: `this` in ECX, an unused EDX, then the stack
// arguments, which the callee pops in both conventions.
void __fastcall REF_PtrClear(TDrawArea* self, int unused, tagRECT* rect, int color);
void __fastcall REF_PtrSpanCopy(
    TDrawArea* self, int unused, TDrawArea* dest, long x, long y, tagRECT* rect, unsigned char** spans
);
void __fastcall REF_OverlayMemCopy(
    TDrawArea* self, int unused, tagRECT* src_rect, tagRECT* dest_rect, int dx, int dy, int x_offset, int y_offset
);
}

#endif
