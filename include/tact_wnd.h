#ifndef AOE1_TACT_WND_H
#define AOE1_TACT_WND_H

#include "action.h"

class RGE_Action_Object;

class TRIBE_Action_Wonder : public RGE_Action {
  public:
    TRIBE_Action_Wonder(int infile, RGE_Action_Object* obj_in);
    TRIBE_Action_Wonder(RGE_Action_Object* obj_in);

    virtual ~TRIBE_Action_Wonder();
    virtual void first_in_stack(unsigned char first_time);
    virtual unsigned char update();
    virtual void set_state(unsigned char new_state);
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char tact_wnd_action_wonder_size[(sizeof(TRIBE_Action_Wonder) == 0x40) ? 1 : -1];
#endif

#endif // AOE1_TACT_WND_H
