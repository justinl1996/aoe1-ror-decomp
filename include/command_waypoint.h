#ifndef AOE1_COMMAND_WAYPOINT_H
#define AOE1_COMMAND_WAYPOINT_H

// Shared three-byte waypoint record owned by Command.cpp.
struct XYZBYTEPoint {
    unsigned char x;
    unsigned char y;
    unsigned char z;
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char command_xyzbytepoint_size_check[(sizeof(XYZBYTEPoint) == 0x03) ? 1 : -1];
#endif

#endif
