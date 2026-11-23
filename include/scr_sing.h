#ifndef AOE1_SCR_SING_H
#define AOE1_SCR_SING_H

// TribeSPMenuScreen is the only class owned by this header. Its screen base
// comes from the canonical screen owner.

#include "Pnl_scr.h"

class TTextPanel;
class TButtonPanel;

class TribeSPMenuScreen : public TScreenPanel {
  public:
    TTextPanel* title;
    TButtonPanel* button[7];
    TButtonPanel* close_button;

    TribeSPMenuScreen();
    virtual ~TribeSPMenuScreen();
    virtual long handle_idle();
    virtual long action(TPanel* fromPanel, long actionIn, uintptr_t a1, uintptr_t a2);
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char scr_sing_screen_size_check[(sizeof(TribeSPMenuScreen) == 0x49C) ? 1 : -1];
#endif

#endif
