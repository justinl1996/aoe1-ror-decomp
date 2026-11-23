#ifndef AOE1_TAICRULE_H
#define AOE1_TAICRULE_H

// Compiler-facing declarations for the original taicrule.cpp module.
//
// The class inventory proves one virtual destructor slot and a 0x9C8-byte
// object.  The rating table starts at +0x04 and contains 2500 bytes.

class CombatRuleSystem {
  public:
    virtual ~CombatRuleSystem();

    unsigned char ratingValue[2500];

    CombatRuleSystem();
    int loadRules(char* rName);
    int bestUnitTypeToAttack(int tType);
    int bestUnitTypeToDefendAgainst(int tType);
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char taicrule_combat_rule_system_size[(sizeof(CombatRuleSystem) == 0x9C8) ? 1 : -1];
#endif

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char taicrule_combat_rule_system_vptr[(sizeof(void*) == 4) ? 1 : -1];
#endif

#endif
