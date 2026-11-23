#ifndef AOE1_RMM_TERRAIN_INFO_H
#define AOE1_RMM_TERRAIN_INFO_H

struct RGE_Terrain_Info_Line {
    long terrain_size;
    long terrain_generated;
    long terrain_type;
    long clumps;
    long spacing;
    long base_terrain_type;
    long clumpiness_factor;
    long avoid_hot_spots;
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char rmm_terrain_info_line_size_check[(sizeof(RGE_Terrain_Info_Line) == 0x20) ? 1 : -1];
#endif

struct RGE_Terrain_Hot_Spots {
    long x;
    long y;
    long radius;
    long fade;
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char rmm_terrain_hot_spots_size_check[(sizeof(RGE_Terrain_Hot_Spots) == 0x10) ? 1 : -1];
#endif

struct RGE_Terrain_Info {
    RGE_Terrain_Info_Line terrain[99];
    long terrain_num;
    RGE_Terrain_Hot_Spots hot_spots[99];
    long hot_spot_num;
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char rmm_terrain_info_size_check[(sizeof(RGE_Terrain_Info) == 0x1298) ? 1 : -1];
#endif

#endif
