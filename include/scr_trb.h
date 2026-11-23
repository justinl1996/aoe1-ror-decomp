#ifndef AOE1_SCR_TRB_H
#define AOE1_SCR_TRB_H

#include "Pnl_scr.h"

class TPicturePanel;
class TTextPanel;
class TButtonPanel;

class TribeSelectTribeScreen : public TScreenPanel {
  public:
    TPicturePanel* background;
    TTextPanel* title;
    TTextPanel* tribeText;
    TButtonPanel* tribeButton[9];
    TButtonPanel* okButton;
    TButtonPanel* cancelButton;
    char** screenText;
    int numLines;

    virtual ~TribeSelectTribeScreen();
    TribeSelectTribeScreen();

  protected:
    virtual long action(TPanel* fromPanel, long actionIn, uintptr_t a1, uintptr_t a2);
    void fillTribeText(int t);
    char* tribeName(int i);
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char scr_trb_screen_size_check[(sizeof(TribeSelectTribeScreen) == 0x4B8) ? 1 : -1];
#endif

#endif
