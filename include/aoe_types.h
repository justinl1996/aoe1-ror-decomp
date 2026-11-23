#ifndef AOE1_TYPES_H
#define AOE1_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef int8_t aoe_i8;
typedef uint8_t aoe_u8;
typedef int16_t aoe_i16;
typedef uint16_t aoe_u16;
typedef int32_t aoe_i32;
typedef uint32_t aoe_u32;
typedef int64_t aoe_i64;
typedef uint64_t aoe_u64;
typedef intptr_t aoe_iptr;
typedef uintptr_t aoe_uptr;

#if defined(__LP64__) || defined(_WIN64) || defined(__wasm64__) || defined(__x86_64__) || defined(__aarch64__)
#define AOE_64BIT 1
#endif

#endif // AOE1_TYPES_H
