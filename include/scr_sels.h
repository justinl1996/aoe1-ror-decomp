#ifndef AOE1_SCR_SELS_H
#define AOE1_SCR_SELS_H

// TribeSelectScenarioScreen is the only class owned by this header. Its
// complete screen base comes from the canonical screen owner.
#include "Pnl_scr.h"

class TPanel;
class TTextPanel;
class TListPanel;
class TScrollBarPanel;
class TButtonPanel;

class TribeSelectScenarioScreen : public TScreenPanel {
  public:
    TTextPanel* title;
    TTextPanel* scenarioTitle;
    TTextPanel* scenarioPlayersTitle;
    long scenarioListX;
    long scenarioListY;
    long scenarioListWidth;
    long scenarioListHeight;
    TListPanel* scenarioList;
    TScrollBarPanel* scenarioScrollbar;
    int scenarioCount;
    char** scenarioMission;
    int* scenarioFixed;
    int scenariosLoaded;
    TTextPanel* missionTitle;
    long missionTextX;
    long missionTextY;
    long missionTextWidth;
    long missionTextHeight;
    TTextPanel* missionText;
    TScrollBarPanel* missionScrollbar;
    TButtonPanel* okButton;
    TButtonPanel* cancelButton;
    TButtonPanel* close_button;
    int last_scenario_line;

    TribeSelectScenarioScreen();
    virtual ~TribeSelectScenarioScreen();
    virtual long handle_idle();

  protected:
    virtual long action(TPanel* fromPanel, long actionIn, uintptr_t a1, uintptr_t a2);
    void init_vars();
    void fillScenarios();
    void getSettings();
    void fillMissionText();
    void sendSettings();
    void activatePanels();
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char scr_sels_screen_size_check[(sizeof(TribeSelectScenarioScreen) == 0x4D8) ? 1 : -1];
#endif

#endif
