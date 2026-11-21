#include "../include/registry.h"
#include "../include/surface.h"

#include <string.h>

// The registry calls (RegCreateKeyExA, RegQueryValueExA, RegSetValueExA)
// became the settings file's (surface_setting_get, surface_setting_set),
// which take the key with each value.

TRegistry::TRegistry(const char* Key) {
    strncpy(this->Key, Key != 0 ? Key : "", sizeof(this->Key) - 1);
    this->Key[sizeof(this->Key) - 1] = 0;
    this->Data[0] = 0;
}

TRegistry::~TRegistry() {
}

int TRegistry::RegSet(int UseUserKey, const char* lptszName, unsigned char* lpData, unsigned long dwSize) {
    long err = surface_setting_set(UseUserKey != 0, this->Key, lptszName, REG_DWORD, lpData, dwSize);
    return (err == 0);
}

int TRegistry::RegSetInt(int UseUserKey, const char* lptszName, int Value) {
    long err = surface_setting_set(UseUserKey != 0, this->Key, lptszName, REG_DWORD, (const unsigned char*) &Value, 4);
    return (err == 0);
}

int TRegistry::RegSetAscii(int UseUserKey, const char* lptszName, unsigned char* lpData, unsigned long dwSize) {
    long err = surface_setting_set(UseUserKey != 0, this->Key, lptszName, REG_SZ, lpData, dwSize);
    return (err == 0);
}

char* TRegistry::RegGetAscii(int UseUserKey, const char* lptszName) {
    unsigned long dwType = REG_SZ;
    unsigned long dwSize = sizeof(this->Data) - 1;
    long err = surface_setting_get(UseUserKey != 0, this->Key, lptszName, &dwType, (unsigned char*) this->Data, &dwSize);
    if (err != 0)
        return 0;
    this->Data[dwSize < sizeof(this->Data) ? dwSize : sizeof(this->Data) - 1] = 0;
    return this->Data;
}

long TRegistry::RegGet(int UseUserKey, const char* lptszName, unsigned char* lpData, unsigned long* lpdwDataSize) {
    unsigned long dwType;
    return surface_setting_get(UseUserKey != 0, this->Key, lptszName, &dwType, lpData, lpdwDataSize);
}

int TRegistry::RegGetInt(int UseUserKey, const char* lptszName) {
    unsigned long Size = 4;
    unsigned long dwType = REG_DWORD;
    int Data;
    long err = surface_setting_get(UseUserKey != 0, this->Key, lptszName, &dwType, (unsigned char*) &Data, &Size);
    if (err == 0 && Size == 4) {
        return Data;
    }
    return -1;
}
