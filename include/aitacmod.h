#ifndef AOE1_AITACMOD_H
#define AOE1_AITACMOD_H

#include "aimodule.h"

class TacticalAIModule : public AIModule {
  public:
    TacticalAIModule(void* pW, int playerID);
    virtual ~TacticalAIModule();
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char aitacmod_tactical_ai_module_size[(sizeof(TacticalAIModule) == 0xF0) ? 1 : -1];
#endif

#endif
