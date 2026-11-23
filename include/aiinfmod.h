#ifndef AOE1_AIINFMOD_H
#define AOE1_AIINFMOD_H

#include "aimodule.h"

class InformationAIModule : public AIModule {
  public:
    InformationAIModule(void* pW, int playerID);
    virtual ~InformationAIModule();
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char aiinfmod_information_ai_module_size[(sizeof(InformationAIModule) == 0xF0) ? 1 : -1];
#endif

#endif
