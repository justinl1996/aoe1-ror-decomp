#ifndef AOE1_RMM_DBCT_H
#define AOE1_RMM_DBCT_H

// Compiler-facing declarations for the complete
// C:\\msdev\\work\\age1_x1\\rmm_dbct.cpp source module.
// The random-map base and generated-info records stay with their canonical
// owners. This controller owns only its database records and controller ABI.
#include <stdint.h>
#include "rmm_base.h"
#include "rmm_land.h"
#include "rmm_terrain_info.h"
#include "rmm_obj.h"
#include "rmm_elev.h"

class RGE_Game_World;

struct RGE_Land_Data_Entry {
    int32_t land_id;
    uchar terrain_type;
    int32_t land_avoidance_tiles;
    int32_t base_square_radius;
    uchar zone;
    uchar placement_type;
    int32_t x;
    int32_t y;
    uchar amount_of_land_used_percent;
    uchar by_player_flag;
    int32_t radius;
    int32_t fade;
    int32_t clumpiness_factor;
};

typedef char rmm_dbct_land_entry_size_check[(sizeof(RGE_Land_Data_Entry) == 0x2C) ? 1 : -1];

struct RGE_Land_Data {
    int32_t map_edge_buffer[4];
    int32_t map_edge_fade;
    int32_t land_placement_edge;
    int32_t base_terrain;
    int32_t grown_land_percent;
    int32_t id;
    int32_t land_num;
    RGE_Land_Data_Entry* land;
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char rmm_dbct_land_data_size_check[(sizeof(RGE_Land_Data) == 0x2C) ? 1 : -1];
#endif

struct RGE_Terrain_Data_Entry {
    int32_t terrain_percent;
    int32_t terrain_type;
    int32_t clumps;
    int32_t spacing;
    int32_t base_terrain_type;
    int32_t clumpiness_factor;
};

typedef char rmm_dbct_terrain_entry_size_check[(sizeof(RGE_Terrain_Data_Entry) == 0x18) ? 1 : -1];

struct RGE_Terrain_Data {
    int32_t terrain_num;
    RGE_Terrain_Data_Entry* terrain;
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char rmm_dbct_terrain_data_size_check[(sizeof(RGE_Terrain_Data) == 0x08) ? 1 : -1];
#endif

struct RGE_Object_Data_Entry {
    int32_t obj_id;
    int32_t terrain;
    uchar group_flag;
    uchar scale_flag;
    int32_t object_number_per_group;
    int32_t object_number_varience;
    int32_t number_of_groups;
    int32_t group_area;
    int32_t player_id;
    int32_t land_id;
    int32_t land_inner_radius;
    int32_t land_outer_radius;
};

typedef char rmm_dbct_object_entry_size_check[(sizeof(RGE_Object_Data_Entry) == 0x2C) ? 1 : -1];

struct RGE_Object_Data {
    int32_t object_num;
    RGE_Object_Data_Entry* objects;
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char rmm_dbct_object_data_size_check[(sizeof(RGE_Object_Data) == 0x08) ? 1 : -1];
#endif

struct RGE_Elevation_Data_Entry {
    int32_t elevation_percent;
    int32_t elevation;
    int32_t clumps;
    int32_t spacing;
    int32_t base_terrain_type;
    int32_t base_elevation_type;
};

typedef char rmm_dbct_elevation_entry_size_check[(sizeof(RGE_Elevation_Data_Entry) == 0x18) ? 1 : -1];

struct RGE_Elevation_Data {
    int32_t elevation_num;
    RGE_Elevation_Data_Entry* elevation;
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char rmm_dbct_elevation_data_size_check[(sizeof(RGE_Elevation_Data) == 0x08) ? 1 : -1];
#endif

struct RGE_Map_Data_Entry {
    int32_t map_id;
    RGE_Land_Data land_info;
    RGE_Terrain_Data terrain_info;
    RGE_Object_Data object_info;
    RGE_Elevation_Data elevation_info;
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char rmm_dbct_map_entry_size_check[(sizeof(RGE_Map_Data_Entry) == 0x48) ? 1 : -1];
#endif

struct RGE_Map_Data {
    int32_t map_num;
    RGE_Map_Data_Entry* maps;
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char rmm_dbct_map_data_size_check[(sizeof(RGE_Map_Data) == 0x08) ? 1 : -1];
#endif

class RGE_RMM_Database_Controller : public RGE_Random_Map_Module {
  public:
    RGE_Game_World* world;
    long map_type;
    long number_of_players;
    RGE_Land_Info land_info;
    RGE_Terrain_Info terrain_info;
    RGE_Object_Info object_info;
    RGE_Elevation_Info elevation_info;
    RGE_Map_Data map_info;

    RGE_RMM_Database_Controller(int infile);
    RGE_RMM_Database_Controller(char* land_file, char* terrain_file, char* object_file, char* map_file);
    ~RGE_RMM_Database_Controller();
    virtual uchar generate();

  protected:
    virtual void add_land_module(uchar fixed_positions);
    virtual void add_terrain_module();
    virtual void add_object_module();
    virtual void add_elevation_module();
    void calc_land_start(long land_index, long edge, long land_num, RGE_Land_Data_Entry* info);

  public:
    virtual uchar init(RGE_Map* in_map, RGE_Game_World* in_world, uchar type, long player_num, uchar fixed_positions);
    virtual uchar de_init();
    uchar save(int outfile);
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char rmm_dbct_controller_size_check[(sizeof(RGE_RMM_Database_Controller) == 0x4F5C) ? 1 : -1];
#endif

#endif
