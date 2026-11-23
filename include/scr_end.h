#ifndef AOE1_SCR_END_H
#define AOE1_SCR_END_H

#include "Pnl_scr.h"

class TPicturePanel;
class TTextPanel;
class TButtonPanel;

class TribeEndScreen : public TScreenPanel {
  public:
    TPicturePanel* background;
    TTextPanel* title;
    TTextPanel* text;
    TButtonPanel* okButton;

    TribeEndScreen(char* titleText, char** textText, int textLineCount);
    virtual ~TribeEndScreen();

  protected:
    virtual long action(TPanel* fromPanel, long actionIn, uintptr_t a1, uintptr_t a2);
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char tribe_end_screen_size_check[(sizeof(TribeEndScreen) == 0x488) ? 1 : -1];
#endif

#endif
