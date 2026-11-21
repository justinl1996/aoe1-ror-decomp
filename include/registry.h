#ifndef AOE1_REGISTRY_H
#define AOE1_REGISTRY_H

#include <windows.h>

// TRegistry reads and writes the game's settings under one key, for the
// current user (UseUserKey) or the machine. The values live in a settings
// file now (port/sdl/settings.cpp), with the registry's types and results.

class TRegistry {
  public:
    char Key[256];
    char Data[256];

    TRegistry(const char* Key);
    ~TRegistry();
    int RegSet(int UseUserKey, const char* lptszName, unsigned char* lpData, unsigned long dwSize);
    int RegSetInt(int UseUserKey, const char* lptszName, int Value);
    int RegSetAscii(int UseUserKey, const char* lptszName, unsigned char* lpData, unsigned long dwSize);
    char* RegGetAscii(int UseUserKey, const char* lptszName);
    long RegGet(int UseUserKey, const char* lptszName, unsigned char* lpData, unsigned long* lpdwDataSize);
    int RegGetInt(int UseUserKey, const char* lptszName);
};

#endif
