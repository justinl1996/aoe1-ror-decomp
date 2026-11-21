// Differential harness core: run one case against the reference assembly and
// against the C++ port, from the same starting memory, and compare.
//
// A case allocates every buffer the call may touch from the arena, then
// supplies a body that performs the calls. run_case() snapshots the arena,
// runs the body on the reference side, keeps the result, restores the
// snapshot, runs the body on the port side, and compares:
//   - every arena byte not marked opaque;
//   - the sequence of values the body recorded with record()/record_text().
// Pointers recorded with record_ptr() are stored as arena offsets, so both
// sides agree even though the case passes absolute addresses.
#ifndef ASM_PORT_HARNESS_H
#define ASM_PORT_HARNESS_H

#include <stddef.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;

struct Rng {
    u32 state;
};

void rng_seed(Rng* rng, u32 seed);
u32 rng_next(Rng* rng);
int rng_range(Rng* rng, int low, int high); // inclusive
int rng_chance(Rng* rng, int percent);
void rng_fill(Rng* rng, void* data, size_t size);

// Arena: one block that holds all case memory. arena_reset() starts a case.
void arena_reset();
void* arena_alloc(size_t size, size_t align, const char* label);
// Opaque allocations hold private state whose layout may differ between the
// assembly and the port (for example the Inf32 decoder state). They are
// restored between sides but not compared.
void* arena_alloc_opaque(size_t size, size_t align, const char* label);
long arena_offset(const void* pointer); // -1 when outside the arena

// Recording, for the side currently running.
void record(const char* name, long value);
void record_ptr(const char* name, const void* pointer);
void record_text(const char* name, const char* text);

// Which side runs now: 0 for the reference assembly, 1 for the port.
int current_side();

typedef void (*CaseBody)(void* context, int port);

// Marks the current case as skipped for a stated reason (for example a path
// whose reference behavior depends on code addresses). The body may call it.
void skip_case(const char* reason);

struct SuiteOptions {
    int cases;    // cases per function, 0 means the suite's default
    u32 seed;     // base seed
    const char* inf32_data;
};

// Returns 1 when the named function's cases should run.
int wants_function(const char* function);
int run_case(const char* function, int case_index, u32 seed, CaseBody body, void* context);
const SuiteOptions* suite_options();
int cases_for(int default_cases);

// Suites.
void suite_asm_draw();
void suite_drawarea();
void suite_inf32();

#endif
