// The game's settings, kept in a text file instead of the registry
// (workflow/docs/sdl_port_tasks.md). TRegistry (src/registry.cpp) reads and
// writes values here with the registry's types and results.
//
// The file is settings.ini next to the executable, in the layout of a .reg
// file:
//
//   [user\Software\Microsoft\Games\Age of Empires\1.00]
//   Screen Size=dword:1024
//   Default Age Of Empires Player Name=sz:Player
//   Some Blob=hex:01,02,03
//
// "user" stands for HKEY_CURRENT_USER and "machine" for HKEY_LOCAL_MACHINE.
// A missing file or value is not an error: the game uses its defaults. The
// file is rewritten whenever a value changes.

#include "../../include/surface.h"
#include "sdl_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Setting {
    Setting* next;
    char* section;      // "user\<key>" or "machine\<key>"
    char* name;
    unsigned long type; // REG_SZ, REG_DWORD or REG_BINARY
    unsigned char* data;
    unsigned long size;
};

static Setting* settings = 0;
static int settings_loaded = 0;

static void settings_path(char* out, size_t size) {
    char* base = port_base_path();
    SDL_snprintf(out, size, "%ssettings.ini", base != 0 ? base : "");
    SDL_free(base);
}

static char* copy_string(const char* text, size_t length) {
    char* copy = (char*) malloc(length + 1);
    if (copy != 0) {
        memcpy(copy, text, length);
        copy[length] = 0;
    }
    return copy;
}

static Setting* settings_find(const char* section, const char* name) {
    for (Setting* setting = settings; setting != 0; setting = setting->next)
        if (SDL_strcasecmp(setting->section, section) == 0 && SDL_strcasecmp(setting->name, name) == 0)
            return setting;
    return 0;
}

// Stores a copy of the value, replacing any earlier one.
static int settings_put(const char* section, const char* name, unsigned long type, const unsigned char* data,
                        unsigned long size) {
    unsigned char* copy = (unsigned char*) malloc(size + 1);
    if (copy == 0)
        return 0;
    memcpy(copy, data, size);
    copy[size] = 0;
    Setting* setting = settings_find(section, name);
    if (setting == 0) {
        setting = (Setting*) calloc(1, sizeof(Setting));
        if (setting == 0) {
            free(copy);
            return 0;
        }
        setting->section = copy_string(section, strlen(section));
        setting->name = copy_string(name, strlen(name));
        // Keep the file's order: append.
        Setting** link = &settings;
        while (*link != 0)
            link = &(*link)->next;
        *link = setting;
    } else {
        free(setting->data);
    }
    setting->type = type;
    setting->data = copy;
    setting->size = size;
    return 1;
}

static int hex_digit(int c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static void settings_load(void) {
    settings_loaded = 1;
    char path[MAX_PATH];
    settings_path(path, sizeof(path));
    FILE* file = fopen(path, "rb");
    if (file == 0)
        return;
    char section[512] = "";
    char line[2048];
    while (fgets(line, sizeof(line), file) != 0) {
        size_t length = strlen(line);
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = 0;
        if (line[0] == '[') {
            char* end = strrchr(line, ']');
            if (end != 0) {
                *end = 0;
                SDL_strlcpy(section, line + 1, sizeof(section));
            }
            continue;
        }
        if (line[0] == ';' || line[0] == '#' || section[0] == 0)
            continue;
        char* equals = strchr(line, '=');
        if (equals == 0)
            continue;
        *equals = 0;
        char* value = equals + 1;
        if (strncmp(value, "dword:", 6) == 0) {
            // A DWORD is 4 bytes; unsigned long is 8 on 64-bit Linux.
            unsigned int number = (unsigned int) strtoul(value + 6, 0, 10);
            settings_put(section, line, REG_DWORD, (const unsigned char*) &number, 4);
        } else if (strncmp(value, "sz:", 3) == 0) {
            settings_put(section, line, REG_SZ, (const unsigned char*) (value + 3), (unsigned long) strlen(value + 3) + 1);
        } else if (strncmp(value, "hex:", 4) == 0) {
            unsigned char bytes[1024];
            unsigned long count = 0;
            const char* p = value + 4;
            while (*p != 0 && count < sizeof(bytes)) {
                int high = hex_digit(p[0]);
                int low = high >= 0 ? hex_digit(p[1]) : -1;
                if (low < 0)
                    break;
                bytes[count++] = (unsigned char) (high * 16 + low);
                p += 2;
                if (*p == ',')
                    ++p;
            }
            settings_put(section, line, REG_BINARY, bytes, count);
        }
    }
    fclose(file);
}

static void settings_save(void) {
    char path[MAX_PATH];
    settings_path(path, sizeof(path));
    FILE* file = fopen(path, "wb");
    if (file == 0)
        return;
    // One block per section, in the order sections first appear.
    int first_block = 1;
    for (Setting* head = settings; head != 0; head = head->next) {
        int seen = 0;
        for (Setting* earlier = settings; earlier != head; earlier = earlier->next)
            if (strcmp(earlier->section, head->section) == 0)
                seen = 1;
        if (seen)
            continue;
        fprintf(file, "%s[%s]\n", first_block ? "" : "\n", head->section);
        first_block = 0;
        for (Setting* setting = head; setting != 0; setting = setting->next) {
            if (strcmp(setting->section, head->section) != 0)
                continue;
            if (setting->type == REG_DWORD && setting->size == 4) {
                unsigned int number;  // 4 bytes, as the DWORD it holds
                memcpy(&number, setting->data, 4);
                fprintf(file, "%s=dword:%u\n", setting->name, number);
            } else if (setting->type == REG_SZ && memchr(setting->data, '\n', setting->size) == 0) {
                fprintf(file, "%s=sz:%s\n", setting->name, (const char*) setting->data);
            } else {
                fprintf(file, "%s=hex:", setting->name);
                for (unsigned long i = 0; i < setting->size; ++i)
                    fprintf(file, i == 0 ? "%02x" : ",%02x", setting->data[i]);
                fprintf(file, "\n");
            }
        }
    }
    fclose(file);
}

static void make_section(char* out, size_t size, int user, const char* key) {
    SDL_snprintf(out, size, "%s\\%s", user != 0 ? "user" : "machine", key != 0 ? key : "");
}

extern "C" long surface_setting_get(int user, const char* key, const char* name, unsigned long* type,
                                    unsigned char* data, unsigned long* size) {
    if (settings_loaded == 0)
        settings_load();
    char section[512];
    make_section(section, sizeof(section), user, key);
    Setting* setting = settings_find(section, name != 0 ? name : "");
    if (setting == 0)
        return ERROR_FILE_NOT_FOUND;
    if (type != 0)
        *type = setting->type;
    if (size == 0)
        return data == 0 ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;
    if (data != 0 && *size < setting->size) {
        *size = setting->size;
        return ERROR_MORE_DATA;
    }
    if (data != 0)
        memcpy(data, setting->data, setting->size);
    *size = setting->size;
    return ERROR_SUCCESS;
}

extern "C" long surface_setting_set(int user, const char* key, const char* name, unsigned long type,
                                    const unsigned char* data, unsigned long size) {
    if (settings_loaded == 0)
        settings_load();
    if (data == 0 && size != 0)
        return ERROR_INVALID_PARAMETER;
    char section[512];
    make_section(section, sizeof(section), user, key);
    if (settings_put(section, name != 0 ? name : "", type, data, size) == 0)
        return ERROR_OUTOFMEMORY;
    settings_save();
    return ERROR_SUCCESS;
}
