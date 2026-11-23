# HD Resolution Support Implementation Plan (`720p`, `900p`, `1080p`)

## Goal
Replace the legacy 4:3 resolutions (`640×480`, `800×600`, `1024×768`, `1280×1024`) in **Age of Empires: The Rise of Rome** with three widescreen HD resolutions — **`1280×720`**, **`1600×900`**, and **`1920×1080`** — for both the **Linux (`make linux`)** and **Windows (`make clang`)** SDL2 ports, with no VC5 backwards-compatibility requirement.

Design choices:
- **Supported resolutions:** `1280×720` (default), `1600×900`, `1920×1080`.
- **Menu layout:** Full-bleed 16:9 HD background art with **centered 4:3 controls** (no horizontal widget stretching).
- **Target builds:** Linux (`make linux`) and Windows (`make clang`) — edit `src/` and `port/` directly without VC5 guards.

---

## Asset Mapping (`../aoe_hd_sprites/data2/inter_up.drs`)

Verified from `data2/inter_up.drs`:

| Resolution | Menu `.sin` BINA Base (`+1,2,3,4,7,8,10,11`) | Menu Background SLPs (`scr1..scr10`) | In-Game HUD SLPs (5 cultures: `0=Egy, 1=Grk, 2=Bab, 3=EA, 4=Rom`) |
|---|---|---|---|
| **`1280×720`** | `52140` | `54201..54209` (`1280×720`) | `54250..54254` (frame 0: `1280×20`, frame 1: `1280×126`) |
| **`1600×900`** | `52260` | `54801..54809` (`1600×900`) | `54850..54854` (frame 0: `1600×20`, frame 1: `1600×126`) |
| **`1920×1080`** | `52300` | `55001..55009` (`1920×1080`) | `55050..55054` (frame 0: `1920×20`, frame 1: `1920×126`) |

Notes:
- Each `52xxx` BINA is a standard `TEasy_Panel` screen-info text file whose `background3_files` entry points to the matching widescreen SLP.
- Each HUD SLP matches the original 2-frame layout (`W×20` top bar + `W×126` bottom bar) and exact 5-culture index order (`0..4`).
- `RESFILE_locate_resource` (`src/Res_file.cpp`) uses **first-opened-wins** order. Open `inter_up.drs` **after** the retail `Interfac.drs` files so all new `52xxx`/`54xxx`/`55xxx` IDs resolve while retail button/palette IDs stay intact. Check file existence with `_stat` before opening (since `RESFILE_open_new_resource_file` uses `OPEN_ALWAYS`).

---

## Proposed Changes

### 1. Shared Resolution Table
- **`include/hd_resolutions.h`** *(new)*
  - Define `AOE_ResolutionPreset` (`width`, `height`, `menu_bina_base`, `hud_slp_base`, `label`) and `AOE_HD_RESOLUTIONS[3]` (`1280×720`, `1600×900`, `1920×1080`).
  - Provide `aoe_find_resolution(wid, hgt)` and `aoe_resolution_or_default(wid, hgt)`.

### 2. Display Modes, Startup, Settings & Options UI
- **`port/sdl/ddraw_memory.cpp`**
  - Update `MemDirectDraw::EnumDisplayModes` to advertise `{1280, 720}`, `{1600, 900}`, `{1920, 1080}` (8-bit and 16-bit), retiring `{640, 480}`, `{800, 600}`, `{1024, 768}`, `{1280, 1024}`.
- **`include/drawarea.h` & `src/drawarea.cpp`**
  - Replace `ModeAvail640` / `ModeAvail800` / `ModeAvail1024` with `ModeAvail1280_720`, `ModeAvail1600_900`, `ModeAvail1920_1080`.
  - Update `TDrawSystem::TDrawSystem`, `CheckAvailModes`, `CheckAvailModesCallback`, and `IsModeAvail` to check `(1280, 720)`, `(1600, 900)`, `(1920, 1080)` (accepting `hgt_in == 0` or exact `hgt_in`).
- **`src/main.cpp`**
  - Change default startup resolution from `800×600` to `1280×720`.
- **`src/basegame.cpp`**
  - `setup()`: Map saved `"Screen Size"` (`1280`, `1600`, `1920`) to `1280×720`, `1600×900`, `1920×1080`; ignore legacy values (`640`/`800`/`1024`) so old `settings.ini` upgrades cleanly to `1280×720`.
  - CLI flags: Replace `640`/`800`/`1024`/`1280` with `720P`/`1280` (`1280×720`), `900P`/`1600` (`1600×900`), `1080P`/`1920` (`1920×1080`).
  - `setup_video_system()`: Fallback loop over `AOE_HD_RESOLUTIONS`.
- **`src/dlg_conf.cpp`**
  - Replace the 3 legacy screen-size radio buttons with `AOE_HD_RESOLUTIONS[0..2]` (`"1280 x 720"`, `"1600 x 900"`, `"1920 x 1080"`), widening the label width (`100` → `115`).
  - Check availability with `IsModeAvail(preset.width, preset.height, 8)` and apply the selected preset on OK.
- **`src/Dib.cpp`**
  - Enlarge `static long SrcXTbl[0x500]` (currently 1280 entries) to `4096` so scaled DIB blits at 1600px / 1920px cannot overflow.

### 3. Loading `inter_up.drs`
- **`src/tribegam.cpp`**
  - In `TRIBE_Game::setup()`, check if `inter_up.drs` exists in `this->prog_info->resource_dir` (or `data\`) via `_stat`, and open it **after** the retail `Interfac.drs` files.
  - Close `inter_up.drs` in `TRIBE_Game::~TRIBE_Game`.

### 4. Menus: HD Widescreen Backgrounds & Centered 4:3 Controls
- **`include/panel_ez.h` & `src/Panel_ez.cpp`**
  - **HD Background BINA Remap (`setup`)**:
    - When `this->info_id` is in `50051..50061` (`scr1`–`scr10`), try `RESFILE_load(RES_TYPE_BINARY, preset.menu_bina_base + (this->info_id - 50050), ...)`. Fall back to `this->info_id` if not present (e.g., `scr5` editor, `credit1`).
    - Since all widths are `>= 1280`, `background3_files` is chosen and draws the full `1280×720` / `1600×900` / `1920×1080` widescreen background.
  - **Centered 4:3 Control Scaling**:
    - Add `scale_ideal_x`, `scale_ideal_y`, `scale_ideal_w`, `scale_ideal_h` helpers on `TEasy_Panel`:
      - When `this->pnl_wid * this->ideal_height > this->pnl_hgt * this->ideal_width`:
        - `virt_w = (this->pnl_hgt * this->ideal_width) / this->ideal_height` (`960` at 720p, `1200` at 900p, `1440` at 1080p)
        - `offset_x = (this->pnl_wid - virt_w) / 2` (`160` at 720p, `200` at 900p, `240` at 1080p)
        - `scale_ideal_x(x) = offset_x + (x * virt_w) / this->ideal_width`
        - `scale_ideal_w(w) = (w * virt_w) / this->ideal_width`
      - Otherwise (e.g., `TDialogPanel` popups where `ideal == pnl`, or `scr_main.cpp`'s full-width copyright footer), `offset_x = 0` and `virt_w = pnl_wid`.
    - Use these helpers in all `create_*` methods and `position_panel`.
- **Screen-Specific Updates**:
  - **`src/Scr_sels.cpp` & `src/scr_set.cpp`**: Use `scale_ideal_w(scenarioListWidth - 60)` for `set_second_column_pos`.
  - **`src/tpnl_tml.cpp`**: Apply the same centered 4:3 virtual width when creating the endgame timeline panel.
  - **`src/scr_vc.cpp`**: Remove `<=640`/`<=800` branches; use `set_ideal_size(1024, 768)` + `position_panel` for controls, and `scale_ideal_x`/`scale_ideal_y` for `vc_back_bmp`.
  - **`src/scr_trb.cpp`**: Remove `<800`/`<1024` branches and use `bgA1024`.

### 5. In-Game Screen (`TRIBE_Screen_Game`)
- **`src/scr_game.cpp` (`handle_size`)**
  - Replace the 3 hardcoded `1024`/`800`/`640` branches with a single parametric layout using `W = this->pnl_wid`, `H = this->pnl_hgt`:
    - `tool_box`: `(0, 20, W, 30)`
    - `main_view`: `(0, 20, W, H - 146)` (or `(0, 50, W, H - 176)` when `tool_box->active`)
    - `inven_panel`: `(0, 0, 270, 20)`
    - `age_panel`: `(270, 2, W - 532, 16)`
    - `fps_panel`: `(2, 2, 280, 16)`
    - `pop_panel`: centered horizontally at `(W - 160) / 2`
    - `button_panel[12..14]` (top-right Help/Chat/Menu): `x = W - 252`, `W - 180`, `W - 72` at `y = 0`
    - `object_panel` (bottom-left): `(5, H - 119, 127, 114)`
    - `button_panel[0..11]` (2×6 command grid): `x = 136 + col * 54`, `y = H - 118 + row * 58`, size `54×54`
    - `map_view` (minimap, bottom-right): `(W - 229, H - 120, 220, 114)`
    - `button_panel[15..16]` (minimap buttons): `(W - 35, H - 36, 30, 30)` and `(W - 35, H - 118, 30, 30)`
    - `more_cancel_pic = this->button_cmd_pic`, `more_frame = 0xB`, `cancel_frame = unselect_frame = 0xA`
- **`src/scr_game.cpp` (`set_player`)**
  - Load `game_pic_id = aoe_resolution_or_default(this->pnl_wid, this->pnl_hgt).hud_slp_base + culture` (`54250+c` / `54850+c` / `55050+c`).
- **`src/scr_game.cpp` (`draw`)**
  - Draw top bar frame 0 at `(0, 0)` and bottom bar frame 1 at `(0, this->pnl_hgt - 126)`.

---

## Verification Plan
1. Build both target ports:
   - `make linux` (32-bit native Linux SDL2 build)
   - `make clang` (Windows `clang-cl`/`lld-link` SDL2 build)
2. Run `make test` to verify tool/harness tests still pass.
3. Runtime check (with `data` + `data2` including `inter_up.drs`):
   - Verify menus show full-bleed 16:9 HD backgrounds with centered 4:3 controls at `1280×720`.
   - Verify Options dialog switches live between `1280×720`, `1600×900`, and `1920×1080`.
   - Verify in-game HUD bars, minimap, command buttons, and viewport align cleanly across all 3 resolutions and all 5 cultures.
