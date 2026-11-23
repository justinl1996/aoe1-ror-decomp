#ifndef AOE1_SCR_INFO_H
#define AOE1_SCR_INFO_H

#include "Pnl_scr.h"

class TRIBE_Screen_Info : public TScreenPanel {
  public:
    int auto_close;
    unsigned long timer;
    unsigned long last_time;

    TRIBE_Screen_Info(char* scr_name_in, char* info_file_in, long info_id_in, unsigned long timer_in);
    virtual ~TRIBE_Screen_Info();
    virtual long handle_idle();
    virtual long key_down_action(long key, short count, int alt_key, int ctrl_key, int shift_key);
    virtual long mouse_left_down_action(long x, long y, int ctrl_key, int shift_key);
    virtual long action(TPanel* fromPanel, long actionIn, uintptr_t a1, uintptr_t a2);
    void close_screen();
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char scr_info_screen_size_check[(sizeof(TRIBE_Screen_Info) == 0x484) ? 1 : -1];
#endif

#endif
