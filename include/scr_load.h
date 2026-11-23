#ifndef AOE1_SCR_LOAD_H
#define AOE1_SCR_LOAD_H

#include "Pnl_scr.h"

class TTextPanel;
class TListPanel;
class TScrollBarPanel;
class TButtonPanel;

class TribeLoadSavedGameScreen : public TScreenPanel {
  public:
    TTextPanel* screenTitle;
    TTextPanel* listTitle;
    TListPanel* list;
    TScrollBarPanel* scrollbar;
    TButtonPanel* okButton;
    TButtonPanel* cancelButton;
    TButtonPanel* deleteButton;

    TribeLoadSavedGameScreen();
    virtual ~TribeLoadSavedGameScreen();
    virtual long handle_idle();
    virtual long action(TPanel* fromPanel, long actionIn, uintptr_t a1, uintptr_t a2);

  protected:
    void fillList();
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char scr_load_screen_size_check[(sizeof(TribeLoadSavedGameScreen) == 0x494) ? 1 : -1];
#endif

#endif
