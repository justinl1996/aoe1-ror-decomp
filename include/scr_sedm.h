#ifndef AOE1_SCR_SEDM_H
#define AOE1_SCR_SEDM_H

#include "Pnl_scr.h"

class TTextPanel;
class TButtonPanel;

class TRIBE_Screen_Sed_Menu : public TScreenPanel {
  public:
    TTextPanel* title;
    TButtonPanel* button[4];
    TButtonPanel* close_button;

    TRIBE_Screen_Sed_Menu();
    virtual ~TRIBE_Screen_Sed_Menu();

  protected:
    virtual long handle_idle();
    virtual long action(TPanel* fromPanel, long actionIn, uintptr_t a1, uintptr_t a2);
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char scr_sedm_screen_size_check[(sizeof(TRIBE_Screen_Sed_Menu) == 0x490) ? 1 : -1];
#endif

#endif
