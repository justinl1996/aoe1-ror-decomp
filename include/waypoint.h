#ifndef AOE1_WAYPOINT_H
#define AOE1_WAYPOINT_H

struct Waypoint {
    float x;
    float y;
    float z;
    unsigned char facetToNextWaypoint;
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char waypoint_size_check[(sizeof(Waypoint) == 0x10) ? 1 : -1];
#endif

#endif
