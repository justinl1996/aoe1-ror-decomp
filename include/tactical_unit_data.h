#ifndef AOE1_TACTICAL_UNIT_DATA_H
#define AOE1_TACTICAL_UNIT_DATA_H

#include <stdint.h>

struct UnitData {
    int id;
    int data1;
    int data2;
    int data3;
    int target;
    // A 32-bit millisecond time: the AI saves its gatherers as raw 24-byte
    // records (taitacmd.cpp), which an 8-byte unsigned long would break on
    // 64-bit Linux.
    uint32_t lastTaskTime;
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char tactical_unit_data_size_check[(sizeof(UnitData) == 0x18) ? 1 : -1];
#endif

#endif
