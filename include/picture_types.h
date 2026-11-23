#ifndef AOE1_PICTURE_TYPES_H
#define AOE1_PICTURE_TYPES_H

struct LINEINFO {
    int AllTrans;
    int MidSolid;
    long StartX;
    long EndX;
};

struct TRANSINFO {
    int AnyTrans;
    long x;
    long y;
    long w;
    long h;
    LINEINFO* LineInfo;
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char picture_line_info_size_check[(sizeof(LINEINFO) == 0x10) ? 1 : -1];
#endif
#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char picture_trans_info_size_check[(sizeof(TRANSINFO) == 0x18) ? 1 : -1];
#endif

#endif
