#ifndef AOE1_RGE_DAMAGE_SPRITE_INFO_H
#define AOE1_RGE_DAMAGE_SPRITE_INFO_H

class RGE_Sprite;

class RGE_Damage_Sprite_Info {
  public:
    RGE_Sprite* sprite;
    unsigned char damage_percent;
    unsigned char flag;
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char rge_damage_sprite_info_size_check[(sizeof(RGE_Damage_Sprite_Info) == 0x8) ? 1 : -1];
#endif

#endif
