// C++ port of src/Inf32.asm: the resumable DEFLATE decoder from compress.lib.
//
// int Inf32BufSize(void)
//     Size of the caller-allocated decoder state. The caller zero-fills it
//     (rge_fio.cpp uses calloc), and a zeroed state starts a new stream.
// int Inf32Decode(input, input_offset, *input_size, output, output_offset,
//                 *output_size, state, final_input)
//     Decodes from input+input_offset into output+output_offset. On return
//     *input_size holds the bytes consumed and *output_size the bytes
//     produced. Returns 0 when suspended (input ran out, or the output filled
//     up), -1 at the end of the stream, and -2 on invalid data. With
//     final_input set, reading past the input yields 0xFF bytes instead of
//     suspending.
//
// The assembly suspends anywhere it reads a byte or flushes its window, by
// saving its own stack into the state buffer. This port is an explicit state
// machine instead. Each input step reads everything it needs before it
// changes any state; when the input runs out in the middle of a step, the
// step is undone, the unread input is kept in the state, and the step runs
// again on the next call. Seen from the caller this matches the assembly: it
// consumes all input before suspending, and produces output only when the
// window flushes.
//
// Observable behavior follows the assembly exactly, including:
//   - the bit reader (DL/DH/CH registers), whose byte fetches set how many
//     input bytes are consumed at the end of the stream;
//   - the 32 KB window, which a match may overrun by up to 257 bytes and is
//     flushed whole, overrun included;
//   - the table builder, which accepts complete codes and codes with at most
//     one symbol, and decodes empty table slots as symbol 0;
//   - literal/length symbols 286 and 287 (99 extra bits, base 0);
//   - on invalid data, *input_size reports the whole input, not the bytes
//     consumed.
// One path cannot be ported: distance symbols 30 and 31 (reachable only in a
// fixed-code block) index past the distance tables in the assembly and
// read code addresses as the distance base. The port treats them as invalid
// data and sets Inf32_port_address_dependent so the harness can tell.
//
// tests/asm_port/ compares this file with the assembly; see
// tools/asm_port_harness.py.
#include <string.h>

typedef unsigned char u8;
typedef unsigned long u32;

extern "C" int Inf32_port_address_dependent = 0;

namespace {

enum {
    kWindowSize = 0x8000,
    kWindowRoom = 0x8200, // the window plus room for one overrunning match
    kStateSize = 0xAD90,  // the assembly's state size, kept for the caller
    kPendingSize = 64,
};

enum Step {
    STEP_START = 0, // a zero-filled state starts here
    STEP_BLOCK,
    STEP_STORED_HEADER,
    STEP_STORED_BYTE,
    STEP_STORED_TAIL,
    STEP_DYNAMIC_HEADER,
    STEP_DYNAMIC_LENGTHS,
    STEP_BUILD_CODES,
    STEP_CODES,
    STEP_BLOCK_DONE,
    STEP_FLUSH,
    STEP_FLUSH_RESUME,
    STEP_END,
    STEP_ERROR,
    STEP_ERROR_REPORT,
};

enum Result { RESULT_SUSPENDED = 0, RESULT_END = -1, RESULT_INVALID = -2 };

const u32 length_extra[31] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                              3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0, 0x63, 0x63};
const u32 length_base[31] = {3,  4,  5,  6,  7,  8,  9,  10,  11,  13,  15,  17,  19,  23,  27,   31,
                             35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 0, 0};
const u32 distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
const u32 distance_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                               193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
// Code-length code lengths are read in this order.
const u8 length_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// One Huffman code. Symbols up to 8 bits long decode from `table`, indexed
// by the next 8 input bits. Longer codes continue in `tree` one bit at a time.
// Slots hold symbol*4 (a leaf) or a negative odd number (a tree node whose two
// children are at ~node and ~node+1).
struct Code {
    int count;
    int* lengths;
    int* codes;
    int* table;
    int* tree;
};

struct BitReader {
    u32 edx; // DL: bits of the current byte, DH: the next byte
    u8 ch;   // bits left in DL
};

struct State {
    u8 window[kWindowRoom];
    int step;
    BitReader bits;
    // Window positions, as offsets into window[].
    long position;     // next write
    long previous_end; // where the window ended at the last flush
    // Flush progress.
    long flush_left;
    long flush_source;
    int flush_return;
    // Block state.
    u32 final_block;
    long stored_left;
    u32 literal_count;  // HLIT
    u32 distance_count; // HDIST
    long lengths_left;
    long lengths_done;
    int code_lengths_tmp[0x11E + 0x1E];
    int literal_lengths[0x120], literal_codes[0x120], literal_table[256], literal_tree[2 * 0x120];
    int distance_lengths[0x20], distance_codes[0x20], distance_table[256], distance_tree[2 * 0x20];
    int length_lengths[0x13], length_codes[0x13], length_table[256], length_tree[2 * 0x13];
    // Input read during an unfinished step, read again when it restarts.
    u8 pending[kPendingSize];
    int pending_used;
    int pending_count;
};

typedef char check_state_size[sizeof(State) <= kStateSize ? 1 : -1];

Code literal_code(State* s)
{
    Code code = {0x120, s->literal_lengths, s->literal_codes, s->literal_table, s->literal_tree};
    return code;
}

Code distance_code(State* s)
{
    Code code = {0x20, s->distance_lengths, s->distance_codes, s->distance_table, s->distance_tree};
    return code;
}

Code length_code(State* s)
{
    Code code = {0x13, s->length_lengths, s->length_codes, s->length_table, s->length_tree};
    return code;
}

// Per-call context.
struct Call {
    State* s;
    const u8* input;
    long input_left;
    long input_given;
    u8* output;
    long output_left;
    long output_given;
    int final_input;
    int failed; // the current step ran out of input
};

// ---------------------------------------------------------------- bit reader

// next_byte: load DH. Pending bytes come first, then the call's input, then
// 0xFF when the input is final. Otherwise the step fails.
void next_byte(Call* c)
{
    State* s = c->s;
    u32 byte;
    if (s->pending_used < s->pending_count) {
        byte = s->pending[s->pending_used++];
    } else if (c->input_left) {
        byte = *c->input++;
        --c->input_left;
    } else if (c->final_input) {
        byte = 0xFF;
    } else {
        c->failed = 1;
        byte = 0;
    }
    s->bits.edx = (s->bits.edx & 0xFFFF00FFUL) | (byte << 8);
}

void shift(Call* c, u8 count)
{
    c->s->bits.edx >>= (count & 31);
}

// Discard `count` bits (count <= 8).
void drop_bits(Call* c, u8 count)
{
    BitReader* b = &c->s->bits;
    u8 old = b->ch;
    b->ch = (u8)(b->ch - count);
    if (old > count) {
        shift(c, count);
        return;
    }
    shift(c, old);
    next_byte(c);
    count = (u8)-b->ch;
    b->ch = (u8)(b->ch + 8);
    shift(c, count);
}

// Consume the eight bits in DL.
void refill_bits(Call* c)
{
    BitReader* b = &c->s->bits;
    shift(c, b->ch);
    next_byte(c);
    shift(c, (u8)(8 - b->ch));
}

// Read `count` bits. `high` supplies EAX's upper half, which the assembly
// leaves unchanged on reads of more than 8 bits.
u32 get_bits(Call* c, u8 count, u32 high)
{
    BitReader* b = &c->s->bits;
    if (count <= 8) {
        u32 value = (((u32)1 << (count & 31)) - 1) & b->edx;
        drop_bits(c, count);
        return value;
    } else {
        u8 low_byte = (u8)b->edx;
        u8 first = b->ch;            // bits taken from DL
        u8 rest = (u8)(count - 8);   // bits taken after the full byte
        u8 high_byte, old, shift_count;
        shift(c, first);
        next_byte(c);
        shift(c, (u8)(8 - first));
        b->ch = first;
        shift_count = (u8)(rest & 31);
        high_byte = (u8)((shift_count >= 8 ? 0 : (u8)(1 << shift_count)) - 1);
        high_byte &= (u8)b->edx;
        old = b->ch;
        b->ch = (u8)(b->ch - rest);
        if (old > rest) {
            shift(c, rest);
        } else {
            shift(c, (u8)(rest + b->ch));
            next_byte(c);
            shift(c, (u8)-b->ch);
            b->ch = (u8)(b->ch + 8);
        }
        return (high & 0xFFFF0000UL) | ((u32)high_byte << 8) | low_byte;
    }
}

u32 decode_symbol(Call* c, const Code* code)
{
    BitReader* b = &c->s->bits;
    int entry = code->table[b->edx & 0xFF];
    if (entry >= 0) {
        // The assembly reads the low byte of lengths[symbol].
        drop_bits(c, (u8)code->lengths[entry >> 2]);
        return (u32)(entry >> 2);
    }
    refill_bits(c);
    for (;;) {
        int index = ~entry;
        b->ch = (u8)(b->ch - 1);
        if ((signed char)b->ch < 0) {
            next_byte(c);
            b->ch = 7;
        }
        index += (int)(b->edx & 1);
        b->edx >>= 1;
        entry = code->tree[index];
        if (entry >= 0) return (u32)(entry >> 2);
    }
}

// ---------------------------------------------------------------- tables

// build_decode_table: returns 0 for an over-subscribed code, or an
// incomplete one with more than one symbol.
int build_code(const Code* code)
{
    long counts[17];
    long next[17];
    long total, used, i, node;
    memset(counts, 0, sizeof(counts));
    for (i = 0; i < code->count; ++i) ++counts[code->lengths[i]];
    next[1] = 0;
    total = 0;
    for (i = 1; i <= 15; ++i) {
        total = (total + counts[i]) * 2;
        next[i + 1] = total;
    }
    if (total != 0x10000) {
        used = 0;
        for (i = 1; i <= 16; ++i) used += counts[i];
        if ((unsigned long)used > 1) return 0;
    }
    for (i = 0; i < code->count; ++i) {
        long length = code->lengths[i];
        code->codes[i] = length ? (int)next[length]++ : 0;
    }
    memset(code->table, 0, 256 * sizeof(int));
    memset(code->tree, 0, 2 * code->count * sizeof(int));
    node = -1;
    for (i = 0; i < code->count; ++i) {
        long length = code->lengths[i];
        unsigned long bits = (unsigned long)code->codes[i], reversed = 0;
        long k;
        if (!length) continue;
        for (k = 0; k < length; ++k) {
            reversed = (reversed << 1) | (bits & 1);
            bits >>= 1;
        }
        if (length <= 8) {
            for (k = (long)reversed; k < 256; k += 1L << length) code->table[k] = (int)(i * 4);
        } else {
            int* slot = &code->table[reversed & 0xFF];
            u8 path = (u8)(reversed >> 8);
            int entry = *slot;
            long levels;
            if (!entry) {
                *slot = (int)node;
                entry = (int)node;
                node -= 2;
            }
            for (levels = length - 9; levels > 0; --levels) {
                int index = ~entry + (path & 1);
                path >>= 1;
                entry = code->tree[index];
                if (!entry) {
                    code->tree[index] = (int)node;
                    entry = (int)node;
                    node -= 2;
                }
            }
            code->tree[~entry + (path & 1)] = (int)(i * 4);
        }
    }
    return 1;
}

void set_fixed_lengths(State* s)
{
    int i;
    for (i = 0; i < 0x90; ++i) s->literal_lengths[i] = 8;
    for (; i < 0x100; ++i) s->literal_lengths[i] = 9;
    for (; i < 0x118; ++i) s->literal_lengths[i] = 7;
    for (; i < 0x120; ++i) s->literal_lengths[i] = 8;
    for (i = 0; i < 0x20; ++i) s->distance_lengths[i] = 5;
}

// ---------------------------------------------------------------- steps

void begin_flush(State* s, int return_step)
{
    s->previous_end = s->position;
    s->flush_left = s->position;
    s->flush_source = 0;
    s->flush_return = return_step;
    s->step = STEP_FLUSH;
}

// Window full after a write: flush, then continue at `next`.
void after_write(State* s, int next)
{
    if (s->position >= kWindowSize)
        begin_flush(s, next);
    else
        s->step = next;
}

void copy_match(State* s, long length, long distance)
{
    u8* w = s->window;
    long source = s->position - distance;
    long i;
    if (source < 0) {
        long before = -source; // bytes that come from the previous window
        long old = s->previous_end - before;
        if (before >= length) {
            for (i = 0; i < length; ++i) w[s->position++] = w[old + i];
            return;
        }
        for (i = 0; i < before; ++i) w[s->position++] = w[old + i];
        length -= before;
        source = 0;
    }
    for (i = 0; i < length; ++i) w[s->position++] = w[source + i];
}

// Run one input step. Returns 0 when it ran out of input.
int run_input_step(Call* c)
{
    State* s = c->s;
    switch (s->step) {
    case STEP_START: {
        s->bits.edx = 0;
        next_byte(c);
        s->bits.edx = (s->bits.edx & 0xFFFFFF00UL) | ((s->bits.edx >> 8) & 0xFF);
        next_byte(c);
        if (c->failed) return 0;
        s->bits.ch = 8;
        s->position = 0;
        s->previous_end = kWindowSize;
        s->step = STEP_BLOCK;
        return 1;
    }
    case STEP_BLOCK: {
        u32 last = get_bits(c, 1, 0);
        u32 type = get_bits(c, 2, last);
        if (c->failed) return 0;
        s->final_block = last;
        if (type == 0) {
            s->step = STEP_STORED_HEADER;
        } else if (type == 1) {
            set_fixed_lengths(s);
            s->step = STEP_BUILD_CODES;
        } else if (type == 2) {
            s->step = STEP_DYNAMIC_HEADER;
        } else {
            s->step = STEP_ERROR;
        }
        return 1;
    }
    case STEP_STORED_HEADER: {
        u32 length, check;
        if (s->bits.ch != 8) drop_bits(c, s->bits.ch);
        length = s->bits.edx & 0xFFFF;
        next_byte(c);
        check = (s->bits.edx >> 8) & 0xFF;
        next_byte(c);
        check |= s->bits.edx & 0xFF00;
        if (c->failed) return 0;
        if ((~check & 0xFFFF) != length) {
            s->step = STEP_ERROR;
            return 1;
        }
        s->stored_left = (long)length;
        s->step = length ? STEP_STORED_BYTE : STEP_STORED_TAIL;
        return 1;
    }
    case STEP_STORED_BYTE: {
        next_byte(c);
        if (c->failed) return 0;
        s->window[s->position++] = (u8)(s->bits.edx >> 8);
        --s->stored_left;
        after_write(s, s->stored_left ? STEP_STORED_BYTE : STEP_STORED_TAIL);
        return 1;
    }
    case STEP_STORED_TAIL: {
        s->bits.edx = 0;
        next_byte(c);
        s->bits.edx |= (s->bits.edx >> 8) & 0xFF;
        next_byte(c);
        if (c->failed) return 0;
        s->bits.ch = 8;
        s->step = STEP_BLOCK_DONE;
        return 1;
    }
    case STEP_DYNAMIC_HEADER: {
        u32 code_count, i;
        Code code;
        memset(s->literal_lengths, 0, sizeof(s->literal_lengths));
        memset(s->distance_lengths, 0, sizeof(s->distance_lengths));
        memset(s->length_lengths, 0, sizeof(s->length_lengths));
        s->literal_count = get_bits(c, 5, 0) + 0x101;
        if (c->failed) return 0;
        if (s->literal_count > 0x11E) {
            s->step = STEP_ERROR;
            return 1;
        }
        s->distance_count = get_bits(c, 5, 0) + 1;
        if (c->failed) return 0;
        if (s->distance_count > 0x1E) {
            s->step = STEP_ERROR;
            return 1;
        }
        code_count = get_bits(c, 4, 0) + 4;
        for (i = 0; i < code_count; ++i) s->length_lengths[length_order[i]] = (int)get_bits(c, 3, 0);
        if (c->failed) return 0;
        code = length_code(s);
        if (!build_code(&code)) {
            s->step = STEP_ERROR;
            return 1;
        }
        s->lengths_left = (long)(s->literal_count + s->distance_count);
        s->lengths_done = 0;
        s->step = STEP_DYNAMIC_LENGTHS;
        return 1;
    }
    case STEP_DYNAMIC_LENGTHS: {
        int entry = s->length_table[s->bits.edx & 0xFF];
        u32 symbol, repeat, value, i;
        drop_bits(c, (u8)s->length_lengths[entry >> 2]);
        symbol = (u32)(entry >> 2) & 0xFF;
        if (symbol <= 15) {
            if (c->failed) return 0;
            s->code_lengths_tmp[s->lengths_done++] = (int)symbol;
            --s->lengths_left;
        } else {
            if (symbol == 16) {
                repeat = get_bits(c, 2, 0) + 3;
                // At the first length the assembly reads the word before the
                // array: the last slot of the (never used) code-length tree.
                value = s->lengths_done ? (u32)s->code_lengths_tmp[s->lengths_done - 1] : 0;
            } else if (symbol == 17) {
                repeat = get_bits(c, 3, 0) + 3;
                value = 0;
            } else {
                repeat = get_bits(c, 7, 0) + 11;
                value = 0;
            }
            if (c->failed) return 0;
            if (repeat > (u32)s->lengths_left) {
                s->step = STEP_ERROR;
                return 1;
            }
            for (i = 0; i < repeat; ++i) s->code_lengths_tmp[s->lengths_done++] = (int)value;
            s->lengths_left -= (long)repeat;
        }
        if (!s->lengths_left) {
            for (i = 0; i < s->literal_count; ++i) s->literal_lengths[i] = s->code_lengths_tmp[i];
            for (i = 0; i < s->distance_count; ++i)
                s->distance_lengths[i] = s->code_lengths_tmp[s->literal_count + i];
            s->step = STEP_BUILD_CODES;
        }
        return 1;
    }
    case STEP_CODES: {
        Code literal = literal_code(s), distance_table = distance_code(s);
        u32 symbol = decode_symbol(c, &literal);
        u32 length, distance_symbol, distance;
        if (c->failed) return 0;
        if (symbol < 0x100) {
            s->window[s->position++] = (u8)symbol;
            after_write(s, STEP_CODES);
            return 1;
        }
        if (!(symbol & 0xFF)) {
            s->step = STEP_BLOCK_DONE;
            return 1;
        }
        length = get_bits(c, (u8)length_extra[symbol - 0x101], symbol) + length_base[symbol - 0x101];
        if (c->failed) return 0;
        if (length > 0x102) {
            s->step = STEP_ERROR;
            return 1;
        }
        distance_symbol = decode_symbol(c, &distance_table);
        if (c->failed) return 0;
        if (distance_symbol >= 30) {
            Inf32_port_address_dependent = 1;
            s->step = STEP_ERROR;
            return 1;
        }
        distance = get_bits(c, (u8)distance_extra[distance_symbol], distance_symbol) + distance_base[distance_symbol];
        if (c->failed) return 0;
        copy_match(s, (long)length, (long)distance);
        after_write(s, STEP_CODES);
        return 1;
    }
    }
    return 1;
}

void store_counts(Call* c, long* input_size, long* output_size)
{
    *input_size = c->input_given - c->input_left;
    *output_size = c->output_given - c->output_left;
}

} // namespace

extern "C" int __cdecl Inf32BufSize()
{
    return kStateSize;
}

extern "C" int __cdecl Inf32Decode(
    void* input, int input_offset, void* input_size, void* output, int output_offset, void* output_size, void* state,
    int final_input
)
{
    Call call;
    Call* c = &call;
    State* s = (State*)state;
    long* in_size = (long*)input_size;
    long* out_size = (long*)output_size;
    c->s = s;
    c->input = (const u8*)input + input_offset;
    c->input_given = *in_size;
    c->input_left = *in_size;
    c->output = (u8*)output + output_offset;
    c->output_given = *out_size;
    c->output_left = *out_size;
    c->final_input = final_input;

    for (;;) {
        switch (s->step) {
        case STEP_BUILD_CODES: {
            Code literal = literal_code(s), distance = distance_code(s);
            if (!build_code(&literal) || !build_code(&distance))
                s->step = STEP_ERROR;
            else
                s->step = STEP_CODES;
            continue;
        }
        case STEP_BLOCK_DONE:
            if (s->final_block == 1)
                begin_flush(s, STEP_END);
            else
                s->step = STEP_BLOCK;
            continue;
        case STEP_ERROR:
            begin_flush(s, STEP_ERROR_REPORT);
            continue;
        case STEP_FLUSH:
        case STEP_FLUSH_RESUME:
            // flush_window copies at least once, and suspends whenever the
            // output is full afterwards, even with nothing left to copy.
            if (s->step == STEP_FLUSH_RESUME && !s->flush_left) {
                s->position = 0;
                s->step = s->flush_return;
                continue;
            }
            for (;;) {
                long count = s->flush_left < c->output_left ? s->flush_left : c->output_left;
                memcpy(c->output, s->window + s->flush_source, count);
                c->output += count;
                c->output_left -= count;
                s->flush_source += count;
                s->flush_left -= count;
                if (!c->output_left) {
                    s->step = STEP_FLUSH_RESUME;
                    store_counts(c, in_size, out_size);
                    return RESULT_SUSPENDED;
                }
                if (!s->flush_left) break;
            }
            s->position = 0;
            s->step = s->flush_return;
            continue;
        case STEP_END:
            store_counts(c, in_size, out_size);
            return RESULT_END;
        case STEP_ERROR_REPORT:
            *in_size = c->input_given;
            *out_size = c->output_given - c->output_left;
            return RESULT_INVALID;
        default: {
            BitReader saved_bits = s->bits;
            const u8* saved_input = c->input;
            long saved_left = c->input_left;
            int saved_pending = s->pending_used;
            c->failed = 0;
            if (run_input_step(c)) continue;
            // Out of input: undo the step and keep everything it read.
            s->bits = saved_bits;
            c->input = saved_input;
            c->input_left = saved_left;
            {
                int keep = s->pending_count - saved_pending;
                memmove(s->pending, s->pending + saved_pending, keep);
                memcpy(s->pending + keep, c->input, c->input_left);
                s->pending_count = keep + (int)c->input_left;
                s->pending_used = 0;
                c->input += c->input_left;
                c->input_left = 0;
            }
            store_counts(c, in_size, out_size);
            return RESULT_SUSPENDED;
        }
        }
    }
}
