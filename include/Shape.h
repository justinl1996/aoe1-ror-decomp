#ifndef AOE1_SHAPE_H
#define AOE1_SHAPE_H

#include "drawarea.h"
#include "Res_file.h"
#include <stdint.h>

typedef unsigned char byte;

typedef struct Shape_Header {
    int32_t bounds;
    int32_t origin;
    int32_t xmin;
    int32_t ymin;
    int32_t xmax;
    int32_t ymax;
} Shape_Header;

typedef struct Shape_File_Header {
    int32_t version;
    int32_t shape_num;
} Shape_File_Header;

typedef struct Shape_Offsets {
    int32_t shape;
    int32_t palette;
} Shape_Offsets;

typedef struct SLhape_File_Header {
    char Version[4];
    int32_t Num_Shapes;
    char Comment[24];
} SLhape_File_Header;

typedef struct Shape_Info {
    uint32_t Shape_Data_Offsets;
    uint32_t Shape_Outline_Offset;
    int32_t Reserved1;
    int32_t Reserved2;
    int32_t Width;
    int32_t Height;
    int32_t Hotspot_X;
    int32_t Hotspot_Y;
} Shape_Info;

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char shape_header_bin_check[(sizeof(Shape_Header) == 0x18) ? 1 : -1];
typedef char shape_file_header_bin_check[(sizeof(Shape_File_Header) == 0x08) ? 1 : -1];
#endif
#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char shape_offsets_bin_check[(sizeof(Shape_Offsets) == 0x08) ? 1 : -1];
typedef char slhape_file_header_bin_check[(sizeof(SLhape_File_Header) == 0x20) ? 1 : -1];
#endif
typedef char shape_info_bin_check[(sizeof(Shape_Info) == 0x20) ? 1 : -1];

struct Shape_Payload {
    byte value;
};

struct Byte_Table {
    union {
        byte table[256];
        byte value[256];
    };
};

struct Shape_Byte_Table {
    byte table[256];
};

typedef tagRECT Rect;
typedef Shape_File_Header ShapeFile;
typedef Shape_Header TShapeHeader;

extern unsigned char shape_file_first;
extern int Shape_Loads;
extern int ASM_Fast_Count;

class DClipInfo_List;

class TShape {
  public:
    byte* shape;
    int load_type;
    int load_size;
    Shape_Header* shape_header;
    Shape_File_Header* head;
    Shape_Offsets* offsets;
    SLhape_File_Header* FShape;
    Shape_Info* shape_info;

    TShape();
    TShape(char* filename, long resource_file_id);
    ~TShape();

  private:
    byte Check_shape(long shape_num, char* proc);

  public:
    int is_loaded();
    byte shape_bounds(long shape_num, short& width, short& height);
    long shape_count();
    byte shape_minmax(long& x_min, long& y_min, long& x_max, long& y_max, long shape_num);
    byte shape_check(long check_x, long check_y, long shape_num);

  private:
    byte shape_draw_unclipped(TDrawArea* drawarea, long shape_x0, long shape_y0, long shape_num);
    byte shape_color_trans_unclipped(TDrawArea* drawarea, long shape_x0, long shape_y0, long shape_num, byte* color_table);
    byte shape_shadow_unclipped(TDrawArea* drawarea, long shape_x0, long shape_y0, byte* color_table);
    byte shape_draw_clipped(
        TDrawArea* drawarea, long shape_x0, long shape_y0, long clip_x0, long clip_y0, long clip_x1, long clip_y1
    );
    byte shape_color_trans_clipped(
        TDrawArea* drawarea, long shape_x0, long shape_y0, long shape_num, long clip_x0, long clip_y0, long clip_x1, long clip_y1,
        byte* color_table
    );
    byte shape_shadow_clipped(
        TDrawArea* drawarea, long shape_x0, long shape_y0, long clip_x0, long clip_y0, long clip_x1, long clip_y1,
        byte* color_table
    );

  public:
    byte shape_draw(TDrawArea* drawarea, long x, long y, long shape_num, byte flags, byte* color_table);

  private:
    byte shape_mirror_unclipped(TDrawArea* drawarea, long shape_x1, long shape_y0);
    byte shape_mirror_color_trans_unclipped(TDrawArea* drawarea, long shape_x1, long shape_y0, long shape_num, byte* color_table);
    byte shape_mirror_shadow_unclipped(TDrawArea* drawarea, long shape_x1, long shape_y0, byte* color_table);
    byte shape_mirror_clipped(
        TDrawArea* drawarea, long shape_x1, long shape_y0, long clip_x0, long clip_y0, long clip_x1, long clip_y1
    );
    byte shape_mirror_color_trans_clipped(
        TDrawArea* drawarea, long shape_x1, long shape_y0, long shape_num, long clip_x0, long clip_y0, long clip_x1, long clip_y1,
        byte* color_table
    );
    byte shape_mirror_shadow_clipped(
        TDrawArea* drawarea, long shape_x1, long shape_y0, long clip_x0, long clip_y0, long clip_x1, long clip_y1,
        byte* color_table
    );

  public:
    byte shape_mirror(TDrawArea* drawarea, long x, long y, long shape_num, byte flags, byte* color_table);

  private:
    byte shape_dithered_unclipped(TDrawArea* drawarea, long shape_x0, long shape_y0, long dx, long dy);
    byte shape_dithered_clipped(
        TDrawArea* drawarea, long shape_x0, long shape_y0, long clip_x0, long clip_y0, long clip_x1, long clip_y1, long dx,
        long dy
    );

  public:
    byte shape_dither(TDrawArea* drawarea, long x, long y, long shape_num, long dx, long dy);
};

#endif
