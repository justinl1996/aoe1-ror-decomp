// Differential harness core and command line. See harness.h.
#include "harness.h"

#include <excpt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Functions in report order. Suites look themselves up here by name.
static const char* const kFunctions[] = {
    "ASMSet_Color_Xform", "ASMGet_Color_Xform", "ASMSet_Xlate_Table", "ASMGet_Xlate_Table",
    "ASMSet_Shadowing", "ASMSet_ShadowingEx", "ASMSet_Surface_Info",
    "ASMDraw_HLine", "ASMDraw_Sprite",
    "TDrawArea::PtrClear", "TDrawArea::PtrSpanCopy", "TDrawArea::OverlayMemCopy",
    "Inf32BufSize", "Inf32Decode",
};
enum { kFunctionCount = sizeof(kFunctions) / sizeof(kFunctions[0]) };

struct FunctionStats {
    int cases;
    int failures;
    int skipped;
};
static FunctionStats g_stats[kFunctionCount];

static const char* g_only[32];
static int g_only_count;
static int g_max_print = 5;
static SuiteOptions g_options;

// ---------------------------------------------------------------- random

void rng_seed(Rng* rng, u32 seed)
{
    rng->state = seed ? seed : 0x9E3779B9UL;
    rng_next(rng);
}

u32 rng_next(Rng* rng)
{
    // xorshift32
    u32 x = rng->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng->state = x;
    return x;
}

int rng_range(Rng* rng, int low, int high)
{
    if (high <= low) return low;
    return low + (int)(rng_next(rng) % (u32)(high - low + 1));
}

int rng_chance(Rng* rng, int percent)
{
    return (int)(rng_next(rng) % 100) < percent;
}

void rng_fill(Rng* rng, void* data, size_t size)
{
    u8* bytes = (u8*)data;
    size_t i;
    for (i = 0; i < size; ++i) bytes[i] = (u8)(rng_next(rng) >> 11);
}

// ---------------------------------------------------------------- arena

enum { kArenaSize = 12 * 1024 * 1024, kMaxBlocks = 2048 };

struct Block {
    size_t offset;
    size_t size;
    int opaque;
    const char* label;
};

static u8* g_arena;
static u8* g_pristine;
static u8* g_reference;
static size_t g_used;
static Block g_blocks[kMaxBlocks];
static int g_block_count;

void arena_reset()
{
    g_used = 0;
    g_block_count = 0;
}

static void* allocate(size_t size, size_t align, const char* label, int opaque)
{
    size_t offset;
    if (align < 1) align = 1;
    // Leave a guard gap between blocks so an overrun lands in compared memory.
    offset = (g_used + 64 + align - 1) / align * align;
    if (offset + size + 64 > kArenaSize || g_block_count == kMaxBlocks) {
        printf("FATAL arena exhausted allocating %s (%lu bytes)\n", label, (unsigned long)size);
        exit(3);
    }
    g_blocks[g_block_count].offset = offset;
    g_blocks[g_block_count].size = size;
    g_blocks[g_block_count].opaque = opaque;
    g_blocks[g_block_count].label = label;
    ++g_block_count;
    g_used = offset + size;
    return g_arena + offset;
}

void* arena_alloc(size_t size, size_t align, const char* label)
{
    return allocate(size, align, label, 0);
}

void* arena_alloc_opaque(size_t size, size_t align, const char* label)
{
    return allocate(size, align, label, 1);
}

long arena_offset(const void* pointer)
{
    const u8* p = (const u8*)pointer;
    if (p < g_arena || p >= g_arena + kArenaSize) return -1;
    return (long)(p - g_arena);
}

static const Block* block_for(size_t offset)
{
    int i;
    for (i = 0; i < g_block_count; ++i)
        if (offset >= g_blocks[i].offset && offset < g_blocks[i].offset + g_blocks[i].size) return &g_blocks[i];
    return 0;
}

static int is_opaque(size_t offset)
{
    const Block* block = block_for(offset);
    return block && block->opaque;
}

// ---------------------------------------------------------------- records

enum { kMaxRecords = 8192, kNameSize = 40, kTextSize = 48 };

struct Record {
    char name[kNameSize];
    char text[kTextSize];
    long value;
    int kind; // 0 value, 1 pointer, 2 text
};

struct RecordSet {
    Record rows[kMaxRecords];
    int count;
    int overflow;
};

static RecordSet* g_records[2];
static int g_side;
static const char* g_skip_reason;

int current_side()
{
    return g_side;
}

static Record* next_record(const char* name, int kind)
{
    RecordSet* set = g_records[g_side];
    Record* row;
    if (set->count == kMaxRecords) {
        set->overflow = 1;
        return 0;
    }
    row = &set->rows[set->count++];
    strncpy(row->name, name, kNameSize - 1);
    row->name[kNameSize - 1] = 0;
    row->text[0] = 0;
    row->value = 0;
    row->kind = kind;
    return row;
}

void record(const char* name, long value)
{
    Record* row = next_record(name, 0);
    if (row) row->value = value;
}

void record_ptr(const char* name, const void* pointer)
{
    Record* row = next_record(name, 1);
    if (row) row->value = pointer ? arena_offset(pointer) : -2;
}

void record_text(const char* name, const char* text)
{
    Record* row = next_record(name, 2);
    if (row) {
        strncpy(row->text, text ? text : "(null)", kTextSize - 1);
        row->text[kTextSize - 1] = 0;
    }
}

void skip_case(const char* reason)
{
    if (!g_skip_reason) g_skip_reason = reason;
}

// ---------------------------------------------------------------- cases

static int function_index(const char* function)
{
    int i;
    for (i = 0; i < kFunctionCount; ++i)
        if (!strcmp(kFunctions[i], function)) return i;
    printf("FATAL unknown function %s\n", function);
    exit(3);
    return -1;
}

int wants_function(const char* function)
{
    int i;
    if (!g_only_count) return 1;
    for (i = 0; i < g_only_count; ++i)
        if (!strcmp(g_only[i], function)) return 1;
    return 0;
}

const SuiteOptions* suite_options()
{
    return &g_options;
}

int cases_for(int default_cases)
{
    return g_options.cases > 0 ? g_options.cases : default_cases;
}

static unsigned long g_fault_code;

static int run_body(CaseBody body, void* context)
{
    __try {
        body(context, g_side);
    } __except (g_fault_code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
    return 0;
}

static void describe_record(const Record* row, char* out, size_t size)
{
    if (!row) {
        _snprintf(out, size, "(missing)");
    } else if (row->kind == 2) {
        _snprintf(out, size, "%s=\"%s\"", row->name, row->text);
    } else if (row->kind == 1) {
        _snprintf(out, size, "%s=@%ld", row->name, row->value);
    } else {
        _snprintf(out, size, "%s=%ld (0x%lX)", row->name, row->value, (unsigned long)row->value);
    }
    out[size - 1] = 0;
}

static int records_equal(const Record* a, const Record* b)
{
    if (a->kind != b->kind || strcmp(a->name, b->name)) return 0;
    if (a->kind == 2) return !strcmp(a->text, b->text);
    return a->value == b->value;
}

int run_case(const char* function, int case_index, u32 seed, CaseBody body, void* context)
{
    FunctionStats* stats = &g_stats[function_index(function)];
    size_t used = g_used;
    int faulted[2];
    unsigned long fault_codes[2];
    char detail[512];
    int failed = 0;
    size_t i;

    g_skip_reason = 0;
    memcpy(g_pristine, g_arena, used);
    for (g_side = 0; g_side < 2; ++g_side) {
        g_records[g_side]->count = 0;
        g_records[g_side]->overflow = 0;
        if (g_side == 1) {
            memcpy(g_reference, g_arena, used);
            memcpy(g_arena, g_pristine, used);
        }
        g_fault_code = 0;
        faulted[g_side] = run_body(body, context);
        fault_codes[g_side] = g_fault_code;
        if (faulted[g_side]) record("fault", (long)g_fault_code);
    }
    g_side = 0;
    detail[0] = 0;

    if (g_skip_reason) {
        ++stats->skipped;
        return 1;
    }
    ++stats->cases;

    if (faulted[0] != faulted[1] || fault_codes[0] != fault_codes[1]) {
        _snprintf(detail, sizeof(detail), "fault mismatch: reference %s 0x%lX, port %s 0x%lX",
                  faulted[0] ? "faulted" : "ok", fault_codes[0], faulted[1] ? "faulted" : "ok", fault_codes[1]);
        failed = 1;
    }
    if (!failed) {
        RecordSet* ref = g_records[0];
        RecordSet* port = g_records[1];
        int count = ref->count > port->count ? ref->count : port->count;
        int r;
        for (r = 0; r < count && !failed; ++r) {
            const Record* a = r < ref->count ? &ref->rows[r] : 0;
            const Record* b = r < port->count ? &port->rows[r] : 0;
            if (!a || !b || !records_equal(a, b)) {
                char left[128], right[128];
                describe_record(a, left, sizeof(left));
                describe_record(b, right, sizeof(right));
                _snprintf(detail, sizeof(detail), "record %d: reference %s, port %s", r, left, right);
                failed = 1;
            }
        }
        if (!failed && ref->overflow != port->overflow) {
            _snprintf(detail, sizeof(detail), "record overflow differs");
            failed = 1;
        }
    }
    if (!failed) {
        size_t first = (size_t)-1, differing = 0;
        for (i = 0; i < used; ++i) {
            if (g_arena[i] != g_reference[i] && !is_opaque(i)) {
                if (first == (size_t)-1) first = i;
                ++differing;
            }
        }
        if (differing) {
            const Block* block = block_for(first);
            _snprintf(detail, sizeof(detail), "memory: %lu byte(s) differ; first at %s+%ld: reference %02X, port %02X, initial %02X",
                      (unsigned long)differing, block ? block->label : "guard",
                      block ? (long)(first - block->offset) : (long)first,
                      g_reference[first], g_arena[first], g_pristine[first]);
            failed = 1;
        }
    }
    detail[sizeof(detail) - 1] = 0;
    if (failed) {
        if (stats->failures < g_max_print)
            printf("FAIL %s case=%d seed=0x%08lX: %s\n", function, case_index, (unsigned long)seed, detail);
        ++stats->failures;
    }
    return !failed;
}

// ---------------------------------------------------------------- main

static void usage()
{
    printf("usage: asm_port_harness [--only FUNCTION]... [--cases N] [--seed N] [--max-print N]\n"
           "                        [--inf32-data FILE] [--list]\n");
}

int main(int argc, char** argv)
{
    int i;
    int list = 0;
    g_options.seed = 0x5EED1998UL;
    g_options.inf32_data = "inf32_streams.bin";
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--only") && i + 1 < argc) {
            if (g_only_count < 32) g_only[g_only_count++] = argv[++i];
        } else if (!strcmp(argv[i], "--cases") && i + 1 < argc) {
            g_options.cases = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            g_options.seed = strtoul(argv[++i], 0, 0);
        } else if (!strcmp(argv[i], "--max-print") && i + 1 < argc) {
            g_max_print = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--inf32-data") && i + 1 < argc) {
            g_options.inf32_data = argv[++i];
        } else if (!strcmp(argv[i], "--list")) {
            list = 1;
        } else {
            usage();
            return 3;
        }
    }
    if (list) {
        for (i = 0; i < kFunctionCount; ++i) printf("%s\n", kFunctions[i]);
        return 0;
    }
    for (i = 0; i < g_only_count; ++i) function_index(g_only[i]);

    g_arena = (u8*)malloc(kArenaSize);
    g_pristine = (u8*)malloc(kArenaSize);
    g_reference = (u8*)malloc(kArenaSize);
    g_records[0] = (RecordSet*)malloc(sizeof(RecordSet));
    g_records[1] = (RecordSet*)malloc(sizeof(RecordSet));
    if (!g_arena || !g_pristine || !g_reference || !g_records[0] || !g_records[1]) {
        printf("FATAL out of memory\n");
        return 3;
    }
    memset(g_arena, 0, kArenaSize);

    suite_asm_draw();
    suite_drawarea();
    suite_inf32();

    {
        int failures = 0, ran = 0;
        for (i = 0; i < kFunctionCount; ++i) {
            if (!wants_function(kFunctions[i])) continue;
            printf("SUMMARY %s cases=%d failures=%d skipped=%d\n", kFunctions[i], g_stats[i].cases,
                   g_stats[i].failures, g_stats[i].skipped);
            failures += g_stats[i].failures;
            ran += g_stats[i].cases;
        }
        printf("TOTAL cases=%d failures=%d\n", ran, failures);
        return failures ? 1 : (ran ? 0 : 4);
    }
}
