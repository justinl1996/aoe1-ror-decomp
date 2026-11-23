#ifndef AOE1_HD_RESOLUTIONS_H
#define AOE1_HD_RESOLUTIONS_H

struct AOE_ResolutionPreset {
    long width;
    long height;
    long menu_bina_base; // UPatch HD BINA base: screen_id = base + (info_id - 50050)
    long hud_slp_base;   // UPatch HD HUD SLP base: slp_id = base + culture (0..4)
    const char* label;
};

static const int AOE_HD_RESOLUTION_COUNT = 3;
static const AOE_ResolutionPreset AOE_HD_RESOLUTIONS[AOE_HD_RESOLUTION_COUNT] = {
    { 1280, 720,  52140, 54250, "1280 x 720"  },
    { 1600, 900,  52260, 54850, "1600 x 900"  },
    { 1920, 1080, 52300, 55050, "1920 x 1080" },
};

static inline const AOE_ResolutionPreset* aoe_find_resolution(long wid, long hgt) {
    for (int i = 0; i < AOE_HD_RESOLUTION_COUNT; ++i) {
        if (AOE_HD_RESOLUTIONS[i].width == wid && (hgt == 0 || AOE_HD_RESOLUTIONS[i].height == hgt))
            return &AOE_HD_RESOLUTIONS[i];
    }
    return 0;
}

static inline const AOE_ResolutionPreset& aoe_resolution_or_default(long wid, long hgt) {
    const AOE_ResolutionPreset* preset = aoe_find_resolution(wid, hgt);
    if (preset != 0)
        return *preset;
    if (wid >= 1920)
        return AOE_HD_RESOLUTIONS[2];
    if (wid >= 1600)
        return AOE_HD_RESOLUTIONS[1];
    return AOE_HD_RESOLUTIONS[0];
}

#endif // AOE1_HD_RESOLUTIONS_H
