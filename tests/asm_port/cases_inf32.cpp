// Cases for Inf32BufSize and Inf32Decode from src/Inf32.asm.
//
// Streams come from a fixture file that tools/asm_port_harness.py writes
// with Python's zlib (raw DEFLATE). Each case decodes one stream through a
// sequence of calls whose input and output chunk sizes, offsets, and final
// flags come from the case seed, so suspension and resumption are exercised
// at many points. Corrupted variants exercise the invalid-data paths.
#include "harness.h"
#include "reference.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" int __cdecl Inf32BufSize();
extern "C" int __cdecl Inf32Decode(
    void* input, int input_offset, void* input_size, void* output, int output_offset, void* output_size, void* state,
    int final_input
);
extern "C" int Inf32_port_address_dependent;

struct Stream {
    u32 kind;
    u32 compressed_size;
    u32 raw_size;
    const u8* compressed;
    const u8* raw;
};

static Stream* g_streams;
static int g_stream_count;

static int load_streams(const char* path)
{
    FILE* file = fopen(path, "rb");
    long size;
    u8* data;
    u32 count, i, offset;
    if (!file) return 0;
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    data = (u8*)malloc(size);
    if (!data || fread(data, 1, size, file) != (size_t)size) {
        fclose(file);
        return 0;
    }
    fclose(file);
    if (size < 8 || memcmp(data, "INF1", 4)) return 0;
    memcpy(&count, data + 4, 4);
    g_streams = (Stream*)malloc(sizeof(Stream) * (count ? count : 1));
    offset = 8;
    for (i = 0; i < count; ++i) {
        Stream* s = &g_streams[i];
        memcpy(&s->kind, data + offset, 4);
        memcpy(&s->compressed_size, data + offset + 4, 4);
        memcpy(&s->raw_size, data + offset + 8, 4);
        offset += 12;
        s->compressed = data + offset;
        offset += s->compressed_size;
        s->raw = data + offset;
        offset += s->raw_size;
        if (offset > (u32)size) return 0;
    }
    g_stream_count = (int)count;
    return 1;
}

// ---------------------------------------------------------------- BufSize

static void buf_size_body(void*, int port)
{
    record("size", port ? Inf32BufSize() : REF_Inf32BufSize());
}

// ---------------------------------------------------------------- Decode

enum { kStateBytes = 0xAD90, kMaxCalls = 6000 };

struct DecodeCase {
    u8* input;
    u32 input_size;
    u8* output;
    u32 output_space;
    u8* state;
    u32 seed;
    int game_style; // rge_fio.cpp: final=1 and 64 KB output, input by offset
    const Stream* stream;
    int check_raw;
};

static int g_sanity_reported;

static int call_decode(int port, void* input, int input_offset, long* in_size, void* output, int output_offset,
                       long* out_size, void* state, int final_input)
{
    if (port) return Inf32Decode(input, input_offset, in_size, output, output_offset, out_size, state, final_input);
    return REF_Inf32Decode(input, input_offset, in_size, output, output_offset, out_size, state, final_input);
}

static int pick_chunk(Rng* rng, int big)
{
    switch (rng_range(rng, 0, 9)) {
    case 0: return rng_range(rng, 0, 3);
    case 1: return rng_range(rng, 1, 16);
    case 2: return rng_range(rng, 1, 300);
    case 3: return 0x8000;
    case 4: return rng_range(rng, 0x7F00, 0x8200);
    case 5: return 0x10000;
    default: return big ? rng_range(rng, 1, 0x30000) : rng_range(rng, 1, 5000);
    }
}

static void decode_body(void* context, int port)
{
    DecodeCase* c = (DecodeCase*)context;
    Rng rng;
    u32 consumed = 0, produced = 0;
    int calls, idle = 0, result = 0;
    int input_style = 0; // 0 pointer advances, 1 offset advances
    if (port) Inf32_port_address_dependent = 0;
    rng_seed(&rng, c->seed);
    input_style = c->game_style ? 1 : rng_range(&rng, 0, 1);
    for (calls = 0; calls < kMaxCalls; ++calls) {
        long in_size, out_size;
        u32 in_left = c->input_size - consumed;
        u32 out_left = c->output_space - produced;
        int final_input;
        if (c->game_style) {
            in_size = (long)in_left;
            out_size = out_left < 0x10000 ? (long)out_left : 0x10000;
            final_input = 1;
        } else {
            u32 in_chunk = (u32)pick_chunk(&rng, 1), out_chunk = (u32)pick_chunk(&rng, 1);
            in_size = (long)(in_chunk < in_left ? in_chunk : in_left);
            out_size = (long)(out_chunk < out_left ? out_chunk : out_left);
            // Final once the input is all offered, most of the time; while
            // idle, eventually always, so a stalled decoder sees 0xFF fill.
            final_input = (u32)in_size == in_left && (rng_chance(&rng, 70) || idle > 1);
        }
        {
            long given_in = in_size, given_out = out_size;
            if (input_style)
                result = call_decode(port, c->input, (int)consumed, &in_size, c->output, (int)produced, &out_size,
                                     c->state, final_input);
            else
                result = call_decode(port, c->input + consumed, 0, &in_size, c->output + produced, 0, &out_size,
                                     c->state, final_input);
            record("call.final", final_input);
            record("call.in_given", given_in);
            record("call.out_given", given_out);
        }
        record("call.result", result);
        record("call.in_size", in_size);
        record("call.out_size", out_size);
        if (in_size < 0 || out_size < 0 || (u32)in_size > c->input_size - consumed ||
            (u32)out_size > c->output_space - produced) {
            record("bad_counts", 1);
            break;
        }
        consumed += (u32)in_size;
        produced += (u32)out_size;
        if (result != 0) break;
        if (produced >= c->output_space) break;
        idle = (in_size == 0 && out_size == 0) ? idle + 1 : 0;
        if (idle > 8) break;
    }
    record("calls", calls);
    record("consumed", (long)consumed);
    record("produced", (long)produced);
    if (port && Inf32_port_address_dependent)
        skip_case("distance symbol 30/31: the assembly reads code addresses as the distance base");
    if (!port && c->check_raw && result == -1 &&
        (produced != c->stream->raw_size || memcmp(c->output, c->stream->raw, produced))) {
        if (g_sanity_reported++ < 3)
            printf("NOTE Inf32Decode reference output differs from zlib's input for stream kind %lu (%lu bytes)\n",
                   (unsigned long)c->stream->kind, (unsigned long)c->stream->raw_size);
    }
}

static void setup_decode(DecodeCase* c, Rng* rng, const Stream* stream, int corrupt)
{
    u32 slack;
    c->stream = stream;
    c->input_size = stream->compressed_size;
    c->input = (u8*)arena_alloc(c->input_size + 16, 4, "input");
    memcpy(c->input, stream->compressed, stream->compressed_size);
    rng_fill(rng, c->input + c->input_size, 16);
    // Kind 100: hand-built blocks with unusual codes and no reference text.
    c->check_raw = !corrupt && stream->kind != 100;
    if (corrupt) {
        int mode = rng_range(rng, 0, 3), flips, i;
        if (mode == 0 && c->input_size > 1) {
            c->input_size = (u32)rng_range(rng, 1, (int)c->input_size - 1); // truncate
        } else if (mode == 1) {
            c->input_size += (u32)rng_range(rng, 1, 16); // trailing garbage
        } else if (c->input_size) {
            flips = rng_range(rng, 1, 3);
            for (i = 0; i < flips; ++i) {
                u32 bit = rng_next(rng) % (c->input_size * 8);
                c->input[bit / 8] ^= (u8)(1 << (bit % 8));
            }
        }
    }
    slack = (corrupt || stream->kind == 100) ? 0x30000 : (u32)rng_range(rng, 0, 0x100);
    c->output_space = stream->raw_size + slack;
    c->output = (u8*)arena_alloc(c->output_space + 64, 4, "output");
    rng_fill(rng, c->output, c->output_space + 64);
    c->state = (u8*)arena_alloc_opaque(kStateBytes, 8, "decoder_state");
    memset(c->state, 0, kStateBytes);
    c->seed = rng_next(rng);
}

void suite_inf32()
{
    int i;
    if (wants_function("Inf32BufSize")) {
        arena_reset();
        run_case("Inf32BufSize", 0, 0, buf_size_body, 0);
    }
    if (!wants_function("Inf32Decode")) return;
    if (!load_streams(suite_options()->inf32_data) || !g_stream_count) {
        printf("FATAL cannot read Inf32 streams from %s\n", suite_options()->inf32_data);
        exit(3);
    }
    {
        int count = cases_for(3 * g_stream_count);
        for (i = 0; i < count; ++i) {
            u32 seed = suite_options()->seed ^ 0x4400000UL ^ (u32)i * 2654435761UL;
            Rng rng;
            DecodeCase c;
            const Stream* stream;
            int variant;
            rng_seed(&rng, seed);
            arena_reset();
            stream = &g_streams[i % g_stream_count];
            variant = (i / g_stream_count) % 3; // 0 game style, 1 random chunks, 2 corrupted
            setup_decode(&c, &rng, stream, variant == 2);
            c.game_style = variant == 0;
            run_case("Inf32Decode", i, seed, decode_body, &c);
        }
    }
}
