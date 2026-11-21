// The game's text, read from the string tables of language.dll and
// languagex.dll (workflow/docs/sdl_port_tasks.md). The game used to load
// the DLLs with LoadLibraryA and read them with LoadStringA; this reads the
// same RT_STRING resources from the file itself, so it needs no Windows
// loader.
//
// A PE file's resources are a three-level directory (type, name, language).
// RT_STRING entries are blocks of 16 strings: block n holds the ids
// (n - 1) * 16 to n * 16 - 1, each a 16-bit length and that many UTF-16
// characters. LoadStringA converted them to the ANSI code page; the game is
// English, so this converts to Windows-1252.

#include "../../include/surface.h"
#include "sdl_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct StringTable {
    unsigned char* file;
    size_t size;
    // The RT_STRING blocks: block id, and the block's data in file.
    int block_count;
    unsigned short* block_ids;
    const unsigned char** blocks;
    unsigned long* block_sizes;
};

static unsigned short read16(const unsigned char* p) {
    return (unsigned short) (p[0] | (p[1] << 8));
}

static unsigned long read32(const unsigned char* p) {
    return (unsigned long) p[0] | ((unsigned long) p[1] << 8) | ((unsigned long) p[2] << 16) | ((unsigned long) p[3] << 24);
}

static unsigned char* read_file(const char* path, size_t* size) {
    FILE* file = fopen(path, "rb");
    if (file == 0)
        return 0;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    unsigned char* data = length > 0 ? (unsigned char*) malloc(length) : 0;
    if (data != 0 && fread(data, 1, length, file) != (size_t) length) {
        free(data);
        data = 0;
    }
    fclose(file);
    *size = data != 0 ? (size_t) length : 0;
    return data;
}

// Maps a relative virtual address to a file offset through the section
// table; returns 0 (no data) when it is outside every section.
static const unsigned char* rva_to_data(StringTable* table, unsigned long sections, int section_count,
                                        unsigned long rva, unsigned long length) {
    for (int i = 0; i < section_count; ++i) {
        const unsigned char* section = table->file + sections + i * 40;
        if (sections + i * 40 + 40 > table->size)
            return 0;
        unsigned long virtual_size = read32(section + 8);
        unsigned long virtual_address = read32(section + 12);
        unsigned long raw_size = read32(section + 16);
        unsigned long raw_offset = read32(section + 20);
        unsigned long extent = virtual_size > raw_size ? virtual_size : raw_size;
        if (rva >= virtual_address && rva + length <= virtual_address + extent) {
            unsigned long offset = raw_offset + (rva - virtual_address);
            if (offset + length > table->size)
                return 0;
            return table->file + offset;
        }
    }
    return 0;
}

// Collects the RT_STRING blocks. Returns 0 if the file is not a PE file
// with resources.
static int parse_strings(StringTable* table) {
    const unsigned char* file = table->file;
    if (table->size < 0x40 || file[0] != 'M' || file[1] != 'Z')
        return 0;
    unsigned long pe = read32(file + 0x3C);
    if (pe + 24 > table->size || memcmp(file + pe, "PE\0\0", 4) != 0)
        return 0;
    int section_count = read16(file + pe + 6);
    unsigned long optional_size = read16(file + pe + 20);
    unsigned long optional = pe + 24;
    if (optional + optional_size > table->size || read16(file + optional) != 0x10B) // PE32
        return 0;
    if (read32(file + optional + 92) <= 2) // NumberOfRvaAndSizes
        return 0;
    unsigned long resource_rva = read32(file + optional + 96 + 2 * 8);
    unsigned long resource_size = read32(file + optional + 96 + 2 * 8 + 4);
    unsigned long sections = optional + optional_size;
    const unsigned char* root = rva_to_data(table, sections, section_count, resource_rva, resource_size);
    if (root == 0 || resource_size < 16)
        return 0;
    unsigned long base = (unsigned long) (root - file);
    unsigned long limit = base + resource_size;

#define DIRECTORY_OK(offset) ((offset) + 16 <= resource_size)
    // Level 1: the type RT_STRING (6).
    int named = read16(root + 12);
    int ids = read16(root + 14);
    unsigned long type_dir = 0;
    for (int i = 0; i < named + ids; ++i) {
        const unsigned char* entry = root + 16 + i * 8;
        if (base + 16 + i * 8 + 8 > limit)
            return 0;
        unsigned long name = read32(entry);
        unsigned long target = read32(entry + 4);
        if ((name & 0x80000000) == 0 && name == 6 && (target & 0x80000000) != 0) {
            type_dir = target & 0x7FFFFFFF;
            break;
        }
    }
    if (type_dir == 0 || !DIRECTORY_OK(type_dir))
        return 0;
    // Level 2: one entry per block.
    const unsigned char* blocks = root + type_dir;
    int block_entries = read16(blocks + 12) + read16(blocks + 14);
    table->block_ids = (unsigned short*) calloc(block_entries + 1, sizeof(unsigned short));
    table->blocks = (const unsigned char**) calloc(block_entries + 1, sizeof(unsigned char*));
    table->block_sizes = (unsigned long*) calloc(block_entries + 1, sizeof(unsigned long));
    if (table->block_ids == 0 || table->blocks == 0 || table->block_sizes == 0)
        return 0;
    for (int i = 0; i < block_entries; ++i) {
        if (type_dir + 16 + i * 8 + 8 > resource_size)
            break;
        const unsigned char* entry = blocks + 16 + i * 8;
        unsigned long name = read32(entry);
        unsigned long target = read32(entry + 4);
        if ((name & 0x80000000) != 0 || (target & 0x80000000) == 0)
            continue;
        // Level 3: languages. US English if present, else the first.
        unsigned long language_dir = target & 0x7FFFFFFF;
        if (!DIRECTORY_OK(language_dir))
            continue;
        const unsigned char* languages = root + language_dir;
        int language_count = read16(languages + 12) + read16(languages + 14);
        unsigned long data_entry = 0;
        for (int j = 0; j < language_count; ++j) {
            if (language_dir + 16 + j * 8 + 8 > resource_size)
                break;
            const unsigned char* language = languages + 16 + j * 8;
            unsigned long offset = read32(language + 4);
            if ((offset & 0x80000000) != 0)
                continue;
            if (data_entry == 0 || read32(language) == 0x409)
                data_entry = offset;
        }
        if (data_entry == 0 || data_entry + 16 > resource_size)
            continue;
        unsigned long data_rva = read32(root + data_entry);
        unsigned long data_size = read32(root + data_entry + 4);
        const unsigned char* data = rva_to_data(table, sections, section_count, data_rva, data_size);
        if (data == 0)
            continue;
        table->block_ids[table->block_count] = (unsigned short) name;
        table->blocks[table->block_count] = data;
        table->block_sizes[table->block_count] = data_size;
        ++table->block_count;
    }
#undef DIRECTORY_OK
    return 1;
}

static void table_free(StringTable* table) {
    free(table->file);
    free(table->block_ids);
    free(table->blocks);
    free(table->block_sizes);
    free(table);
}

// Windows-1252 for the UTF-16 characters 0x80-0x9F stands for.
static const unsigned short cp1252_high[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

static char to_cp1252(unsigned short c) {
    if (c < 0x80 || (c >= 0xA0 && c < 0x100))
        return (char) c;
    for (int i = 0; i < 32; ++i)
        if (cp1252_high[i] == c && c != 0)
            return (char) (0x80 + i);
    return '?';
}

extern "C" void* surface_string_table_open(const char* name) {
    if (name == 0 || *name == 0)
        return 0;
    size_t size = 0;
    // LoadLibraryA looked in the executable's folder, then the current one.
    char path[MAX_PATH];
    char* base = SDL_GetBasePath();
    SDL_snprintf(path, sizeof(path), "%s%s", base != 0 ? base : "", name);
    SDL_free(base);
    unsigned char* file = read_file(path, &size);
    if (file == 0)
        file = read_file(name, &size);
    if (file == 0)
        return 0;
    StringTable* table = (StringTable*) calloc(1, sizeof(StringTable));
    if (table == 0) {
        free(file);
        return 0;
    }
    table->file = file;
    table->size = size;
    if (parse_strings(table) == 0) {
        table_free(table);
        return 0;
    }
    return table;
}

extern "C" void surface_string_table_close(void* handle) {
    if (handle != 0)
        table_free((StringTable*) handle);
}

extern "C" int surface_string_table_load(void* handle, unsigned int id, char* buffer, int max_length) {
    if (buffer == 0 || max_length <= 0)
        return 0;
    buffer[0] = 0;
    StringTable* table = (StringTable*) handle;
    if (table == 0)
        return 0;
    unsigned short block_id = (unsigned short) ((id >> 4) + 1);
    for (int i = 0; i < table->block_count; ++i) {
        if (table->block_ids[i] != block_id)
            continue;
        const unsigned char* p = table->blocks[i];
        const unsigned char* end = p + table->block_sizes[i];
        for (unsigned int k = 0; k < (id & 15); ++k) {
            if (p + 2 > end)
                return 0;
            p += 2 + read16(p) * 2;
        }
        if (p + 2 > end)
            return 0;
        int length = read16(p);
        p += 2;
        if (p + length * 2 > end)
            return 0;
        int count = length < max_length - 1 ? length : max_length - 1;
        for (int c = 0; c < count; ++c)
            buffer[c] = to_cp1252(read16(p + c * 2));
        buffer[count] = 0;
        return count;
    }
    return 0;
}
